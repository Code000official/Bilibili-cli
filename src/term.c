/* term.c - 手写终端后端
 *
 * - raw 模式读取按键（POSIX: termios + 转义序列解析；Windows: 控制台输入事件）
 * - 单元缓冲（每格 字符+前景色+背景色）+ 差量刷新，避免整屏重绘闪烁
 * - 宽字符（CJK）占两格，前格存码点、后格标记为 0 不输出
 * - 尺寸变化检测（POSIX: SIGWINCH；Windows: 轮询控制台缓冲区信息）
 * - 输出走 ANSI/VT 转义（Windows 10+ 控制台与 Windows Terminal 原生支持，
 *   由 port.c 的 bili_console_init() 打开）
 */
#include "term.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_EXTENDED_FLAGS
#define ENABLE_EXTENDED_FLAGS 0x0080
#endif
#ifndef ENABLE_QUICK_EDIT_MODE
#define ENABLE_QUICK_EDIT_MODE 0x0040
#endif

#else /* !_WIN32 */

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#endif /* !_WIN32 */

/* ---------- 终端状态 ---------- */

#ifdef _WIN32
static HANDLE g_hin = NULL;         /* 控制台输入句柄 */
static DWORD g_saved_in_mode = 0;   /* 进入 raw 模式前的输入模式 */
#endif
static int g_raw_ok = 0;
static int g_started = 0;

static int g_w = 80, g_h = 24;

typedef struct {
    uint32_t cp;   /* 0 = 被前一个宽字符占用的续格，跳过输出 */
    int16_t fg, bg;
} cell_t;

static cell_t *g_cur = NULL, *g_prev = NULL; /* 当前帧 / 上一帧 */
static int g_dirty = 1;                      /* 需要整屏重绘 */
static int16_t g_fill_fg = -1, g_fill_bg = -1;

static int g_cursor_x = -1, g_cursor_y = -1;
static int g_last_out_x = -1, g_last_out_y = -1;  /* 上一个实际输出位置 */
static int g_last_cursor_x = -2, g_last_cursor_y = -2; /* 上一帧输出的光标状态 */

#ifndef _WIN32

static struct termios g_saved_termios;
static volatile sig_atomic_t g_sigwinch = 0;

static void on_sigwinch(int sig)
{
    (void)sig;
    g_sigwinch = 1;
}

#endif /* !_WIN32 */

/* ---------- 内部工具 ---------- */

#ifdef _WIN32

static void write_all(const char *s, size_t n)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || !h) {
        return;
    }
    while (n > 0) {
        DWORD chunk = n > 0x10000 ? 0x10000 : (DWORD)n;
        DWORD written = 0;
        if (!WriteFile(h, s, chunk, &written, NULL) || written == 0) {
            return;
        }
        s += written;
        n -= written;
    }
}

#else /* !_WIN32 */

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t r = write(STDOUT_FILENO, s, n);
        if (r <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        s += r;
        n -= (size_t)r;
    }
}

#endif /* !_WIN32 */

#define OUT_LIT(s)  write_all(s, sizeof(s) - 1)
#define OUT_STR(s)  write_all(s, strlen(s))

static void out_fmt(const char *fmt, int a, int b)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), fmt, a, b);
    if (n > 0) {
        write_all(buf, (size_t)n);
    }
}

#ifdef _WIN32

static void read_winsize(void)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h &&
        GetConsoleScreenBufferInfo(h, &csbi)) {
        int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (cols > 0 && rows > 0 && (cols != g_w || rows != g_h)) {
            g_w = cols;
            g_h = rows;
            g_dirty = 1;
        }
    }
}

#else /* !_WIN32 */

static void read_winsize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        if (ws.ws_col != g_w || ws.ws_row != g_h) {
            g_w = ws.ws_col;
            g_h = ws.ws_row;
            g_dirty = 1;
        }
    }
}

#endif /* !_WIN32 */

