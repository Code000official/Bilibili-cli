/* bili.c - Bilibili API 层
 *
 * 请求链路：
 *   finger/spi 取设备指纹 -> nav 取 WBI 密钥 -> WBI 签名请求 view/playurl。
 * buvid 与 WBI 密钥缓存在 $XDG_CACHE_HOME/bili-cli/ 下减少请求次数。
 */
#include "bili.h"
#include "http.h"
#include "util.h"
#include "wbi.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define API_SPI   "https://api.bilibili.com/x/frontend/finger/spi"
#define API_NAV   "https://api.bilibili.com/x/web-interface/nav"
#define API_VIEW  "https://api.bilibili.com/x/web-interface/wbi/view"
#define API_PLAY  "https://api.bilibili.com/x/player/wbi/playurl"

#define WBI_CACHE_SECONDS (6 * 3600)

struct qn_name {
    int qn;
    const char *name;
};

static const struct qn_name QN_NAMES[] = {
    { 127, "8K 超高清" },
    { 126, "杜比视界" },
    { 125, "HDR 真彩" },
    { 120, "4K 超清" },
    { 116, "1080P 60帧" },
    { 112, "1080P 高码率" },
    { 80,  "1080P 高清" },
    { 74,  "720P 60帧" },
    { 64,  "720P 高清" },
    { 32,  "480P 清晰" },
    { 16,  "360P 流畅" },
};

const char *bili_qn_name(int qn)
{
    for (size_t i = 0; i < sizeof(QN_NAMES) / sizeof(QN_NAMES[0]); i++) {
        if (QN_NAMES[i].qn == qn) {
            return QN_NAMES[i].name;
        }
    }
    return "未知";
}

/* ---------- 缓存 ---------- */

static char *cache_dir(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char *dir = xmalloc(strlen(xdg ? xdg : home) + 32);
    if (xdg) {
        sprintf(dir, "%s/bili-cli", xdg);
    } else {
        sprintf(dir, "%s/.cache/bili-cli", home);
    }
    return dir;
}

/* 读缓存文件；每行一个字段，返回字段数（文件不存在返回 -1） */
static int cache_read(const char *name, char ***fields_out)
{
    char *dir = cache_dir();
    char *path = xmalloc(strlen(dir) + strlen(name) + 2);
    sprintf(path, "%s/%s", dir, name);
    free(dir);

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) {
        return -1;
    }

    char **fields = NULL;
    int n = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        fields = xrealloc(fields, (size_t)(n + 1) * sizeof(char *));
        fields[n++] = xstrdup(line);
    }
    fclose(fp);
    *fields_out = fields;
    return n;
}

static void cache_write(const char *name, const char *const *fields, int n)
{
    char *dir = cache_dir();
    mkdir_p(dir);
    char *path = xmalloc(strlen(dir) + strlen(name) + 2);
    sprintf(path, "%s/%s", dir, name);
    free(dir);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(path);
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(fp, "%s\n", fields[i]);
    }
    fclose(fp);
    free(path);
}

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

/* DASH/durl 流的 URL：兼容 baseUrl / base_url / url 三种字段名 */
static char *jstream_url(cJSON *item)
{
    char *u = jstr(item, "baseUrl");
    if (!u) {
        u = jstr(item, "base_url");
    }
    if (!u) {
        u = jstr(item, "url");
    }
    /* 协议相对地址补全 */
    if (u && u[0] == '/') {
        size_t l = strlen(u);
        char *full = xmalloc(l + 6);
        memcpy(full, "https:", 6);
        memcpy(full + 6, u, l + 1);
        free(u);
        u = full;
    }
    return u;
}

static void parse_stream(cJSON *item, bili_stream_t *s)
{
    memset(s, 0, sizeof(*s));
    s->id = (int)jint(item, "id", 0);
    s->width = (int)jint(item, "width", 0);
    s->height = (int)jint(item, "height", 0);
    s->size = (uint64_t)jint(item, "size", 0);
    s->bandwidth = (uint64_t)jint(item, "bandwidth", 0);
    s->codecs = jstr(item, "codecs");
    s->url = jstream_url(item);
}

