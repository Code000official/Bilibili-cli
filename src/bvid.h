/* bvid.h - 输入解析与 av/BV 号转换 */
#ifndef BILI_CLI_BVID_H
#define BILI_CLI_BVID_H

#include <stdint.h>

/* bvid_parse_input 的返回码 */
typedef enum {
    BVID_OK = 0,          /* 普通视频，*bvid_out 有效 */
    BVID_EP,              /* 番剧单集，*num_out 为 ep id */
    BVID_SS,              /* 番剧整季，*num_out 为 ss id */
    BVID_NOT_RECOGNIZED,  /* 无法识别的输入 */
    BVID_UNSUPPORTED,     /* 识别到但不支持的类型（课程等） */
} bvid_result_t;

/*
 * 解析用户输入（URL / BV号 / av号 / 纯数字 / ep号 / ss号）。
 * BVID_OK 时 *bvid_out 为 malloc 的 "BVxxxx" 字符串；
 * BVID_EP/BVID_SS 时 *num_out 为对应 id；
 * *page_out: 0=未指定, -1=全部分P, >0=指定分P（仅普通视频）。
 */
bvid_result_t bvid_parse_input(const char *input, char **bvid_out,
                               int *page_out, long *num_out);

/* aid -> bvid（返回 malloc 字符串） */
char *bvid_av2bv(uint64_t aid);

/* bvid -> aid，非法输入返回 0 */
uint64_t bvid_bv2av(const char *bvid);

#endif
