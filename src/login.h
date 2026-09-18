/* login.h - 扫码登录、cookie 持久化与账号信息 */
#ifndef BILI_CLI_LOGIN_H
#define BILI_CLI_LOGIN_H

#include "util.h"

/*
 * 扫码登录全流程：生成二维码 -> 终端渲染 -> 轮询（约2s/次，过期自动重出）。
 * 成功后 cookie 持久化到 state 目录。返回 0 成功。
 */
int login_qr_flow(int verbose);

/* 清除已保存的登录 cookie。返回 0 成功 */
int login_logout(void);

/* 查询当前登录状态（nav 接口）并打印。cookie 可为 NULL */
int login_whoami(const char *cookie);

/*
 * 轻量登录状态查询（不打印）。
 * 返回 1 已登录（*uname 返回 malloc 用户名，可为 NULL 不需要）；
 * 0 未登录；-1 查询失败。
 */
int login_status(const char *cookie, char **uname);

/* 读取已保存的登录 cookie，追加到 kv 列表（无则不动） */
void login_load_cookies(kv_t **arr, size_t *n);

/* 组装完整 Cookie 头（自动取 buvid + 已保存登录凭证）。返回 malloc 字符串，可为 NULL */
char *login_full_cookie(void);

/*
 * 组装完整 Cookie 头值：buvid + 已保存登录凭证（SESSDATA 可被 sessdata_override 覆盖）。
 * 返回 malloc 字符串，无任何内容时返回 NULL。
 */
char *login_build_cookie(const char *b3, const char *b4,
                         const char *sessdata_override);

#endif
