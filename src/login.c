/* login.c - 扫码登录、cookie 持久化与账号信息
 *
 * Web 端二维码登录流程：
 *   GET /x/passport-login/web/qrcode/generate -> {qrcode_key, url}
 *   终端渲染 QR（qrcodegen + 半块字符 + 固定 ANSI 颜色，不依赖终端主题）
 *   GET /x/passport-login/web/qrcode/poll?qrcode_key=
 *     data.code: 86101 未扫码 / 86090 已扫未确认 / 86038 已过期 / 0 成功
 *   成功后 data.url 的查询参数中携带 SESSDATA/bili_jct 等凭证。
 */
#include "login.h"

#include "bili.h"
#include "http.h"

#include <cJSON.h>
#include <qrcodegen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QR_GEN  "https://passport.bilibili.com/x/passport-login/web/qrcode/generate"
#define QR_POLL "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key="
#define API_NAV "https://api.bilibili.com/x/web-interface/nav"

#define POLL_ROUNDS    100   /* 每张二维码最多 100*2s */
#define QR_MAX_ATTEMPT 3     /* 过期后最多重出次数 */

/* ---------- cJSON 辅助 ---------- */

static char *jstr(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? xstrdup(cJSON_GetStringValue(it)) : NULL;
}

static long long jint(cJSON *obj, const char *key, long long def)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (long long)cJSON_GetNumberValue(it) : def;
}

/* JSON 布尔/数字通吃的取值（nav 的 isLogin 是布尔 true，不是 1） */
static int jbool(cJSON *obj, const char *key, int def)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it) ? 1 : 0;
    }
    if (cJSON_IsNumber(it)) {
        return cJSON_GetNumberValue(it) != 0;
    }
    return def;
}

/* ---------- cookie 文件 ---------- */

static char *cookie_path(void)
{
    char *dir = state_dir();
    mkdir_p(dir);
    char *path = xmalloc(strlen(dir) + 16);
    sprintf(path, "%s/cookie.txt", dir);
    free(dir);
    return path;
}

void login_load_cookies(kv_t **arr, size_t *n)
{
    char *path = cookie_path();
    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) {
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        char *eq = strchr(line, '=');
        if (!eq || eq == line) {
            continue;
        }
        *eq = '\0';
        kv_add(arr, n, line, eq + 1);
    }
    fclose(fp);
}

/* 保存 kv 列表到 cookie 文件（仅限凭证字段，调用前已过滤） */
static void save_cookies(const kv_t *kv, size_t n)
{
    char *path = cookie_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "错误: 无法写入 %s\n", path);
        free(path);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        fprintf(fp, "%s=%s\n", kv[i].key, kv[i].val);
    }
    fclose(fp);
    free(path);
}

char *login_full_cookie(void)
{
    char *b3 = NULL, *b4 = NULL;
    bili_get_buvid(&b3, &b4, 0);
    char *ck = login_build_cookie(b3, b4, NULL);
    free(b3);
    free(b4);
    return ck;
}

char *login_build_cookie(const char *b3, const char *b4,
                         const char *sessdata_override)
{
    kv_t *kv = NULL;
    size_t n = 0;
    if (b3 && *b3) {
        kv_add(&kv, &n, "buvid3", b3);
    }
    if (b4 && *b4) {
        kv_add(&kv, &n, "buvid4", b4);
    }
    login_load_cookies(&kv, &n);
    if (sessdata_override && *sessdata_override) {
        int found = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(kv[i].key, "SESSDATA") == 0) {
                free(kv[i].val);
                kv[i].val = xstrdup(sessdata_override);
                found = 1;
                break;
            }
        }
        if (!found) {
            kv_add(&kv, &n, "SESSDATA", sessdata_override);
        }
    }
    if (n == 0) {
        return NULL;
    }
    strbuf_t sb;
    sb_init(&sb);
    for (size_t i = 0; i < n; i++) {
        sb_appendf(&sb, "%s%s=%s", i ? "; " : "", kv[i].key, kv[i].val);
    }
    kv_free(kv, n);
    return sb.data;
}

int login_logout(void)
{
    char *path = cookie_path();
    int rc = unlink(path);
    free(path);
    if (rc == 0) {
        printf("已退出登录\n");
        return 0;
    }
    printf("当前未登录\n");
    return 0;
}

/* ---------- 终端二维码渲染 ---------- */

