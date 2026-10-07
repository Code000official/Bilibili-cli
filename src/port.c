/* port.c - 跨平台基础层实现（POSIX / Windows） */
#include "port.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ---------------- Windows ---------------- */

#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static HANDLE g_child_stdout_read = NULL;

/* UTF-8 <-> UTF-16 转换；失败返回 NULL（结果 malloc，调用方 free） */
static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    char *s = xmalloc((size_t)n);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

/* 按 Windows 规则拼接命令行：含空格/制表/引号的参数加引号，
 * 引号转义为 \"，引号前的连续反斜杠按规则翻倍。 */
static void quote_arg(strbuf_t *cmd, const char *arg)
{
    if (arg[0] != '\0' && strpbrk(arg, " \t\"") == NULL) {
        sb_append(cmd, arg);
        return;
    }
    sb_append_len(cmd, "\"", 1);
    size_t slashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            slashes++;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) {
                sb_append_len(cmd, "\\", 1);
            }
            slashes = 0;
            sb_append_len(cmd, "\\\"", 2);
        } else {
            for (size_t i = 0; i < slashes; i++) {
                sb_append_len(cmd, "\\", 1);
            }
            slashes = 0;
            sb_append_len(cmd, p, 1);
        }
    }
    for (size_t i = 0; i < slashes * 2; i++) {
        sb_append_len(cmd, "\\", 1);
    }
    sb_append_len(cmd, "\"", 1);
}

bili_pid_t bili_spawn(char *const argv[], bili_spawn_mode_t mode)
{
    strbuf_t cmd;
    sb_init(&cmd);
    for (size_t i = 0; argv[i]; i++) {
        if (i) {
            sb_append_len(&cmd, " ", 1);
        }
        quote_arg(&cmd, argv[i]);
    }
    /* 必须走 W 版：A 版会用系统 ANSI 代码页（GBK）转换命令行，
     * UTF-8 的中文路径会被破坏 */
    wchar_t *wcmd = utf8_to_wide(cmd.data);
    sb_free(&cmd);
    if (!wcmd) {
        return BILI_PID_INVALID;
    }

    HANDLE rd = NULL, wr = NULL, nul = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (mode == BILI_SPAWN_PIPE) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            free(wcmd);
            return BILI_PID_INVALID;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); /* 读端不继承 */
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    } else if (mode == BILI_SPAWN_SILENT) {
        nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, NULL);
        if (nul == INVALID_HANDLE_VALUE) {
            free(wcmd);
            return BILI_PID_INVALID;
        }
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = nul;
        si.hStdError = nul;
    }

    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL,
                             &si, &pi);
    if (wr) {
        CloseHandle(wr);
    }
    if (nul) {
        CloseHandle(nul);
    }
    free(wcmd);
    if (!ok) {
        if (rd) {
            CloseHandle(rd);
        }
        return BILI_PID_INVALID;
    }
    CloseHandle(pi.hThread);
    if (mode == BILI_SPAWN_PIPE) {
        g_child_stdout_read = rd;
    }
    return (bili_pid_t)pi.hProcess;
}

static void close_child_pipe(void)
{
    if (g_child_stdout_read) {
        CloseHandle(g_child_stdout_read);
        g_child_stdout_read = NULL;
    }
}

long bili_child_read(char *buf, size_t n)
{
    if (!g_child_stdout_read) {
        return -1;
    }
    DWORD got = 0;
    if (!ReadFile(g_child_stdout_read, buf, (DWORD)n, &got, NULL) ||
        got == 0) {
        return 0; /* EOF */
    }
    return (long)got;
}

int bili_wait(bili_pid_t pid)
{
    HANDLE h = (HANDLE)pid;
    if (!h) {
        return -1;
    }
    WaitForSingleObject(h, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    close_child_pipe();
    return (int)code;
}

int bili_poll(bili_pid_t pid, int *exit_code)
{
    HANDLE h = (HANDLE)pid;
    if (!h) {
        return -1;
    }
    DWORD r = WaitForSingleObject(h, 0);
    if (r == WAIT_TIMEOUT) {
        return 0;
    }
    if (r != WAIT_OBJECT_0) {
        return -1;
    }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    close_child_pipe();
    *exit_code = (int)code;
    return 1;
}

void bili_kill(bili_pid_t pid)
{
    HANDLE h = (HANDLE)pid;
    if (h) {
        TerminateProcess(h, (UINT)-1);
    }
}

void bili_sleep_ms(long ms)
{
    Sleep((DWORD)ms);
}

const char *bili_null_device(void)
{
    return "NUL";
}

/* ---------- 控制台 ---------- */

static UINT g_saved_in_cp = 0, g_saved_out_cp = 0;

static void console_restore(void)
{
    if (g_saved_out_cp) {
        SetConsoleOutputCP(g_saved_out_cp);
    }
    if (g_saved_in_cp) {
        SetConsoleCP(g_saved_in_cp);
    }
}

void bili_console_init(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    UINT in_cp = GetConsoleCP(), out_cp = GetConsoleOutputCP();
    if (out_cp != 65001) {
        g_saved_out_cp = out_cp;
        SetConsoleOutputCP(65001);
    }
    if (in_cp != 65001) {
        g_saved_in_cp = in_cp;
        SetConsoleCP(65001);
    }
    atexit(console_restore);
}

/* ---------- UTF-8 安全的文件与环境封装 ---------- */

char *bili_getenv(const char *name)
{
    wchar_t *wname = utf8_to_wide(name);
    if (!wname) {
        return NULL;
    }
    DWORD n = GetEnvironmentVariableW(wname, NULL, 0);
    if (n == 0) {
        free(wname);
        return NULL; /* 未设置（或为空） */
    }
    wchar_t *wval = xmalloc((size_t)n * sizeof(wchar_t));
    GetEnvironmentVariableW(wname, wval, n);
    free(wname);
    char *val = wide_to_utf8(wval);
    free(wval);
    return val;
}

FILE *bili_fopen(const char *path, const char *mode)
{
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }
    wchar_t wmode[16];
    size_t i = 0;
    for (; i < sizeof(wmode) / sizeof(wmode[0]) - 1 && mode[i]; i++) {
        wmode[i] = (wchar_t)(unsigned char)mode[i];
    }
    wmode[i] = 0;
    FILE *fp = _wfopen(wpath, wmode);
    free(wpath);
    return fp;
}

