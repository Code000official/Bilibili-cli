/* wbi.h - Bilibili WBI 签名 */
#ifndef BILI_CLI_WBI_H
#define BILI_CLI_WBI_H

#include "util.h"

/*
 * 对参数做 WBI 签名，输出完整 query 串：
 *   "k1=v1&k2=v2&...&wts=<ts>&w_rid=<md5>"
 * 参数顺序按 key 字典序排列，value 过滤 "!'()*" 字符并做百分号编码。
 * 返回 0 成功，*out_query 为 malloc 的字符串。
 * wts 指定时间戳；传 NULL 则使用当前时间。
 */
int wbi_sign_query_wts(const char *img_key, const char *sub_key,
                       kv_t *params, size_t nparams, const char *wts,
                       char **out_query);

/* 同上，wts 使用当前时间 */
int wbi_sign_query(const char *img_key, const char *sub_key,
                   kv_t *params, size_t nparams, char **out_query);

#endif
