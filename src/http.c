/* http.c - 基于系统 curl 二进制的 HTTP 封装
 *
 * 不直接依赖 libcurl 头文件，而是 spawn 系统子进程调用 curl，
 * 借此获得重定向、断点续传、重试与 TLS 等能力，且零编译依赖。
 * argv 中全部为字符串字面量，无动态内存，无需逐项释放。
 */
#include "http.h"
#include "port.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *HTTP_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

/* 组装 API 请求公共参数，返回参数个数。
 * API 均为小 JSON，正常 <1s 完成；快速失败优先：
 * 单次 10s 连接 / 20s 总超时，重试 3 次，最坏 ~1 分钟出结果，
 * 避免网络不通时长时间挂起。 */
static int curl_args(char **argv, const char *cookie, const char *max_time)
{
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"--fail";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"10";
    argv[n++] = (char *)"--max-time";
    argv[n++] = (char *)max_time;
    argv[n++] = (char *)"--retry";
    argv[n++] = (char *)"3";
    argv[n++] = (char *)"--retry-all-errors";
    argv[n++] = (char *)"--retry-delay";
    argv[n++] = (char *)"1";
    argv[n++] = (char *)"-A";
    argv[n++] = (char *)HTTP_UA;
    if (cookie && *cookie) {
        static char cookie_hdr[8192];
        snprintf(cookie_hdr, sizeof(cookie_hdr), "Cookie: %s", cookie);
        argv[n++] = (char *)"-H";
        argv[n++] = cookie_hdr;
    }
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Referer: https://www.bilibili.com";
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Origin: https://www.bilibili.com";
    return n;
}

/* 读取 PIPE 模式子进程全部 stdout 并等待退出；失败释放 sb 返回 -1 */
static int read_all_and_wait(bili_pid_t pid, strbuf_t *sb)
{
    char buf[8192];
    long r;
    while ((r = bili_child_read(buf, sizeof(buf))) > 0) {
        sb_append_len(sb, buf, (size_t)r);
    }
    int rc = bili_wait(pid);
    if (rc != 0) {
        sb_free(sb);
        return -1;
    }
    return 0;
}

int http_get_str_jar(const char *url, const char *cookie, const char *jarfile,
                     char **out)
{
    *out = NULL;
    char *argv[52];
    int n = curl_args(argv, cookie, "20");
    if (jarfile && *jarfile) {
        argv[n++] = (char *)"-c";
        argv[n++] = (char *)jarfile;
    }
    argv[n++] = (char *)url;
    argv[n] = NULL;

    bili_pid_t pid = bili_spawn(argv, BILI_SPAWN_PIPE);
    if (pid == BILI_PID_INVALID) {
        return -1;
    }

    strbuf_t sb;
    sb_init(&sb);
    if (read_all_and_wait(pid, &sb) != 0) {
        return -1;
    }
    *out = sb.data; /* 直接移交缓冲 */
    return 0;
}

int http_get_str(const char *url, const char *cookie, char **out)
{
    return http_get_str_jar(url, cookie, NULL, out);
}

int http_download(const char *url, const char *outfile, const char *cookie)
{
    char *argv[48];
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"--fail";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"15";
    /* 下载不限总时长，但 30s 内速度低于 1KB/s 视为卡死并触发重试 */
    argv[n++] = (char *)"--speed-time";
    argv[n++] = (char *)"30";
    argv[n++] = (char *)"--speed-limit";
    argv[n++] = (char *)"1024";
    argv[n++] = (char *)"--retry";
    argv[n++] = (char *)"8";
    argv[n++] = (char *)"--retry-all-errors";
    argv[n++] = (char *)"--retry-delay";
    argv[n++] = (char *)"2";
    argv[n++] = (char *)"--continue-at";
    argv[n++] = (char *)"-";
    argv[n++] = (char *)"-#";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)outfile;
    argv[n++] = (char *)"-A";
    argv[n++] = (char *)HTTP_UA;
    if (cookie && *cookie) {
        static char cookie_hdr[8192];
        snprintf(cookie_hdr, sizeof(cookie_hdr), "Cookie: %s", cookie);
        argv[n++] = (char *)"-H";
        argv[n++] = cookie_hdr;
    }
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Referer: https://www.bilibili.com";
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Origin: https://www.bilibili.com";
    argv[n++] = (char *)url;
    argv[n] = NULL;

    bili_pid_t pid = bili_spawn(argv, BILI_SPAWN_INHERIT);
    if (pid == BILI_PID_INVALID) {
        return -1;
    }
    return bili_wait(pid);
}

int http_resolve(const char *url, char **out)
{
    *out = NULL;
    char *argv[24];
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)bili_null_device();
    argv[n++] = (char *)"-w";
    argv[n++] = (char *)"%{url_effective}";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"10";
    argv[n++] = (char *)"--max-time";
    argv[n++] = (char *)"20";
    argv[n++] = (char *)"--retry";
    argv[n++] = (char *)"3";
    argv[n++] = (char *)"--retry-all-errors";
    argv[n++] = (char *)"--retry-delay";
    argv[n++] = (char *)"1";
    argv[n++] = (char *)"-A";
    argv[n++] = (char *)HTTP_UA;
    argv[n++] = (char *)url;
    argv[n] = NULL;

    bili_pid_t pid = bili_spawn(argv, BILI_SPAWN_PIPE);
    if (pid == BILI_PID_INVALID) {
        return -1;
    }

    strbuf_t sb;
    sb_init(&sb);
    if (read_all_and_wait(pid, &sb) != 0) {
        return -1;
    }
    if (sb.len == 0) {
        sb_free(&sb);
        return -1;
    }
    *out = sb.data;
    return 0;
}
