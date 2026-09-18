/* http.h - 基于系统 curl 二进制的 HTTP 封装 */
#ifndef BILI_CLI_HTTP_H
#define BILI_CLI_HTTP_H

/* 所有请求使用的 User-Agent */
extern const char *HTTP_UA;

/*
 * GET 请求，返回响应体文本（调用方 free；*out 置 NULL 表示失败）。
 * cookie: 完整的 Cookie 头值（如 "buvid3=xxx"），可为 NULL。
 * jarfile: 非 NULL 时让 curl 用 -c 把响应的 Set-Cookie 写入该文件
 *          （Netscape 格式，扫码登录捕获凭证用）。
 * 返回 0 成功（此时 *out 为 malloc 的 NUL 结尾字符串），非 0 失败。
 */
int http_get_str_jar(const char *url, const char *cookie, const char *jarfile,
                     char **out);

/* 同 http_get_str_jar(jarfile=NULL) */
int http_get_str(const char *url, const char *cookie, char **out);

/*
 * 下载文件到 outfile（断点续传，curl 自带进度条与重试）。
 * 返回 0 成功，非 0 失败。
 */
int http_download(const char *url, const char *outfile, const char *cookie);

/*
 * 跟随重定向解析最终 URL（用于 b23.tv 短链）。
 * 返回 0 成功，*out 为 malloc 的最终 URL。
 */
int http_resolve(const char *url, char **out);

#endif