static void alloc_buffers(void)
{
    size_t n = (size_t)g_w * (size_t)g_h;
    free(g_cur);
    free(g_prev);
    g_cur = malloc(n * sizeof(cell_t));
    g_prev = malloc(n * sizeof(cell_t));
    if (!g_cur || !g_prev) {
        fprintf(stderr, "term: 内存分配失败\n");
        exit(1);
    }
    for (size_t i = 0; i < n; i++) {
        g_cur[i].cp = ' ';
        g_cur[i].fg = g_fill_fg;
        g_cur[i].bg = g_fill_bg;
        g_prev[i].cp = 0xFFFFFFFF; /* 强制首帧全绘 */
        g_prev[i].fg = -2;
        g_prev[i].bg = -2;
    }
}

#ifdef _WIN32

static void set_raw(int on)
{
    if (!g_hin) {
        return;
    }
    if (on) {
        if (!GetConsoleMode(g_hin, &g_saved_in_mode)) {
            return; /* 不是真实控制台（重定向等），TUI 不可用 */
        }
        /* 关闭行输入/回显/按键处理（Ctrl-C 转为按键事件），
         * 保留窗口输入事件用于感知尺寸变化。
         * 同时关闭 QuickEdit：conhost 下点选文本会暂停控制台写入，
         * 全屏 TUI 会表现为假死；ENABLE_EXTENDED_FLAGS 不带
         * QUICK_EDIT 位即关闭它。 */
        SetConsoleMode(g_hin, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);
        g_raw_ok = 1;
    } else {
        SetConsoleMode(g_hin, g_saved_in_mode); /* 含原 QuickEdit 状态 */
        g_raw_ok = 0;
    }
}

#else /* !_WIN32 */

static void set_raw(int on)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) {
        return;
    }
    if (on) {
        g_saved_termios = t;
        struct termios r = t;
        r.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        r.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK);
        r.c_cflag |= CS8;
        r.c_cc[VMIN] = 1;
        r.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &r);
        g_raw_ok = 1;
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_raw_ok = 0;
    }
}

#endif /* !_WIN32 */

/* ---------- 生命周期 ---------- */

void term_init(void)
{
    if (g_started) {
        return;
    }
#ifdef _WIN32
    g_hin = GetStdHandle(STD_INPUT_HANDLE);
    if (g_hin == INVALID_HANDLE_VALUE) {
        g_hin = NULL;
    }
    DWORD mode = 0;
    if (!g_hin || !GetConsoleMode(g_hin, &mode)) {
        /* stdin 不是控制台（管道/重定向），TUI 无法工作 */
        fprintf(stderr, "错误: TUI 需要真实终端（stdin 不是控制台）\n");
        exit(1);
    }
#else
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) {
        fprintf(stderr, "错误: TUI 需要真实终端（stdin 不是 tty）\n");
        exit(1);
    }
    signal(SIGWINCH, on_sigwinch);
#endif
    set_raw(1);
    OUT_LIT("\x1b[?1049h"); /* 备用屏 */
    OUT_LIT("\x1b[?25l");   /* 隐藏光标 */
    OUT_LIT("\x1b[2J");
#ifndef _WIN32
    /* 外层 shell 可能遗留以下模式，不关闭会让按键变成乱序列：
     * - kitty 键盘协议（fish 4.x 默认开启）：按键变成 \x1b[<码>u
     * - bracketed paste（zsh 常见）：粘贴被包裹成 \x1b[200~...\x1b[201~
     * - 鼠标上报：滚轮/点击变成 \x1b[M... 序列 */
    OUT_LIT("\x1b[>0u");            /* kitty 协议：压入 flags=0（禁用） */
    OUT_LIT("\x1b[?2004l");         /* 关闭 bracketed paste */
    OUT_LIT("\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"); /* 关鼠标上报 */
#endif
    g_started = 1;
    read_winsize();
    alloc_buffers();
}

