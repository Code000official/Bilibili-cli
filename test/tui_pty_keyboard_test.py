#!/usr/bin/env python3
"""TUI 键盘逻辑 pty 端到端测试（Linux）。

用伪终端驱动真实的 ./bili，验证：
  1. 进入备用屏与 init 转义序列
  2. 输入框字符输入与退格
  3. CSI 参数化转义序列被完整消费（F5=\\x1b[15~、Ctrl+Right=\\x1b[1;5C），
     残留字节不再泄漏进输入框
  4. 方向键/Home/End/PgUp/PgDn/Shift-Tab/Esc 不崩溃
  5. SIGWINCH 触发全量重绘
  6. Ctrl-C 退出并恢复主屏

运行: python3 test/tui_pty_keyboard_test.py [./bili]
"""
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./bili"


def strip_ansi(b: bytes) -> bytes:
    b = re.sub(rb"\x1b\[[0-9;?<>=]*[a-zA-Z]", b"", b)  # CSI
    b = re.sub(rb"\x1b[()][0-9A-Za-z]", b"", b)        # 字符集
    b = re.sub(rb"\x1b[a-zA-Z0-9=><#]", b"", b)        # 其余两字节转义
    return b


def main() -> int:
    mfd, sfd = pty.openpty()
    fcntl.ioctl(sfd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        try:
            fcntl.ioctl(sfd, termios.TIOCSCTTY, 0)
        except OSError:
            pass
        os.dup2(sfd, 0)
        os.dup2(sfd, 1)
        os.dup2(sfd, 2)
        os.close(mfd)
        if sfd > 2:
            os.close(sfd)
        os.execv(BIN, [BIN])
    os.close(sfd)

    buf = bytearray()
    failures = []

    def read_avail(timeout: float) -> None:
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                return
            r, _, _ = select.select([mfd], [], [], left)
            if not r:
                return
            try:
                data = os.read(mfd, 65536)
            except OSError:
                return
            if not data:
                return
            buf.extend(data)

    def check(name: str, cond: bool) -> None:
        print(("PASS " if cond else "FAIL ") + name)
        if not cond:
            failures.append(name)

    def screen_since(mark: int) -> bytes:
        return strip_ansi(bytes(buf[mark:]))

    def full_redraw() -> None:
        """改 pty 尺寸触发 SIGWINCH 全量重绘，让当前整屏进入输出流"""
        fcntl.ioctl(mfd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 25, 80, 0, 0))
        read_avail(0.8)
        fcntl.ioctl(mfd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 80, 0, 0))
        read_avail(0.8)

    read_avail(1.0)
    check("进入备用屏", b"\x1b[?1049h" in bytes(buf))
    check("init 关闭 kitty 键盘协议", b"\x1b[>0u" in bytes(buf))

    # 输入 abc
    mark = len(buf)
    os.write(mfd, b"abc")
    read_avail(0.5)
    full_redraw()
    check("输入 abc", "abc".encode() in screen_since(mark))

    # 退格 -> ab
    mark = len(buf)
    os.write(mfd, b"\x7f")
    read_avail(0.5)
    full_redraw()
    s = screen_since(mark)
    check("退格后为 ab", "ab".encode() in s and "abc".encode() not in s)

    # F5（\x1b[15~）：'~' 不得泄漏进输入框
    mark = len(buf)
    os.write(mfd, b"\x1b[15~")
    read_avail(0.5)
    full_redraw()
    s = screen_since(mark)
    check("F5 序列不泄漏 '~'", b"~" not in s)
    check("F5 不破坏输入 ab",
          "ab".encode() in s and "abc".encode() not in s)

    # Ctrl+Right（\x1b[1;5C）：'5'/'C' 不得泄漏
    mark = len(buf)
    os.write(mfd, b"\x1b[1;5C")
    read_avail(0.5)
    full_redraw()
    s = screen_since(mark)
    check("Ctrl+Right 序列不泄漏 '5'", "5".encode() not in s)
    check("Ctrl+Right 不破坏输入 ab", "ab".encode() in s)

    # 方向键/Home/End/PgUp/PgDn/Shift-Tab/ESC：不崩溃、无副作用
    os.write(mfd, b"\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[5~\x1b[6~\x1b[Z\x1b")
    read_avail(0.5)

    # Ctrl-C 退出
    mark = len(buf)
    os.write(mfd, b"\x03")
    exited = False
    deadline = time.time() + 3
    while time.time() < deadline:
        wpid, _ = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            exited = True
            read_avail(0.2)
            break
        r, _, _ = select.select([mfd], [], [], 0.1)
        if r:
            try:
                data = os.read(mfd, 65536)
            except OSError:
                exited = True
                break
            if not data:
                exited = True
                break
            buf.extend(data)
    check("Ctrl-C 退出", exited)
    check("退出备用屏", b"\x1b[?1049l" in bytes(buf[mark:]))

    try:
        os.close(mfd)
    except OSError:
        pass

    print()
    if failures:
        print(f"{len(failures)} 项失败")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