/* ---------- buvid ---------- */

int bili_get_buvid(char **b3, char **b4, int verbose)
{
    *b3 = *b4 = NULL;

    char **f = NULL;
    int n = cache_read("buvid.txt", &f);
    if (n >= 2 && f[0][0] && f[1][0]) {
        *b3 = xstrdup(f[0]);
        *b4 = xstrdup(f[1]);
        for (int i = 0; i < n; i++) {
            free(f[i]);
        }
        free(f);
        return 0;
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            free(f[i]);
        }
        free(f);
    }

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", API_SPI);
    }
    char *body = NULL;
    if (http_get_str(API_SPI, NULL, &body) != 0) {
        fprintf(stderr, "警告: 获取设备指纹失败，将匿名继续\n");
        return -1;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "警告: 设备指纹响应解析失败\n");
        return -1;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    char *v3 = jstr(data, "b_3");
    char *v4 = jstr(data, "b_4");
    cJSON_Delete(root);
    if (!v3 || !v4) {
        fprintf(stderr, "警告: 设备指纹响应缺少字段\n");
        free(v3);
        free(v4);
        return -1;
    }

    *b3 = v3;
    *b4 = v4;
    const char *fields[2] = { v3, v4 };
    cache_write("buvid.txt", fields, 2);
    return 0;
}

/* ---------- WBI 密钥 ---------- */

/* 从 "https://i0.hdslb.com/bfs/wbi/<key>.png" 提取 <key> */
static char *wbi_key_from_url(const char *url)
{
    if (!url) {
        return NULL;
    }
    const char *slash = strrchr(url, '/');
    const char *dot = strrchr(url, '.');
    if (!slash || !dot || dot < slash) {
        return NULL;
    }
    return xstrndup(slash + 1, (size_t)(dot - slash - 1));
}

int bili_get_wbi_keys(const char *cookie, char **img, char **sub, int verbose)
{
    *img = *sub = NULL;

    char **f = NULL;
    int n = cache_read("wbi.txt", &f);
    if (n >= 3 && f[0][0] && f[1][0]) {
        long long ts = atoll(f[2]);
        if (time(NULL) - (time_t)ts < WBI_CACHE_SECONDS) {
            *img = xstrdup(f[0]);
            *sub = xstrdup(f[1]);
            for (int i = 0; i < n; i++) {
                free(f[i]);
            }
            free(f);
            return 0;
        }
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            free(f[i]);
        }
        free(f);
    }

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", API_NAV);
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
    /* 未登录时 code 为 -101，但 data.wbi_img 仍然有效 */
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *wbi = cJSON_GetObjectItemCaseSensitive(data, "wbi_img");
    char *img_url = jstr(wbi, "img_url");
    char *sub_url = jstr(wbi, "sub_url");
    cJSON_Delete(root);

    *img = wbi_key_from_url(img_url);
    *sub = wbi_key_from_url(sub_url);
    free(img_url);
    free(sub_url);
    if (!*img || !*sub) {
        fprintf(stderr, "错误: 获取 WBI 密钥失败\n");
        return -1;
    }

    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    const char *fields[3] = { *img, *sub, ts };
    cache_write("wbi.txt", fields, 3);
    return 0;
}

/* ---------- 视频信息 ---------- */