int bili_mkdir(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return -1;
    }
    int rc = _wmkdir(wpath);
    free(wpath);
    return rc;
}

int bili_rename(const char *oldpath, const char *newpath)
{
    wchar_t *wold = utf8_to_wide(oldpath);
    wchar_t *wnew = utf8_to_wide(newpath);
    if (!wold || !wnew) {
        free(wold);
        free(wnew);
        return -1;
    }
    /* MoveFileExW + REPLACE_EXISTING 使语义与 POSIX rename 一致（覆盖目标） */
    int rc = MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
    free(wold);
    free(wnew);
    return rc;
}

int bili_remove(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return -1;
    }
    int rc = _wremove(wpath);
    free(wpath);
    return rc;
}

long long bili_file_size(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return -1;
    }
    struct _stat64 st;
    int rc = _wstat64(wpath, &st);
    free(wpath);
    return rc == 0 ? (long long)st.st_size : -1;
}

#else /* _WIN32 */

/* ---------------- POSIX ---------------- */

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pipe_fd = -1;

bili_pid_t bili_spawn(char *const argv[], bili_spawn_mode_t mode)
{
    fflush(NULL);
    int pipefd[2] = { -1, -1 };
    if (mode == BILI_SPAWN_PIPE && pipe(pipefd) != 0) {
        return BILI_PID_INVALID;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return BILI_PID_INVALID;
    }
    if (pid == 0) {
        if (mode == BILI_SPAWN_PIPE) {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        } else if (mode == BILI_SPAWN_SILENT) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        execvp(argv[0], argv);
        fprintf(stderr, "错误: 找不到 %s 命令\n", argv[0]);
        _exit(127);
    }
    if (mode == BILI_SPAWN_PIPE) {
        close(pipefd[1]);
        g_pipe_fd = pipefd[0];
    }
    return (bili_pid_t)pid;
}

static void close_child_pipe(void)
{
    if (g_pipe_fd >= 0) {
        close(g_pipe_fd);
        g_pipe_fd = -1;
    }
}

long bili_child_read(char *buf, size_t n)
{
    if (g_pipe_fd < 0) {
        return -1;
    }
    for (;;) {
        ssize_t r = read(g_pipe_fd, buf, n);
        if (r < 0 && errno == EINTR) {
            continue; /* SIGWINCH 等信号打断，重试以免响应被截断 */
        }
        return (long)r;
    }
}

int bili_wait(bili_pid_t pid)
{
    if (pid <= 0) {
        close_child_pipe();
        return -1;
    }
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) {
        close_child_pipe();
        return -1;
    }
    close_child_pipe();
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int bili_poll(bili_pid_t pid, int *exit_code)
{
    if (pid <= 0) {
        return -1;
    }
    int st = 0;
    pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }
    close_child_pipe();
    *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return 1;
}

void bili_kill(bili_pid_t pid)
{
    if (pid > 0) {
        kill((pid_t)pid, SIGTERM);
    }
}

void bili_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

const char *bili_null_device(void)
{
    return "/dev/null";
}

void bili_console_init(void)
{
}

/* ---------- UTF-8 安全的文件与环境封装（POSIX 直接映射） ---------- */

char *bili_getenv(const char *name)
{
    const char *v = getenv(name);
    return (v && *v) ? xstrdup(v) : NULL;
}

FILE *bili_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int bili_mkdir(const char *path)
{
    return mkdir(path, 0755);
}

int bili_rename(const char *oldpath, const char *newpath)
{
    return rename(oldpath, newpath);
}

int bili_remove(const char *path)
{
    return remove(path);
}

long long bili_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

#endif /* _WIN32 */
