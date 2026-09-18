/* util.h - 字符串缓冲、编码、文件名处理等基础工具 */
#ifndef BILI_CLI_UTIL_H
#define BILI_CLI_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* 可增长字符串缓冲 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

void sb_init(strbuf_t *sb);
void sb_append(strbuf_t *sb, const char *s);
void sb_append_len(strbuf_t *sb, const char *s, size_t n);
void sb_appendf(strbuf_t *sb, const char *fmt, ...);
void sb_free(strbuf_t *sb);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* RFC 3986 百分号编码（空格编码为 %20） */
char *url_encode(const char *s);

/* 百分号解码（不处理 '+'），返回 malloc 字符串 */
char *url_decode(const char *s);

/* 状态文件目录 $XDG_STATE_HOME/bili-cli 或 ~/.local/state/bili-cli（调用方 free） */
char *state_dir(void);

/* 将文件名中的非法字符替换为 '_'，并去掉首尾空白与结尾的 '.' */
char *sanitize_filename(const char *name);

/* 递归创建目录，已存在视为成功；返回 0 成功 */
int mkdir_p(const char *path);

/* 人类可读的大小，如 "12.3 MB"（返回静态缓冲） */
const char *human_size(uint64_t bytes);

/* 时长格式化 "3:33" / "1:02:33"（返回静态缓冲） */
const char *human_duration(long seconds);

/* key=value 对列表（WBI 签名参数用） */
typedef struct {
    char *key;
    char *val;
} kv_t;

void kv_add(kv_t **arr, size_t *n, const char *k, const char *v);
void kv_free(kv_t *arr, size_t n);

#endif