void term_suspend(void)
{
    if (!g_started) {
        return;
    }
    OUT_LIT("\x1b[?25h");
    OUT_LIT("\x1b[?1049l"); /* 回主屏 */
    OUT_LIT("\x1b[0m\n");
    set_raw(0);
#ifndef _WIN32
    tcflush(STDIN_FILENO, TCIFLUSH);
#else
    if (g_hin) {
        FlushConsoleInputBuffer(g_hin);
    }
#endif
}

void term_resume(void)
{
    if (!g_started) {
        return;
    }
    set_raw(1);
    OUT_LIT("\x1b[?1049h");
    OUT_LIT("\x1b[?25l");
    g_dirty = 1;
    read_winsize();
    alloc_buffers();
}

void term_shutdown(void)
{
    if (!g_started) {
        return;
    }
    OUT_LIT("\x1b[0m");
    OUT_LIT("\x1b[?25h");
    OUT_LIT("\x1b[?1049l");
#ifndef _WIN32
    OUT_LIT("\x1b[<u"); /* 弹出 term_init 压入的 kitty 键盘协议层，恢复原状态 */
#endif
    set_raw(0);
    g_started = 0;
}

int term_width(void)
{
    return g_w;
}

int term_height(void)
{
    return g_h;
}

#ifdef _WIN32

/* Windows 没有 SIGWINCH：尺寸检查在输入超时时周期触发 */
bool term_check_resized(void)
{
    read_winsize();
    if (g_dirty) {
        g_dirty = 0;
        alloc_buffers();
        OUT_LIT("\x1b[2J");
        return true;
    }
    return false;
}

#else /* !_WIN32 */

bool term_check_resized(void)
{
    if (g_sigwinch) {
        g_sigwinch = 0;
        read_winsize();
        if (g_dirty) {
            alloc_buffers();
            OUT_LIT("\x1b[2J");
        }
        return true;
    }
    return false;
}

#endif /* !_WIN32 */

/* ---------- 绘制 ---------- */

void term_clear(int16_t fg, int16_t bg)
{
    g_fill_fg = fg;
    g_fill_bg = bg;
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            g_cur[y * g_w + x].cp = ' ';
            g_cur[y * g_w + x].fg = fg;
            g_cur[y * g_w + x].bg = bg;
        }
    }
}

void term_putc(int x, int y, uint32_t cp, int16_t fg, int16_t bg)
{
    if (x < 0 || x >= g_w || y < 0 || y >= g_h) {
        return;
    }
    cell_t *c = &g_cur[y * g_w + x];
    c->cp = cp;
    c->fg = fg;
    c->bg = bg;
}

void term_fill(int x, int y, int w, int h, int16_t fg, int16_t bg)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            term_putc(i, j, ' ', fg, bg);
        }
    }
}

void term_hline(int x, int y, int w, uint32_t cp, int16_t fg, int16_t bg)
{
    for (int i = 0; i < w; i++) {
        term_putc(x + i, y, cp, fg, bg);
    }
}

int term_utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    int n;
    uint32_t v;
    if ((p[0] & 0xE0) == 0xC0) {
        n = 2;
        v = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        n = 3;
        v = p[0] & 0x0F;
    } else if ((p[0] & 0xF8) == 0xF0) {
        n = 4;
        v = p[0] & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i;
        }
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return n;
}

