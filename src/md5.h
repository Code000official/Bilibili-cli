/* md5.h - RFC 1321 MD5 摘要（WBI 签名用） */
#ifndef BILI_CLI_MD5_H
#define BILI_CLI_MD5_H

#include <stddef.h>
#include <stdint.h>

/* 计算 MD5，输出 16 字节二进制摘要 */
void md5_digest(const uint8_t *data, size_t len, uint8_t out[16]);

/* 计算 MD5，输出 32 字符小写十六进制串（含 '\0'，需 33 字节） */
void md5_hex(const uint8_t *data, size_t len, char out[33]);

#endif