int bili_view(const char *bvid, const char *cookie,
              const char *img_key, const char *sub_key,
              int verbose, bili_view_t *out)
{
    memset(out, 0, sizeof(*out));
    kv_t *params = NULL;
    size_t np = 0;
    kv_add(&params, &np, "bvid", bvid);

    char *query = NULL;
    if (wbi_sign_query(img_key, sub_key, params, np, &query) != 0) {
        kv_free(params, np);
        return -1;
    }
    kv_free(params, np);

    char *url = xmalloc(strlen(API_VIEW) + strlen(query) + 2);
    sprintf(url, "%s?%s", API_VIEW, query);
    free(query);

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", url);
    }
    char *body = NULL;
    int rc = http_get_str(url, cookie, &body);
    free(url);
    if (rc != 0) {
        fprintf(stderr, "错误: 视频信息请求失败\n");
        return -1;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "错误: 视频信息响应解析失败\n");
        return -1;
    }
    long long code = jint(root, "code", -1);
    if (code != 0) {
        char *msg = jstr(root, "message");
        fprintf(stderr, "错误: 获取视频信息失败 (%lld): %s\n", code, msg ? msg : "未知错误");
        free(msg);
        cJSON_Delete(root);
        return -1;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    out->bvid = jstr(data, "bvid");
    if (!out->bvid) {
        out->bvid = xstrdup(bvid);
    }
    out->aid = jint(data, "aid", 0);
    out->title = jstr(data, "title");
    out->duration = (long)jint(data, "duration", 0);
    cJSON *owner = cJSON_GetObjectItemCaseSensitive(data, "owner");
    out->owner = jstr(owner, "name");

    cJSON *pages = cJSON_GetObjectItemCaseSensitive(data, "pages");
    int npages = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    out->npages = npages;
    if (npages > 0) {
        out->pages = xmalloc((size_t)npages * sizeof(bili_page_t));
        int i = 0;
        cJSON *pg;
        cJSON_ArrayForEach(pg, pages)
        {
            out->pages[i].cid = (long)jint(pg, "cid", 0);
            out->pages[i].page = (int)jint(pg, "page", i + 1);
            out->pages[i].part = jstr(pg, "part");
            i++;
        }
    }
    cJSON_Delete(root);
    return (out->npages > 0) ? 0 : -1;
}

void bili_view_free(bili_view_t *v)
{
    if (!v) {
        return;
    }
    free(v->bvid);
    free(v->title);
    free(v->owner);
    for (int i = 0; i < v->npages; i++) {
        free(v->pages[i].part);
    }
    free(v->pages);
    memset(v, 0, sizeof(*v));
}

/* ---------- 播放地址 ---------- */