int term_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int term_char_width(uint32_t cp)
{
    if (cp == 0) {
        return 0;
    }
    /* 组合字符（近似） */
    if ((cp >= 0x0300 && cp <= 0x036F) || cp == 0x200B || cp == 0xFEFF) {
        return 0;
    }
    /* CJK / 全角（近似常用区段） */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

int term_str_width(const char *s)
{
    int w = 0;
    while (*s) {
        uint32_t cp;
        int n = term_utf8_decode(s, &cp);
        w += term_char_width(cp);
        s += n;
    }
    return w;
}

void term_puts(int x, int y, const char *s, int16_t fg, int16_t bg)
{
    if (y < 0 || y >= g_h) {
        return;
    }
    int cx = x;
    while (*s) {
        uint32_t cp;
        int n = term_utf8_decode(s, &cp);
        s += n;
        int cw = term_char_width(cp);
        if (cw == 0) {
            continue;
        }
        if (cx >= g_w) {
            break;
        }
        term_putc(cx, y, cp, fg, bg);
        cx++;
        if (cw == 2) {
            /* 宽字符的第二格标记为续格 */
            if (cx < g_w) {
                term_putc(cx, y, 0, fg, bg);
                cx++;
            }
        }
    }
}

void term_setcursor(int x, int y)
{
    g_cursor_x = x;
    g_cursor_y = y;
}

void term_flush(void)
{
    g_last_out_x = -1;
    int any_change = 0;
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            cell_t *c = &g_cur[y * g_w + x];
            cell_t *p = &g_prev[y * g_w + x];
            if (c->cp == p->cp && c->fg == p->fg && c->bg == p->bg) {
                continue;
            }
            any_change = 1;
            if (c->cp == 0) {
                /* 宽字符续格：不单独输出，只同步 prev */
                if (p->cp != c->cp) {
                    any_change = 1;
                }
                *p = *c;
                continue;
            }
            /* 定位：仅当与上一个输出位置不连续时才移动光标 */
            if (x != g_last_out_x || y != g_last_out_y) {
                out_fmt("\x1b[%d;%dH", y + 1, x + 1);
            }
            if (p->fg != c->fg || p->bg != c->bg || g_last_out_x < 0) {
                if (c->fg < 0 && c->bg < 0) {
                    OUT_LIT("\x1b[39;49m");
                } else if (c->bg < 0) {
                    out_fmt("\x1b[38;5;%dm", c->fg, 0);
                } else if (c->fg < 0) {
                    out_fmt("\x1b[48;5;%dm", c->bg, 0);
                } else {
                    out_fmt("\x1b[38;5;%d;48;5;%dm", c->fg, c->bg);
                }
            }
            char utf8[4];
            int n = term_utf8_encode(c->cp, utf8);
            write_all(utf8, (size_t)n);
            g_last_out_x = x + 1;
            g_last_out_y = y;
            *p = *c;
        }
    }
    /* 无任何变化且光标状态不变：不输出，避免周期性空刷 */
    if (!any_change &&
        g_cursor_x == g_last_cursor_x && g_cursor_y == g_last_cursor_y) {
        g_cursor_x = g_cursor_y = -1;
        return;
    }
    g_last_cursor_x = g_cursor_x;
    g_last_cursor_y = g_cursor_y;

    /* 光标 */
    if (g_cursor_x >= 0 && g_cursor_y >= 0) {
        OUT_LIT("\x1b[?25h");
        out_fmt("\x1b[%d;%dH", g_cursor_y + 1, g_cursor_x + 1);
    } else {
        OUT_LIT("\x1b[?25l");
    }
    fflush(NULL);
    g_cursor_x = g_cursor_y = -1;
}

/* ---------- 输入 ---------- */

#ifdef _WIN32

