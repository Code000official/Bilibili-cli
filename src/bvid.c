/* bvid.c - 输入解析与 av/BV 号转换
 *
 * 转换算法：
 *   XorCode = 23442827791579, MaskCode = 2^51 - 1, Base58 字母表
 *   "FcwAPNKTMug3GV5Lj7EJnHpWsx4tb8haYeviqBz6rkCy12mUSDQX9RdoZf"
 */
#include "bvid.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BVID_LEN 12 /* "BV" + 10 字符 */

static const char B58[] = "FcwAPNKTMug3GV5Lj7EJnHpWsx4tb8haYeviqBz6rkCy12mUSDQX9RdoZf";

char *bvid_av2bv(uint64_t aid)
{
    char bytes[BVID_LEN] = { 'B', 'V', '1', '0', '0', '0', '0', '0', '0', '0', '0', '0' };
    int idx = BVID_LEN - 1;

    uint64_t tmp = ((1ULL << 51) | aid) ^ 23442827791579ULL;
    while (tmp > 0) {
        bytes[idx--] = B58[tmp % 58];
        tmp /= 58;
    }

    char t = bytes[3];
    bytes[3] = bytes[9];
    bytes[9] = t;
    t = bytes[4];
    bytes[4] = bytes[7];
    bytes[7] = t;

    return xstrndup(bytes, BVID_LEN);
}

uint64_t bvid_bv2av(const char *bvid)
{
    if (strlen(bvid) < BVID_LEN) {
        return 0;
    }
    char b[BVID_LEN + 1];
    memcpy(b, bvid, BVID_LEN);
    b[BVID_LEN] = '\0';

    char t = b[3];
    b[3] = b[9];
    b[9] = t;
    t = b[4];
    b[4] = b[7];
    b[7] = t;

    uint64_t tmp = 0;
    for (int i = 3; i < BVID_LEN; i++) {
        const char *p = strchr(B58, b[i]);
        if (!p || b[i] == '\0') {
            return 0;
        }
        tmp = tmp * 58 + (uint64_t)(p - B58);
    }
    return (tmp & 2251799813685247ULL) ^ 23442827791579ULL;
}

/* 校验从 pos 开始是否为合法 BV 号，是则返回 malloc 拷贝 */
static char *try_bv_at(const char *s)
{
    if (s[0] != 'B' || s[1] != 'V') {
        return NULL;
    }
    for (int i = 2; i < BVID_LEN; i++) {
        if (!isalnum((unsigned char)s[i])) {
            return NULL;
        }
    }
    return xstrndup(s, BVID_LEN);
}

/* 从字符串中提取 "?...&p=N" 的分P号 */
static int extract_page_param(const char *s)
{
    const char *q = strchr(s, '?');
    if (!q) {
        return 0;
    }
    for (const char *p = q + 1; *p;) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > 2 && p[0] == 'p' && p[1] == '=') {
            int v = atoi(p + 2);
            return v > 0 ? v : 0;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return 0;
}

bvid_result_t bvid_parse_input(const char *input, char **bvid_out,
                               int *page_out, long *num_out)
{
    *bvid_out = NULL;
    *page_out = 0;
    *num_out = 0;
    while (*input == ' ' || *input == '\t') {
        input++;
    }
    size_t len = strlen(input);
    if (len == 0) {
        return BVID_NOT_RECOGNIZED;
    }

    /* 课程 */
    if (strstr(input, "/cheese/") || strncmp(input, "che", 3) == 0) {
        return BVID_UNSUPPORTED;
    }

    /* 纯 ep/ss 号（大小写前缀均可） */
    if (((input[0] == 'e' || input[0] == 'E') &&
         (input[1] == 'p' || input[1] == 'P') && isdigit((unsigned char)input[2])) ||
        ((input[0] == 's' || input[0] == 'S') &&
         (input[1] == 's' || input[1] == 'S') && isdigit((unsigned char)input[2]))) {
        long id = atol(input + 2);
        if (id > 0) {
            *num_out = id;
            return (input[0] == 'e' || input[0] == 'E') ? BVID_EP : BVID_SS;
        }
    }

    /* 番剧 URL: /bangumi/play/ep123 或 /bangumi/play/ss123 */
    const char *bg = strstr(input, "/bangumi/play/");
    if (bg) {
        const char *p = bg + strlen("/bangumi/play/");
        if ((*p == 'e' || *p == 'E') && isdigit((unsigned char)p[1])) {
            *num_out = atol(p + 1);
            return BVID_EP;
        }
        if ((*p == 's' || *p == 'S') && isdigit((unsigned char)p[1])) {
            *num_out = atol(p + 1);
            return BVID_SS;
        }
    }
    /* 查询参数 ?ep= / &ep= / ?ss= / &ss=（部分分享链接形式） */
    {
        const char *p = input;
        while ((p = strpbrk(p, "?&")) != NULL) {
            p++;
            if (strncmp(p, "ep=", 3) == 0 && isdigit((unsigned char)p[3])) {
                *num_out = atol(p + 3);
                return BVID_EP;
            }
            if (strncmp(p, "ss=", 3) == 0 && isdigit((unsigned char)p[3])) {
                *num_out = atol(p + 3);
                return BVID_SS;
            }
        }
    }

    /* 纯 BV 号（大小写前缀均可） */
    if ((input[0] == 'B' || input[0] == 'b') &&
        (input[1] == 'V' || input[1] == 'v')) {
        char *bv = try_bv_at(input);
        if (bv) {
            *bvid_out = bv;
            return BVID_OK;
        }
    }

    /* av 号 */
    if ((input[0] == 'a' || input[0] == 'A') && input[1] == 'v' &&
        isdigit((unsigned char)input[2])) {
        uint64_t aid = strtoull(input + 2, NULL, 10);
        if (aid > 0) {
            *bvid_out = bvid_av2bv(aid);
            return BVID_OK;
        }
    }
    /* 纯数字视为 aid */
    int all_digit = 1;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)input[i])) {
            all_digit = 0;
            break;
        }
    }
    if (all_digit) {
        uint64_t aid = strtoull(input, NULL, 10);
        if (aid > 0) {
            *bvid_out = bvid_av2bv(aid);
            return BVID_OK;
        }
    }

    /* URL：在任意位置找 BV 号，并提取 ?p= 参数 */
    for (const char *p = input; *p; p++) {
        if (p[0] == 'B' && p[1] == 'V') {
            char *bv = try_bv_at(p);
            if (bv) {
                *bvid_out = bv;
                *page_out = extract_page_param(p);
                return BVID_OK;
            }
        }
    }
    /* URL 中的 av 号：/video/av123 */
    const char *av = strstr(input, "/video/av");
    if (av && isdigit((unsigned char)av[9])) {
        uint64_t aid = strtoull(av + 9, NULL, 10);
        if (aid > 0) {
            *bvid_out = bvid_av2bv(aid);
            *page_out = extract_page_param(av);
            return BVID_OK;
        }
    }

    return BVID_NOT_RECOGNIZED;
}
