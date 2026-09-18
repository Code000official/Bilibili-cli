/* bangumi.c - 番剧 API
 *
 * 端点：
 *   GET /pgc/view/web/season?season_id= | ?ep_id=   （外层 result）
 *   GET /pgc/player/web/playurl?cid=&qn=&fnval=4048&ep_id=  （无需 WBI 签名）
 */
#include "bangumi.h"
#include "http.h"
#include "util.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_SEASON "https://api.bilibili.com/pgc/view/web/season"
#define API_PGC_PLAY "https://api.bilibili.com/pgc/player/web/playurl"

/* 复用 bili.c 的 cJSON 辅助逻辑（此处独立实现，保持模块自洽） */
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

int bangumi_season(long season_id, long ep_id, const char *cookie,
                   int verbose, bangumi_season_t *out)
{
    memset(out, 0, sizeof(*out));
    if (season_id <= 0 && ep_id <= 0) {
        return -1;
    }
    char id_s[32];
    if (season_id > 0) {
        snprintf(id_s, sizeof(id_s), "season_id=%ld", season_id);
    } else {
        snprintf(id_s, sizeof(id_s), "ep_id=%ld", ep_id);
    }
    char *url = xmalloc(strlen(API_SEASON) + strlen(id_s) + 2);
    sprintf(url, "%s?%s", API_SEASON, id_s);

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", url);
    }
    char *body = NULL;
    if (http_get_str(url, cookie, &body) != 0) {
        fprintf(stderr, "错误: 番剧信息请求失败\n");
        free(url);
        return -1;
    }
    free(url);

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "错误: 番剧信息响应解析失败\n");
        return -1;
    }
    long long code = jint(root, "code", -1);
    if (code != 0) {
        char *msg = jstr(root, "message");
        fprintf(stderr, "错误: 获取番剧信息失败 (%lld): %s\n", code, msg ? msg : "未知错误");
        free(msg);
        cJSON_Delete(root);
        return -1;
    }
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");

    out->season_id = (long)jint(result, "season_id", 0);
    out->title = jstr(result, "title");

    cJSON *eps = cJSON_GetObjectItemCaseSensitive(result, "episodes");
    int n = cJSON_IsArray(eps) ? cJSON_GetArraySize(eps) : 0;
    if (n == 0) {
        fprintf(stderr, "错误: 该番剧没有可用的剧集列表\n");
        cJSON_Delete(root);
        return -1;
    }
    out->neps = n;
    out->episodes = xmalloc((size_t)n * sizeof(bangumi_episode_t));
    int i = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, eps)
    {
        bangumi_episode_t *ep = &out->episodes[i++];
        memset(ep, 0, sizeof(*ep));
        ep->id = (long)jint(it, "id", 0);
        ep->cid = (long)jint(it, "cid", 0);
        ep->aid = jint(it, "aid", 0);
        ep->bvid = jstr(it, "bvid");
        ep->title = jstr(it, "title");
        ep->long_title = jstr(it, "long_title");
        /* pgc 接口的 duration 为毫秒；过小的值按秒兜底 */
        long long d = jint(it, "duration", 0);
        ep->duration = d > 100000 ? (long)(d / 1000) : (long)d;
    }
    cJSON_Delete(root);
    return 0;
}

int bangumi_playurl(long cid, long ep_id, int qn, const char *cookie,
                    int verbose, bili_playurl_t *out)
{
    char q[160];
    snprintf(q, sizeof(q),
             "cid=%ld&ep_id=%ld&fnval=4048&fnver=0&fourk=1&qn=%d",
             cid, ep_id, qn);
    char *url = xmalloc(strlen(API_PGC_PLAY) + strlen(q) + 2);
    sprintf(url, "%s?%s", API_PGC_PLAY, q);

    if (verbose) {
        fprintf(stderr, "[verbose] GET %s\n", url);
    }
    char *body = NULL;
    if (http_get_str(url, cookie, &body) != 0) {
        fprintf(stderr, "错误: 番剧播放地址请求失败\n");
        free(url);
        return -1;
    }
    free(url);
    return bili_parse_playurl_json(body, out);
}

void bangumi_season_free(bangumi_season_t *s)
{
    if (!s) {
        return;
    }
    free(s->title);
    for (int i = 0; i < s->neps; i++) {
        free(s->episodes[i].bvid);
        free(s->episodes[i].title);
        free(s->episodes[i].long_title);
    }
    free(s->episodes);
    memset(s, 0, sizeof(*s));
}
