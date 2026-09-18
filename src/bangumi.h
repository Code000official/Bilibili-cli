/* bangumi.h - 番剧 API（季信息与播放地址） */
#ifndef BILI_CLI_BANGUMI_H
#define BILI_CLI_BANGUMI_H

#include "bili.h"

typedef struct {
    long id;          /* ep id */
    long cid;
    long long aid;
    char *bvid;
    char *title;      /* 集数序号，如 "1" */
    char *long_title; /* 集名，如 "吹一曲青蛙的歌" */
    long duration;    /* 秒 */
} bangumi_episode_t;

typedef struct {
    long season_id;
    char *title;
    bangumi_episode_t *episodes;
    int neps;
} bangumi_season_t;

/*
 * 获取剧集明细。season_id 与 ep_id 至少传一个（>0），
 * 传 ep_id 时返回其所属季。返回 0 成功。
 */
int bangumi_season(long season_id, long ep_id, const char *cookie,
                   int verbose, bangumi_season_t *out);

/*
 * 获取番剧播放地址（pgc 接口，无需 WBI 签名）。
 * 返回 0 成功，结构体与普通视频共用。
 */
int bangumi_playurl(long cid, long ep_id, int qn, const char *cookie,
                    int verbose, bili_playurl_t *out);

void bangumi_season_free(bangumi_season_t *s);

#endif
