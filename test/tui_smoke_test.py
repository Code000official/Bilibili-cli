#!/usr/bin/env python3
"""TUI pty 冒烟测试：启动 -> 首页 -> 输入BV -> 详情 -> 勾选 -> 下载 -> 完成 -> 退出"""
import pty, os, time, select, fcntl, termios, struct, signal, sys, re

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


def clean(b):
    return ANSI.sub(b"", b).decode("utf-8", "ignore")

BIN = sys.argv[1] if len(sys.argv) > 1 else "./bili"


def read_window(fd, seconds):
    """固定时长读取，不因数据到来而延长"""
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            out += d
    return out


def wait_for(fd, needle, timeout, already=b""):
    """等待输出中出现 needle（绝对超时，匹配前剥离 ANSI 码）"""
    buf = already
    end = time.time() + timeout
    while time.time() < end:
        if needle in clean(buf):
            return True, buf
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
    return needle.encode() in buf, buf


pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.chdir(os.getcwd())
    os.execv(BIN, [BIN])
    os._exit(127)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 112, 0, 0))
failures = []

try:
    out = read_window(fd, 3.5)
    txt = clean(out)
    raw = out.decode("latin1")
    print("== 首页 ==")
    ok1 = "\x1b[?1049h" in raw
    ok2 = "bili" in txt and ("未登录" in txt or "已登录" in txt)
    ok3 = "链接/BV" in txt
    print("  备用屏:", ok1, "| 标题+登录态:", ok2, "| 输入框提示:", ok3)
    if not (ok1 and ok2 and ok3):
        failures.append("首页渲染")
        print("  [debug] 首页输出片段:", txt[:500])

    def send(s, wait=0.4):
        os.write(fd, s.encode())
        return read_window(fd, wait)

    print("== Ctrl-L 应直接打开登录流程 ==")
    buf = send("\x0c", 2.5)
    ok_l, buf_l = wait_for(fd, "扫描二维码", 8, buf)
    print("  登录二维码流程:", ok_l)
    if not ok_l:
        failures.append("L 登录快捷键")
        print("  [debug]:", clean(buf_l)[-300:])
    # 登录轮询无法自动完成（需手机扫码），直接杀掉进程重新启动继续后续测试
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    os.close(fd)
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(BIN, [BIN])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 112, 0, 0))
    read_window(fd, 3.0)

    print("== Ctrl-R 刷新登录状态 ==")
    buf = send("\x12", 1.0)
    ok_r, buf_r = wait_for(fd, "已刷新", 6, buf)
    buf_r += read_window(fd, 1.0)
    print("  刷新提示:", ok_r)
    if not ok_r:
        failures.append("R 刷新")
        print("  [debug]:", clean(buf_r)[-300:])

    print("== 输入 BV 号并回车 ==")
    for ch in "BV1GJ411x7h7":
        send(ch, 0.03)
    send("\r", 0.5)
    ok, buf = wait_for(fd, "Never Gonna", 20)
    buf += read_window(fd, 2.0)
    txt = clean(buf)
    ok_p1 = "P1" in txt
    ok_q = ("480P" in txt) or ("1080P" in txt)
    print("  详情标题:", ok, "| P1 列表:", ok_p1, "| 清晰度:", ok_q)
    if not (ok and ok_p1 and ok_q):
        failures.append("详情页")
        print("  [debug] 详情输出片段:", txt[:500])

    print("== 空格勾选 + Ctrl-D 入队 ==")
    buf = send(" ", 0.4) + send("\x04", 1.5)
    ok, buf2 = wait_for(fd, "已加入", 8, buf)
    buf = buf + buf2
    buf += read_window(fd, 1.5)
    print("  入队提示:", ok)
    if not ok:
        failures.append("入队")
        print("  [debug] 入队片段:", clean(buf)[-500:])

    print("== 等待下载+混流完成 ==")
    ok, buf = wait_for(fd, "完成 ", 150, buf)
    txt = clean(buf)
    ok_done = ("完成" in txt) and (("MB" in txt) or ("KB" in txt))
    print("  任务完成:", ok_done)
    if not ok_done:
        failures.append("下载完成")
        print("  [debug] 队列区片段:", txt[-500:])

    print("== Esc 返回首页，Ctrl-C 退出 ==")
    send("\x1b", 0.5)
    send("\x03", 1.0)
    status = None
    try:
        for _ in range(10):
            p, status = os.waitpid(pid, os.WNOHANG)
            if p == pid:
                break
            time.sleep(0.3)
    except ChildProcessError:
        status = "already reaped"
    print("  进程退出:", status)
finally:
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.close(fd)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

print()
if failures:
    print("结果: FAIL —", ",".join(failures))
    sys.exit(1)
print("结果: ALL PASS")