/* 解析 playurl 响应体，普通视频与番剧共用 */
int bili_parse_playurl_json(const char *body, bili_playurl_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        fprintf(stderr, "错误: 播放地址响应解析失败\n");
        return -1;
    }
    long long code = jint(root, "code", -1);
    if (code != 0) {
        char *msg = jstr(root, "message");
        fprintf(stderr, "错误: 获取播放地址失败 (%lld): %s\n", code, msg ? msg : "未知错误");
        free(msg);
        cJSON_Delete(root);
        return -1;
    }
    /* 普通视频外层为 data，番剧为 result */
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        data = cJSON_GetObjectItemCaseSensitive(root, "result");
    }

    out->quality = (int)jint(data, "quality", 0);
    out->is_preview = (int)jint(data, "is_preview", 0);

    cJSON *accept = cJSON_GetObjectItemCaseSensitive(data, "accept_quality");
    if (cJSON_IsArray(accept)) {
        out->naccept = cJSON_GetArraySize(accept);
        out->accept = xmalloc((size_t)out->naccept * sizeof(int));
        int i = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, accept)
        {
            out->accept[i++] = (int)cJSON_GetNumberValue(it);
        }
    }

    cJSON *dash = cJSON_GetObjectItemCaseSensitive(data, "dash");
    if (cJSON_IsObject(dash)) {
        cJSON *videos = cJSON_GetObjectItemCaseSensitive(dash, "video");
        if (cJSON_IsArray(videos)) {
            out->nvideos = cJSON_GetArraySize(videos);
            out->videos = xmalloc((size_t)out->nvideos * sizeof(bili_stream_t));
            int i = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, videos)
            {
                parse_stream(it, &out->videos[i++]);
            }
        }
        cJSON *audios = cJSON_GetObjectItemCaseSensitive(dash, "audio");
        if (cJSON_IsArray(audios)) {
            out->naudios = cJSON_GetArraySize(audios);
            out->audios = xmalloc((size_t)out->naudios * sizeof(bili_stream_t));
            int i = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, audios)
            {
                parse_stream(it, &out->audios[i++]);
            }
        }
        /* 无损/杜比音轨兜底 */
        if (out->naudios == 0) {
            cJSON *flac = cJSON_GetObjectItemCaseSensitive(dash, "flac");
            cJSON *fa = cJSON_GetObjectItemCaseSensitive(flac, "audio");
            if (cJSON_IsObject(fa)) {
                out->naudios = 1;
                out->audios = xmalloc(sizeof(bili_stream_t));
                parse_stream(fa, &out->audios[0]);
            }
        }
    }

    /* durl 兜底（老视频） */
    cJSON *durl = cJSON_GetObjectItemCaseSensitive(data, "durl");
    if (cJSON_IsArray(durl)) {
        out->ndurls = cJSON_GetArraySize(durl);
        out->durls = xmalloc((size_t)out->ndurls * sizeof(char *));
        out->dsizes = xmalloc((size_t)out->ndurls * sizeof(uint64_t));
        int i = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, durl)
        {
            out->durls[i] = jstream_url(it);
            out->dsizes[i] = (uint64_t)jint(it, "size", 0);
            i++;
        }
    }

    cJSON_Delete(root);
    if (out->nvideos == 0 && out->ndurls == 0) {
        fprintf(stderr, "错误: 响应中没有可下载的媒体流\n");
        return -1;
    }
    return 0;
}

int bili_playurl(const char *bvid, long cid, int qn, const char *cookie,
                 const char *img_key, const char *sub_key,
                 int verbose, bili_playurl_t *out)
{
    char cid_s[24], qn_s[16];
    snprintf(cid_s, sizeof(cid_s), "%ld", cid);
    snprintf(qn_s, sizeof(qn_s), "%d", qn);

    kv_t *params = NULL;
    size_t np = 0;
    kv_add(&params, &np, "bvid", bvid);
    kv_add(&params, &np, "cid", cid_s);
    kv_add(&params, &np, "fnval", "4048");
    kv_add(&params, &np, "fnver", "0");
    kv_add(&params, &np, "fourk", "1");
    kv_add(&params, &np, "qn", qn_s);

    char *query = NULL;
    if (wbi_sign_query(img_key, sub_key, params, np, &query) != 0) {
        kv_free(params, np);
        return -1;
    }
    kv_free(params, np);

    char *url = xmalloc(strlen(API_PLAY) + strlen(query) + 2);
    sprintf(url, "%s?%s", API_PLAY, query);
    free(query);

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", url);
    }
    char *body = NULL;
    int rc = http_get_str(url, cookie, &body);
    free(url);
    if (rc != 0) {
        fprintf(stderr, "错误: 播放地址请求失败\n");
        return -1;
    }
    rc = bili_parse_playurl_json(body, out);
    free(body);
    return rc;
}

void bili_playurl_free(bili_playurl_t *p)
{
    if (!p) {
        return;
    }
    free(p->accept);
    for (int i = 0; i < p->nvideos; i++) {
        free(p->videos[i].codecs);
        free(p->videos[i].url);
    }
    free(p->videos);
    for (int i = 0; i < p->naudios; i++) {
        free(p->audios[i].codecs);
        free(p->audios[i].url);
    }
    free(p->audios);
    for (int i = 0; i < p->ndurls; i++) {
        free(p->durls[i]);
    }
    free(p->durls);
    free(p->dsizes);
    memset(p, 0, sizeof(*p));
}
