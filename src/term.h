/* term.h - 手写终端后端：raw 输入 + 单元缓冲差量重绘（零依赖，htop 式界面用） */
#ifndef BILI_CLI_TERM_H
#define BILI_CLI_TERM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,       /* 可打印字符/UTF-8 码点，cp 有效 */
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_TAB,
    KEY_ESC,
    KEY_CTRL,       /* Ctrl+字母，cp 为对应小写字母 */
    KEY_EOF,
} key_type_t;

typedef struct {
    key_type_t type;
    uint32_t cp;
} term_key_t;

/* 初始化（进入备用屏 + raw 模式），进程退出时自动恢复 */
void term_init(void);
/* 暂时离开 TUI（扫码登录等前台流程用），之后 term_resume() */
void term_suspend(void);
void term_resume(void);
void term_shutdown(void);

int  term_width(void);
int  term_height(void);
/* 自上次调用后终端尺寸是否变化（变化则内部重置缓冲） */
bool term_check_resized(void);

/* 绘制 API：写入后台缓冲，term_flush() 差量输出到终端。
 * 颜色 fg/bg：0-255 为 256 色索引，-1 为终端默认色。 */
void term_clear(int16_t fg, int16_t bg);
void term_putc(int x, int y, uint32_t cp, int16_t fg, int16_t bg);
void term_puts(int x, int y, const char *s, int16_t fg, int16_t bg);
void term_hline(int x, int y, int w, uint32_t cp, int16_t fg, int16_t bg);
void term_fill(int x, int y, int w, int h, int16_t fg, int16_t bg);
/* 帧末光标位置（输入框用）；x<0 隐藏光标 */
void term_setcursor(int x, int y);
void term_flush(void);

/* 输入轮询：timeout_ms 为超时毫秒（<0 阻塞）。返回 1 有按键，0 超时。 */
int  term_poll_key(int timeout_ms, term_key_t *k);

/* UTF-8 辅助 */
int  term_utf8_decode(const char *s, uint32_t *cp); /* 返回消耗的字节数 */
int  term_utf8_encode(uint32_t cp, char *out);      /* 返回字节数（<=4） */
int  term_char_width(uint32_t cp);                  /* 1 / 2(CJK全角) / 0(组合) */
int  term_str_width(const char *s);

#endif