/* 半块字符 + 显式 ANSI 颜色，不依赖终端主题：dark=16(黑) light=231(白) */
static void print_qr(const char *text)
{
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qrcode, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        printf("(二维码生成失败) 可在手机浏览器打开登录链接:\n  %s\n", text);
        return;
    }
    int size = qrcodegen_getSize(qrcode);
    int quiet = 2;
    int total = size + quiet * 2;
    printf("\n");
    for (int y = 0; y < total; y += 2) {
        for (int x = 0; x < total; x++) {
            /* 静区视为白 */
            int top = 0, bot = 0;
            if (x >= quiet && x < total - quiet) {
                if (y >= quiet && y < total - quiet) {
                    top = qrcodegen_getModule(qrcode, x - quiet, y - quiet);
                }
                if (y + 1 >= quiet && y + 1 < total - quiet) {
                    bot = qrcodegen_getModule(qrcode, x - quiet, y + 1 - quiet);
                }
            }
            /* ▀ 上半块用前景色，下半块露背景色 */
            printf("\x1b[38;5;%dm\x1b[48;5;%dm▀", top ? 16 : 231, bot ? 16 : 231);
        }
        printf("\x1b[0m\n");
    }
}

/* ---------- 登录流程 ---------- */

/* 凭证字段过滤 */
static int is_credential_key(const char *k)
{
    return strcmp(k, "SESSDATA") == 0 || strcmp(k, "bili_jct") == 0 ||
           strcmp(k, "Expires") == 0 || strncmp(k, "DedeUserID", 10) == 0;
}

/* 从 crossDomain URL 的查询参数提取凭证，追加到 kv */
static void collect_cookies_from_url(const char *url, kv_t **arr, size_t *n)
{
    const char *q = strchr(url, '?');
    if (!q) {
        return;
    }
    char *dup = xstrdup(q + 1);
    char *save = NULL;
    for (char *p = strtok_r(dup, "&", &save); p; p = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (is_credential_key(p)) {
            char *v = url_decode(eq + 1);
            size_t i;
            int replaced = 0;
            for (i = 0; i < *n; i++) {
                if (strcmp((*arr)[i].key, p) == 0) {
                    free((*arr)[i].val);
                    (*arr)[i].val = xstrdup(v);
                    replaced = 1;
                    break;
                }
            }
            if (!replaced) {
                kv_add(arr, n, p, v);
            }
            free(v);
        }
    }
    free(dup);
}

/* 从 curl -c 的 Netscape cookie jar 提取凭证，追加到 kv。
 * 行格式: domain \t flag \t path \t secure \t expiry \t name \t value
 * HttpOnly 行以 "#HttpOnly_" 开头。 */
static void collect_cookies_from_jar(const char *path, kv_t **arr, size_t *n)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[16384];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        if (strncmp(line, "#HttpOnly_", 10) == 0) {
            memmove(line, line + 10, l - 9);
            l -= 10;
        } else if (line[0] == '#') {
            continue;
        }
        /* 按制表符切 7 列 */
        char *cols[7] = { NULL };
        int nc = 0;
        char *save = NULL;
        for (char *p = strtok_r(line, "\t", &save); p && nc < 7;
             p = strtok_r(NULL, "\t", &save)) {
            cols[nc++] = p;
        }
        if (nc < 7 || !is_credential_key(cols[5])) {
            continue;
        }
        kv_add(arr, n, cols[5], cols[6]);
    }
    fclose(fp);
}

