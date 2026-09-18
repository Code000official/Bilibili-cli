/* term.c - 手写终端后端
 *
 * - termios raw 模式读取按键（方向键/功能键转义序列解析）
 * - 单元缓冲（每格 字符+前景色+背景色）+ 差量刷新，避免整屏重绘闪烁
 * - 宽字符（CJK）占两格，前格存码点、后格标记为 0 不输出
 * - SIGWINCH 检测终端尺寸变化
 */
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ---------- 终端状态 ---------- */

static struct termios g_saved_termios;
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

static volatile sig_atomic_t g_sigwinch = 0;

static void on_sigwinch(int sig)
{
    (void)sig;
    g_sigwinch = 1;
}

/* ---------- 内部工具 ---------- */

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

/* ---------- 生命周期 ---------- */

void term_init(void)
{
    if (g_started) {
        return;
    }
    signal(SIGWINCH, on_sigwinch);
    set_raw(1);
    OUT_LIT("\x1b[?1049h"); /* 备用屏 */
    OUT_LIT("\x1b[?25l");   /* 隐藏光标 */
    OUT_LIT("\x1b[2J");
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
    tcflush(STDIN_FILENO, TCIFLUSH);
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

/* 转义序列缓冲：一次 read 可能含多字节 */
static int read_byte(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, timeout_ms);
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
            default:
                if (c3 >= '0' && c3 <= '9') {
                    int c4 = read_byte(30);
                    if (c4 == '~') {
                        if (c3 == '5') { k->type = KEY_PGUP; return 1; }
                        if (c3 == '6') { k->type = KEY_PGDN; return 1; }
                        if (c3 == '1') { k->type = KEY_HOME; return 1; }
                        if (c3 == '4') { k->type = KEY_END; return 1; }
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