int term_poll_key(int timeout_ms, term_key_t *k)
{
    if (!g_hin) {
        return 0;
    }
    k->cp = 0;

    static WCHAR pending_hi = 0; /* UTF-16 高代理暂存 */
    for (;;) {
        /* 缓冲中已有记录：先逐条消化。
         * 必须先 Peek 确认存在再 Read：ReadConsoleInput 在缓冲为空时
         * 会无限阻塞（曾导致按键后 key-up 被消费、TUI 卡死到下一次按键）。 */
        DWORD avail = 0;
        if (GetNumberOfConsoleInputEvents(g_hin, &avail) && avail > 0) {
            INPUT_RECORD rec;
            DWORD nread = 0;
            if (!PeekConsoleInputW(g_hin, &rec, 1, &nread) || nread == 0) {
                return 0;
            }
            if (!ReadConsoleInputW(g_hin, &rec, 1, &nread) || nread == 0) {
                return 0;
            }

            if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                read_winsize(); /* SIGWINCH 替代：事件到达即感知尺寸变化 */
                continue;
            }
            if (rec.EventType != KEY_EVENT) {
                continue; /* 鼠标/焦点/菜单事件忽略 */
            }
            KEY_EVENT_RECORD *ke = &rec.Event.KeyEvent;
            if (!ke->bKeyDown) {
                continue; /* 忽略 key-up */
            }
            WORD vk = ke->wVirtualKeyCode;
            int ctrl = (ke->dwControlKeyState &
                        (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

            switch (vk) {
            case VK_UP:    k->type = KEY_UP;        return 1;
            case VK_DOWN:  k->type = KEY_DOWN;      return 1;
            case VK_LEFT:  k->type = KEY_LEFT;      return 1;
            case VK_RIGHT: k->type = KEY_RIGHT;     return 1;
            case VK_HOME:  k->type = KEY_HOME;      return 1;
            case VK_END:   k->type = KEY_END;       return 1;
            case VK_PRIOR: k->type = KEY_PGUP;      return 1;
            case VK_NEXT:  k->type = KEY_PGDN;      return 1;
            case VK_BACK:  k->type = KEY_BACKSPACE; return 1;
            case VK_TAB:
                k->type = KEY_TAB;
                k->cp = (ke->dwControlKeyState & SHIFT_PRESSED) ? 1 : 0;
                return 1; /* cp=1 表示反向 Tab，与 POSIX 版一致 */
            case VK_RETURN:
                k->type = KEY_ENTER;
                return 1;
            case VK_ESCAPE:
                k->type = KEY_ESC;
                return 1;
            default:
                break;
            }
            /* Ctrl+字母 → KEY_CTRL（cp 为小写字母），与 POSIX raw 模式一致 */
            if (ctrl && vk >= 'A' && vk <= 'Z') {
                k->type = KEY_CTRL;
                k->cp = (uint32_t)('a' + vk - 'A');
                return 1;
            }
            if (vk == VK_DELETE || vk == VK_INSERT) {
                continue; /* 未映射，忽略 */
            }

            WCHAR wc = ke->uChar.UnicodeChar;
            if (wc == 0) {
                continue; /* 纯修饰键按下 */
            }
            uint32_t cp;
            if (wc >= 0xD800 && wc <= 0xDBFF) {
                pending_hi = wc; /* 高代理，等下一个事件合成 */
                continue;
            }
            if (pending_hi) {
                if (wc >= 0xDC00 && wc <= 0xDFFF) {
                    cp = 0x10000 + ((uint32_t)(pending_hi - 0xD800) << 10) +
                         (uint32_t)(wc - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
                pending_hi = 0;
            } else if (wc >= 0xDC00 && wc <= 0xDFFF) {
                continue; /* 孤立低代理，忽略 */
            } else {
                cp = wc;
            }
            if (ctrl && cp >= 1 && cp <= 26) {
                k->type = KEY_CTRL;
                k->cp = (uint32_t)('a' + cp - 1);
                return 1;
            }
            k->type = KEY_CHAR;
            k->cp = cp;
            return 1;
        }

        /* 缓冲已空：带超时等待新事件到来 */
        DWORD r = WaitForSingleObject(g_hin, (DWORD)timeout_ms);
        if (r != WAIT_OBJECT_0) {
            read_winsize(); /* 超时：顺带检测尺寸变化（无 SIGWINCH 的替代） */
            return 0;
        }
        /* 新事件已到达，回到循环顶部消化 */
    }
}

#else /* !_WIN32 */

/* 转义序列缓冲：一次 read 可能含多字节 */
static int read_byte(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR); /* SIGWINCH 等信号打断时重试，
                                          否则转义序列会被误判为 ESC */
    if (r <= 0) {
        return -1;
    }
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        return c;
    }
    return -1;
}

int term_poll_key(int timeout_ms, term_key_t *k)
{
    int c = read_byte(timeout_ms);
    if (c < 0) {
        return 0;
    }
    k->cp = 0;

    if (c == 0x1b) {
        /* 转义序列：立即再读（数据已在缓冲） */
        int c2 = read_byte(30);
        if (c2 < 0) {
            k->type = KEY_ESC;
            return 1;
        }
        if (c2 == '[' || c2 == 'O') {
            int c3 = read_byte(30);
            if (c3 < 0) {
                k->type = KEY_ESC;
                return 1;
            }
            switch (c3) {
            case 'A': k->type = KEY_UP; return 1;
            case 'B': k->type = KEY_DOWN; return 1;
            case 'C': k->type = KEY_RIGHT; return 1;
            case 'D': k->type = KEY_LEFT; return 1;
            case 'H': k->type = KEY_HOME; return 1;
            case 'F': k->type = KEY_END; return 1;
            case 'Z': k->type = KEY_TAB; k->cp = 1; return 1; /* 反向 Tab */
            case 'M':
                /* X10 鼠标事件：后续还有 3 字节，必须一并消费，
                 * 否则残留字节会被当成普通字符输入 */
                for (int i = 0; i < 3; i++) {
                    if (read_byte(30) < 0) {
                        break;
                    }
                }
                k->type = KEY_NONE;
                return 1;
            default:
                if (c3 >= '0' && c3 <= '9') {
                    /* 参数可能是多位（F5=\x1b[15~）且可带修饰符
                     * （Ctrl+右=\x1b[1;5C 不会到 '~'，F5 类以 '~' 结束）。
                     * 必须消费到终止符，否则残留字节泄漏成乱输入。 */
                    int val = c3 - '0';
                    int c4;
                    while ((c4 = read_byte(30)) >= 0) {
                        if (c4 >= '0' && c4 <= '9') {
                            val = val * 10 + (c4 - '0');
                            continue;
                        }
                        if (c4 == '~') {
                            if (val == 5) { k->type = KEY_PGUP; return 1; }
                            if (val == 6) { k->type = KEY_PGDN; return 1; }
                            if (val == 1 || val == 7) { k->type = KEY_HOME; return 1; }
                            if (val == 4 || val == 8) { k->type = KEY_END; return 1; }
                            k->type = KEY_NONE;
                            return 1;
                        }
                        /* ';' 修饰参数等：忽略，继续消费 */
                    }
                }
                k->type = KEY_NONE;
                return 1;
            }
        }
        /* Alt+键：按 ESC 处理 */
        tcflush(STDIN_FILENO, TCIFLUSH);
        k->type = KEY_ESC;
        return 1;
    }
    if (c == '\r' || c == '\n') {
        k->type = KEY_ENTER;
        return 1;
    }
    if (c == 0x7f || c == 0x08) {
        k->type = KEY_BACKSPACE;
        return 1;
    }
    if (c == '\t') {
        k->type = KEY_TAB;
        return 1;
    }
    if (c < 0x20) {
        k->type = KEY_CTRL;
        k->cp = (uint32_t)('a' + c - 1);
        return 1;
    }
    /* UTF-8 多字节序列 */
    {
        char buf[4] = { (char)c, 0, 0, 0 };
        int extra = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        }
        for (int i = 0; i < extra; i++) {
            int cc = read_byte(30);
            if (cc < 0) {
                break;
            }
            buf[1 + i] = (char)cc;
        }
        term_utf8_decode(buf, &k->cp);
        k->type = KEY_CHAR;
        return 1;
    }
}

#endif /* !_WIN32 */
