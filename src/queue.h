/* queue.h - TUI 下载任务队列引擎（UI 无关，无线程） */
#ifndef BILI_CLI_QUEUE_H
#define BILI_CLI_QUEUE_H

#include <stdint.h>
#include <sys/types.h>

typedef enum {
    JOB_PENDING = 0,   /* 等待开始 */
    JOB_DL_VIDEO,      /* 下载视频流 */
    JOB_DL_AUDIO,      /* 下载音频流 */
    JOB_MUXING,        /* ffmpeg 混流 */
    JOB_DONE,
    JOB_FAILED,
    JOB_CANCELED,
} job_state_t;

typedef struct {
    /* 任务描述（bq_add 前填好） */
    int kind;              /* 0=普通视频分P 1=番剧集 */
    char bvid[16];
    long cid;
    long ep_id;            /* kind==1 有效 */
    char title[512];       /* 视频/番剧标题（用于目录名） */
    char part[256];        /* 分P/集名（用于文件名与显示） */
    int page_no;           /* 分P号（kind==0） */
    int npages;            /* 总分P数（决定文件名是否带 _P<n>） */
    char outdir[1024];     /* 输出根目录 */
    int qn;                /* 0=最高可用 */
    int no_mux;            /* 保留分离流 */
    int audio_only;
    int video_only;
    int mp4;

    /* 引擎内部状态 */
    job_state_t state;
    char note[96];         /* 当前状态描述（给 UI 显示） */
    char err[256];
    char dir[1200];        /* 实际输出目录（outdir/标题） */
    char base[600];
    char vpath[1900], apath[1900], outpath[2000];
    char *vurl, *aurl;
    uint64_t vsize, asize; /* 期望大小（API 可能不给，0=未知） */
    int attempts;          /* 当前阶段重试次数 */
    pid_t pid;             /* 活动 curl/ffmpeg 子进程 */
    int has_child;
} bjob_t;

typedef struct {
    bjob_t *jobs;
    int n, cap;
    char *cookie;          /* 借用，不释放 */
    char *img_key, *sub_key;
    int verbose;
    int max_active;
} bqueue_t;

void bq_init(bqueue_t *q, char *cookie, char *img_key, char *sub_key, int verbose);
/* 追加任务（desc 内容拷贝），返回任务下标 */
int  bq_add(bqueue_t *q, const bjob_t *desc);
/* 非阻塞推进：提升等待任务、检查子进程、更新进度 */
void bq_tick(bqueue_t *q);
/* 取消任务（终止子进程，保留已下载部分） */
void bq_cancel(bqueue_t *q, int idx);
/* 清除已结束（DONE/FAILED/CANCELED）的任务 */
void bq_remove_finished(bqueue_t *q);
int  bq_active_count(const bqueue_t *q);

#endif