static int has_key(const kv_t *kv, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(kv[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

int login_qr_flow(int verbose)
{
    (void)verbose;

    for (int attempt = 0; attempt < QR_MAX_ATTEMPT; attempt++) {
        /* 生成二维码 */
        char *body = NULL;
        if (http_get_str(QR_GEN, NULL, &body) != 0) {
            fprintf(stderr, "错误: 获取登录二维码失败\n");
            return 1;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            fprintf(stderr, "错误: 登录二维码响应解析失败\n");
            return 1;
        }
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        char *qrkey = jstr(data, "qrcode_key");
        char *qrurl = jstr(data, "url");
        cJSON_Delete(root);
        if (!qrkey || !qrurl) {
            fprintf(stderr, "错误: 登录二维码响应缺少字段\n");
            free(qrkey);
            free(qrurl);
            return 1;
        }

        printf("请使用哔哩哔哩 APP 扫描二维码登录（%d/3）:\n", attempt + 1);
        print_qr(qrurl);
        printf("无法扫码时，可在手机浏览器打开:\n  %s\n", qrurl);
        printf("等待扫码中...\n");

        /* 轮询全程挂 cookie jar：成功那次响应的 Set-Cookie 会被 curl 记录 */
        char *jar = NULL;
        {
            char *dir = state_dir();
            jar = xmalloc(strlen(dir) + 16);
            sprintf(jar, "%s/login.jar", dir);
            free(dir);
            remove(jar);
        }
        free(qrurl);

        /* 轮询 */
        int scanned = 0, expired = 0;
        for (int t = 0; t < POLL_ROUNDS; t++) {
            sleep(2);
            char *purl = xmalloc(strlen(QR_POLL) + strlen(qrkey) + 1);
            sprintf(purl, "%s%s", QR_POLL, qrkey);
            char *pbody = NULL;
            int rc = http_get_str_jar(purl, NULL, jar, &pbody);
            free(purl);
            if (rc != 0) {
                continue; /* 网络抖动，继续轮询 */
            }
            cJSON *proot = cJSON_Parse(pbody);
            if (!proot) {
                free(pbody);
                continue;
            }
            cJSON *pdata = cJSON_GetObjectItemCaseSensitive(proot, "data");
            long long code = jint(pdata, "code", -1);
            char *durl = jstr(pdata, "url");
            cJSON_Delete(proot);
            /* pbody 延迟到本迭代各出口统一释放（成功路径的调试输出还要用它） */

            if (code == 86101) {
                free(durl);
                free(pbody);
                continue; /* 未扫码 */
            }
            if (code == 86090) {
                if (!scanned) {
                    printf("已扫描，请在手机上确认登录...\n");
                    scanned = 1;
                }
                free(durl);
                free(pbody);
                continue;
            }
            if (code == 0) {
                printf("\n登录成功！\n");
                /* 双通道取凭证：Set-Cookie jar（成功响应头）+ data.url 查询参数 */
                kv_t *kv = NULL;
                size_t nk = 0;
                collect_cookies_from_jar(jar, &kv, &nk);
                if (durl) {
                    collect_cookies_from_url(durl, &kv, &nk);
                }
                remove(jar);
                free(jar);
                free(durl);
                durl = NULL;

                if (!has_key(kv, nk, "SESSDATA")) {
                    fprintf(stderr,
                            "错误: 未能从登录响应中提取 SESSDATA（SET-COOKIE 与"
                            " data.url 均无凭证）。\n"
                            "      原始响应已写入 bili-login-debug.log，请带着该文件反馈。\n");
                    FILE *dbg = fopen("bili-login-debug.log", "w");
                    if (dbg) {
                        fprintf(dbg, "%s\n", pbody ? pbody : "(空)");
                        fclose(dbg);
                    }
                    free(pbody);
                    kv_free(kv, nk);
                    free(qrkey);
                    return 1;
                }
                free(pbody);
                save_cookies(kv, nk);
                printf("登录凭证已保存到 %s/cookie.txt\n", state_dir());
                kv_free(kv, nk);
                free(qrkey);
                /* 打印账号信息确认 */
                char *ck = login_build_cookie(NULL, NULL, NULL);
                login_whoami(ck);
                free(ck);
                return 0;
            }
            if (code == 86038) {
                printf("二维码已过期。\n");
                expired = 1;
                free(durl);
                free(pbody);
                break;
            }
            fprintf(stderr, "\n登录失败 (%lld)，请重试\n", code);
            free(durl);
            free(pbody);
            free(qrkey);
            return 1;
        }
        (void)expired;
        free(qrkey);
        free(jar);
    }
    fprintf(stderr, "多次二维码过期，已放弃。请重新运行登录命令。\n");
    return 1;
}

int login_status(const char *cookie, char **uname)
{
    if (uname) {
        *uname = NULL;
    }
    char *body = NULL;
    if (http_get_str(API_NAV, cookie, &body) != 0) {
        return -1;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return -1;
    }
    long long code = jint(root, "code", -1);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    int logged = (code == 0 && cJSON_IsObject(data) && jbool(data, "isLogin", 0) == 1);
    if (logged && uname) {
        *uname = jstr(data, "uname");
    }
    cJSON_Delete(root);
    return logged;
}

int login_whoami(const char *cookie)
{
    char *body = NULL;
    if (http_get_str(API_NAV, cookie, &body) != 0) {
        fprintf(stderr, "错误: 查询登录状态失败\n");
        return 1;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "错误: 登录状态响应解析失败\n");
        return 1;
    }
    long long code = jint(root, "code", -1);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (code == -101 || !cJSON_IsObject(data) || jbool(data, "isLogin", 0) == 0) {
        printf("当前未登录（匿名仅能下载 480P，扫码登录后可解锁高清晰度）\n");
        cJSON_Delete(root);
        return 1;
    }
    char *uname = jstr(data, "uname");
    long long level = jint(cJSON_GetObjectItemCaseSensitive(data, "level_info"),
                           "current_level", 0);
    long long vip = jint(data, "vipStatus", 0);
    char *vip_label = jstr(cJSON_GetObjectItemCaseSensitive(data, "vipLabel"),
                           "text");
    long long coin = jint(data, "money", -1);
    printf("已登录: %s (Lv.%lld)%s%s 硬币: %lld\n",
           uname ? uname : "?", level,
           vip ? " " : "", vip ? (vip_label && *vip_label ? vip_label : "大会员") : "",
           coin);
    free(uname);
    free(vip_label);
    cJSON_Delete(root);
    return 0;
}
