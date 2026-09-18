/* bili.h - Bilibili API 层（buvid/WBI 缓存、视频信息、播放地址） */
#ifndef BILI_CLI_BILI_H
#define BILI_CLI_BILI_H

#include <stdint.h>

/* 清晰度名称（B 站 qn 定义） */
const char *bili_qn_name(int qn);

typedef struct {
    long cid;
    int page;
    char *part;
} bili_page_t;

typedef struct {
    char *bvid;
    long long aid;
    char *title;
    char *owner;
    long duration; /* 秒 */
    bili_page_t *pages;
    int npages;
} bili_view_t;

typedef struct {
    int id; /* 视频为 qn，音频为音频流 id */
    int width;
    int height;
    uint64_t size;
    uint64_t bandwidth; /* bit/s */
    char *codecs;
    char *url;
} bili_stream_t;

typedef struct {
    int quality;      /* 实际返回的清晰度 */
    int *accept;      /* 可用清晰度列表 */
    int naccept;
    int is_preview;   /* 番剧预览（试看）标记 */
    bili_stream_t *videos;
    int nvideos;
    bili_stream_t *audios;
    int naudios;
    char **durls;     /* durl 兜底（旧视频无 DASH） */
    uint64_t *dsizes;
    int ndurls;
} bili_playurl_t;

/*
 * 获取设备指纹 buvid3/buvid4（本地缓存，通常一次获取长期使用）。
 * 失败返回非 0，此时可将两者置 NULL（匿名降级）。
 */
int bili_get_buvid(char **b3, char **b4, int verbose);

/*
 * 获取 WBI 签名密钥（本地缓存 6 小时，未登录时 nav 也返回密钥）。
 */
int bili_get_wbi_keys(const char *cookie, char **img, char **sub, int verbose);

/* 获取视频信息。返回 0 成功 */
int bili_view(const char *bvid, const char *cookie,
              const char *img_key, const char *sub_key,
              int verbose, bili_view_t *out);

/* 获取指定分P的播放地址（DASH）。返回 0 成功 */
int bili_playurl(const char *bvid, long cid, int qn, const char *cookie,
                 const char *img_key, const char *sub_key,
                 int verbose, bili_playurl_t *out);

/*
 * 解析 playurl 响应体（普通视频 data / 番剧 result 外层均可），
 * 供番剧模块复用。返回 0 成功。
 */
int bili_parse_playurl_json(const char *body, bili_playurl_t *out);

void bili_view_free(bili_view_t *v);
void bili_playurl_free(bili_playurl_t *p);

#endif
