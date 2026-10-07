/* port.h - 跨平台基础层（POSIX / Windows）
 *
 * 抽象本项目中所有依赖操作系统的调用，其余模块保持纯 C11：
 *   1. 子进程：spawn（继承/静默/管道三种模式）、非阻塞轮询、阻塞等待、终止
 *   2. 控制台：Windows 下启用 ANSI 转义与 UTF-8 代码页
 *   3. 杂项：毫秒睡眠、NUL 设备名
 */
#ifndef BILI_CLI_PORT_H
#define BILI_CLI_PORT_H

#include <stddef.h>
#include <stdio.h>

/* 子进程标识：POSIX 为 pid，Windows 为进程句柄（需要强转）。
 * 无效值统一用 BILI_PID_INVALID 判断。 */
#ifdef _WIN32
typedef void *bili_pid_t;
#define BILI_PID_INVALID ((bili_pid_t)0)
#else
typedef long bili_pid_t;
#define BILI_PID_INVALID ((bili_pid_t)-1)
#endif

typedef enum {
    BILI_SPAWN_INHERIT = 0, /* 继承父进程 stdio：shell 模式下 curl/ffmpeg 进度可见 */
    BILI_SPAWN_SILENT,      /* 子进程 stdin/stdout/stderr 全部重定向到 NUL（TUI 后台任务） */
    BILI_SPAWN_PIPE,        /* 子进程 stdout 接管道，用 bili_child_read() 读取 */
} bili_spawn_mode_t;

/* 启动子进程（argv 以 NULL 结尾），失败返回 BILI_PID_INVALID */
bili_pid_t bili_spawn(char *const argv[], bili_spawn_mode_t mode);

/* 读取 PIPE 模式子进程的 stdout；返回字节数，0=EOF/结束，<0=错误 */
long bili_child_read(char *buf, size_t n);

/* 阻塞等待子进程退出并回收资源；返回退出码，失败 -1 */
int bili_wait(bili_pid_t pid);

/* 非阻塞轮询：1=已退出（*exit_code 有效，资源已回收），0=仍在运行，-1=出错 */
int bili_poll(bili_pid_t pid, int *exit_code);

/* 终止子进程（SIGTERM / TerminateProcess）；调用后需 bili_wait() 回收 */
void bili_kill(bili_pid_t pid);

void bili_sleep_ms(long ms);

/* NUL 设备路径：POSIX "/dev/null"，Windows "NUL" */
const char *bili_null_device(void);

/* Windows 控制台初始化：启用 ANSI 转义输出 + UTF-8 代码页（POSIX 无操作）。
 * main() 入口处调用一次；退出时自动恢复。 */
void bili_console_init(void);

/* ---------- UTF-8 安全的文件与环境封装 ----------
 *
 * 全程序内部统一使用 UTF-8 字符串；Windows CRT 的窄字符 API 会把字节串
 * 按系统 ANSI 代码页（如 GBK）解释，任何非 ASCII 路径都会出错，
 * 因此 Windows 下这些封装先把路径转为 UTF-16 再走宽字符 API。 */

/* getenv 的 UTF-8 安全版；返回 malloc 字符串，未设置返回 NULL（调用方 free） */
char *bili_getenv(const char *name);

FILE *bili_fopen(const char *path, const char *mode);

/* mkdir；已存在返回 -1（errno==EEXIST），语义与 POSIX mkdir 一致 */
int bili_mkdir(const char *path);

/* rename；Windows 下覆盖已存在的目标 */
int bili_rename(const char *oldpath, const char *newpath);

int bili_remove(const char *path);

/* 文件大小，失败返回 -1 */
long long bili_file_size(const char *path);

#endif
