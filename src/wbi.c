/* wbi.c - Bilibili WBI 签名
 *
 * 算法：
 * 1. mixin_key = 重排(img_key + sub_key) 前 32 字符
 * 2. 参数追加 wts=当前秒级时间戳
 * 3. 按 key 字典序排序，value 过滤 "!'()*"
 * 4. w_rid = md5(urlencode(query) + mixin_key)
 */
#include "wbi.h"
#include "md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 打乱重排实时口令的置换表 */
static const int MIXIN_TAB[64] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
    33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40,
    61, 26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11,
    36, 20, 34, 44, 52
};

static void get_mixin_key(const char *origin, char out[33])
{
    for (int i = 0; i < 32; i++) {
        out[i] = origin[MIXIN_TAB[i]];
    }
    out[32] = '\0';
}

static int kv_cmp(const void *a, const void *b)
{
    return strcmp(((const kv_t *)a)->key, ((const kv_t *)b)->key);
}

int wbi_sign_query_wts(const char *img_key, const char *sub_key,
                       kv_t *params, size_t nparams, const char *wts_in,
                       char **out_query)
{
    *out_query = NULL;
    char origin[65];
    snprintf(origin, sizeof(origin), "%s%s", img_key, sub_key);
    if (strlen(origin) < 64) {
        return -1;
    }
    char mixin[33];
    get_mixin_key(origin, mixin);

    /* 复制参数并追加 wts */
    kv_t *ps = xmalloc(nparams * sizeof(kv_t) + sizeof(kv_t));
    size_t n = 0;
    for (size_t i = 0; i < nparams; i++) {
        ps[n].key = xstrdup(params[i].key);
        /* 过滤 "!'()*" 字符 */
        size_t vl = strlen(params[i].val);
        char *v = xmalloc(vl + 1);
        size_t j = 0;
        for (size_t k = 0; k < vl; k++) {
            if (!strchr("!'()*", params[i].val[k])) {
                v[j++] = params[i].val[k];
            }
        }
        v[j] = '\0';
        ps[n].val = v;
        n++;
    }
    char wts[24];
    if (wts_in) {
        snprintf(wts, sizeof(wts), "%s", wts_in);
    } else {
        snprintf(wts, sizeof(wts), "%lld", (long long)time(NULL));
    }
    ps[n].key = xstrdup("wts");
    ps[n].val = xstrdup(wts);
    n++;

    qsort(ps, n, sizeof(kv_t), kv_cmp);

    /* 构造 query 并计算 w_rid */
    strbuf_t query;
    sb_init(&query);
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            sb_append(&query, "&");
        }
        char *ek = url_encode(ps[i].key);
        char *ev = url_encode(ps[i].val);
        sb_appendf(&query, "%s=%s", ek, ev);
        free(ek);
        free(ev);
    }

    strbuf_t sign_src;
    sb_init(&sign_src);
    sb_appendf(&sign_src, "%s%s", query.data, mixin);
    char wrid[33];
    md5_hex((const uint8_t *)sign_src.data, sign_src.len, wrid);
    sb_free(&sign_src);

    sb_appendf(&query, "&w_rid=%s", wrid);

    for (size_t i = 0; i < n; i++) {
        free(ps[i].key);
        free(ps[i].val);
    }
    free(ps);

    *out_query = query.data;
    return 0;
}

int wbi_sign_query(const char *img_key, const char *sub_key,
                   kv_t *params, size_t nparams, char **out_query)
{
    return wbi_sign_query_wts(img_key, sub_key, params, nparams, NULL, out_query);
}
