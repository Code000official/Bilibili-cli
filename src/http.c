/* http.c - 基于系统 curl 二进制的 HTTP 封装
 *
 * 不直接依赖 libcurl 头文件，而是 fork/exec 调用系统 curl，
 * 借此获得重定向、断点续传、重试与 TLS 等能力，且零编译依赖。
 * argv 中全部为字符串字面量，无动态内存，无需逐项释放。
 */
#include "http.h"
#include "util.h"

#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *HTTP_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

/* 组装 API 请求公共参数，返回参数个数 */
static int curl_args(char **argv, const char *cookie, const char *max_time)
{
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"--fail";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"15";
    argv[n++] = (char *)"--max-time";
    argv[n++] = (char *)max_time;
    argv[n++] = (char *)"--retry";
    argv[n++] = (char *)"8";
    argv[n++] = (char *)"--retry-all-errors";
    argv[n++] = (char *)"--retry-delay";
    argv[n++] = (char *)"2";
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

static int wait_child(pid_t pid)
{
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    return -1;
}

int http_get_str_jar(const char *url, const char *cookie, const char *jarfile,
                     char **out)
{
    *out = NULL;
    char *argv[52];
    int n = curl_args(argv, cookie, "60");
    if (jarfile && *jarfile) {
        argv[n++] = (char *)"-c";
        argv[n++] = (char *)jarfile;
    }
    argv[n++] = (char *)url;
    argv[n] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp("curl", argv);
        fprintf(stderr, "错误: 找不到 curl 命令\n");
        _exit(127);
    }
    close(pipefd[1]);

    strbuf_t sb;
    sb_init(&sb);
    char buf[8192];
    ssize_t r;
    while ((r = read(pipefd[0], buf, sizeof(buf))) > 0) {
        sb_append_len(&sb, buf, (size_t)r);
    }
    close(pipefd[0]);

    int rc = wait_child(pid);
    if (rc != 0) {
        sb_free(&sb);
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

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp("curl", argv);
        fprintf(stderr, "错误: 找不到 curl 命令\n");
        _exit(127);
    }
    return wait_child(pid);
}

int http_resolve(const char *url, char **out)
{
    *out = NULL;
    char *argv[24];
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)"/dev/null";
    argv[n++] = (char *)"-w";
    argv[n++] = (char *)"%{url_effective}";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"15";
    argv[n++] = (char *)"--max-time";
    argv[n++] = (char *)"60";
    argv[n++] = (char *)"-A";
    argv[n++] = (char *)HTTP_UA;
    argv[n++] = (char *)url;
    argv[n] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp("curl", argv);
        _exit(127);
    }
    close(pipefd[1]);

    strbuf_t sb;
    sb_init(&sb);
    char buf[1024];
    ssize_t r;
    while ((r = read(pipefd[0], buf, sizeof(buf))) > 0) {
        sb_append_len(&sb, buf, (size_t)r);
    }
    close(pipefd[0]);

    int rc = wait_child(pid);
    if (rc != 0 || sb.len == 0) {
        sb_free(&sb);
        return -1;
    }
    *out = sb.data;
    return 0;
}
