/* util.c - 字符串缓冲、编码、文件名处理等基础工具 */
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <errno.h>

void sb_init(strbuf_t *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->data = xmalloc(sb->cap);
    sb->data[0] = '\0';
}

static void sb_reserve(strbuf_t *sb, size_t extra)
{
    if (sb->len + extra + 1 > sb->cap) {
        while (sb->len + extra + 1 > sb->cap) {
            sb->cap *= 2;
        }
        sb->data = xrealloc(sb->data, sb->cap);
    }
}

void sb_append_len(strbuf_t *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(strbuf_t *sb, const char *s)
{
    sb_append_len(sb, s, strlen(s));
}

void sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(sb, (size_t)need);
    vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
}

void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "错误: 内存分配失败\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "错误: 内存分配失败\n");
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *url_encode(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = xmalloc(n * 3 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0x0f];
        }
    }
    out[j] = '\0';
    return out;
}

char *url_decode(const char *s)
{
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t i = 0, j = 0;
    while (i < n) {
        if (s[i] == '%' && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            out[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            out[j++] = s[i++];
        }
    }
    out[j] = '\0';
    return out;
}

char *state_dir(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char *dir;
    if (xdg && *xdg) {
        dir = xmalloc(strlen(xdg) + 16);
        sprintf(dir, "%s/bili-cli", xdg);
    } else {
        dir = xmalloc(strlen(home) + 32);
        sprintf(dir, "%s/.local/state/bili-cli", home);
    }
    return dir;
}

char *sanitize_filename(const char *name)
{
    static const char *illegal = "/\\:*?\"<>|";
    size_t n = strlen(name);
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr(illegal, c)) {
            out[j++] = '_';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    /* 去掉首尾空白 */
    char *start = out;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '.')) {
        end--;
    }
    *end = '\0';
    char *res = xstrdup(*start ? start : "_");
    free(out);
    return res;
}

int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    int rc = mkdir(tmp, 0755);
    free(tmp);
    return (rc == 0 || errno == EEXIST) ? 0 : -1;
}

const char *human_size(uint64_t bytes)
{
    static char buf[32];
    if (bytes >= 1u << 30) {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    } else if (bytes >= 1u << 20) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

const char *human_duration(long seconds)
{
    static char buf[32];
    long h = seconds / 3600;
    long m = (seconds % 3600) / 60;
    long s = seconds % 60;
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, s);
    } else {
        snprintf(buf, sizeof(buf), "%ld:%02ld", m, s);
    }
    return buf;
}

void kv_add(kv_t **arr, size_t *n, const char *k, const char *v)
{
    *arr = xrealloc(*arr, (*n + 1) * sizeof(kv_t));
    (*arr)[*n].key = xstrdup(k);
    (*arr)[*n].val = xstrdup(v ? v : "");
    (*n)++;
}

void kv_free(kv_t *arr, size_t n)
{
    if (!arr) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(arr[i].key);
        free(arr[i].val);
    }
    free(arr);
}
