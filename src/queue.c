/* queue.c - TUI 下载任务队列引擎
 *
 * 设计：无线程。每条任务是一个小状态机，bq_tick() 由 UI 主循环周期调用：
 *   - 任务开始时同步取播放地址（约1秒，一次一条，其余时间非阻塞）
 *   - 下载阶段 fork/exec curl 子进程（stderr 指向 /dev/null 以免干扰 TUI），
 *     waitpid(WNOHANG) 检查完成，stat 部分文件大小计算进度
 *   - 完成后 spawn ffmpeg 混流
 */
#include "queue.h"

#include "bangumi.h"
#include "bili.h"
#include "http.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ATTEMPTS 2

/* 与 main.c 中 pick_video/pick_audio 相同的选流逻辑 */
static const bili_stream_t *q_pick_video(const bili_playurl_t *pu, int want)
{
    if (pu->nvideos == 0) {
        return NULL;
    }
    const bili_stream_t *best = &pu->videos[0];
    for (int i = 1; i < pu->nvideos; i++) {
        const bili_stream_t *s = &pu->videos[i];
        int s_ok = s->id <= want, b_ok = best->id <= want;
        if (s_ok && b_ok) {
            best = s->id > best->id ? s : best;
        } else if (s_ok) {
            best = s;
        } else if (!b_ok) {
            best = s->id < best->id ? s : best;
        }
    }
    return best;
}

static const bili_stream_t *q_pick_audio(const bili_playurl_t *pu)
{
    static const int prefer[] = { 30280, 30232, 30216 };
    for (size_t k = 0; k < sizeof(prefer) / sizeof(prefer[0]); k++) {
        for (int i = 0; i < pu->naudios; i++) {
            if (pu->audios[i].id == prefer[k]) {
                return &pu->audios[i];
            }
        }
    }
    return pu->naudios ? &pu->audios[0] : NULL;
}

static long long fsize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/* ---------- 子进程 ---------- */

/* TUI 模式下子进程输出必须静默，避免破坏屏幕 */
static pid_t spawn_child(char **argv)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        freopen("/dev/null", "w", stdout);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_curl(const char *url, const char *outfile, const char *cookie)
{
    char *argv[40];
    int n = 0;
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"--fail";
    argv[n++] = (char *)"--location";
    argv[n++] = (char *)"--connect-timeout";
    argv[n++] = (char *)"15";
    argv[n++] = (char *)"--speed-time";
    argv[n++] = (char *)"30";
    argv[n++] = (char *)"--speed-limit";
    argv[n++] = (char *)"1024";
    argv[n++] = (char *)"--retry";
    argv[n++] = (char *)"8";
    argv[n++] = (char *)"--retry-all-errors";
    argv[n++] = (char *)"--retry-delay";
    argv[n++] = (char *)"2";
    argv[n++] = (char *)"--continue-at";
    argv[n++] = (char *)"-";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)outfile;
    argv[n++] = (char *)"-A";
    argv[n++] = (char *)HTTP_UA;
    if (cookie && *cookie) {
        static char hdr[8192];
        snprintf(hdr, sizeof(hdr), "Cookie: %s", cookie);
        argv[n++] = (char *)"-H";
        argv[n++] = hdr;
    }
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Referer: https://www.bilibili.com";
    argv[n++] = (char *)"-H";
    argv[n++] = (char *)"Origin: https://www.bilibili.com";
    argv[n++] = (char *)url;
    argv[n] = NULL;
    return spawn_child(argv);
}

static pid_t spawn_mux(const char *v, const char *a, const char *out, int mp4)
{
    char *argv[20];
    int n = 0;
    argv[n++] = (char *)"ffmpeg";
    argv[n++] = (char *)"-y";
    argv[n++] = (char *)"-nostdin";
    argv[n++] = (char *)"-hide_banner";
    argv[n++] = (char *)"-loglevel";
    argv[n++] = (char *)"error";
    argv[n++] = (char *)"-i";
    argv[n++] = (char *)v;
    argv[n++] = (char *)"-i";
    argv[n++] = (char *)a;
    argv[n++] = (char *)"-map";
    argv[n++] = (char *)"0:v:0";
    argv[n++] = (char *)"-map";
    argv[n++] = (char *)"1:a:0";
    argv[n++] = (char *)"-c";
    argv[n++] = (char *)"copy";
    if (mp4) {
        argv[n++] = (char *)"-movflags";
        argv[n++] = (char *)"faststart";
    }
    argv[n++] = (char *)out;
    argv[n] = NULL;
    return spawn_child(argv);
}

/* ---------- 队列基础 ---------- */

void bq_init(bqueue_t *q, char *cookie, char *img_key, char *sub_key, int verbose)
{
    memset(q, 0, sizeof(*q));
    q->cookie = cookie;
    q->img_key = img_key;
    q->sub_key = sub_key;
    q->verbose = verbose;
    q->max_active = 1;
}

int bq_add(bqueue_t *q, const bjob_t *desc)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 8;
        q->jobs = xrealloc(q->jobs, (size_t)q->cap * sizeof(bjob_t));
    }
    bjob_t *j = &q->jobs[q->n++];
    *j = *desc;
    j->state = JOB_PENDING;
    j->note[0] = '\0';
    j->err[0] = '\0';
    j->vurl = j->aurl = NULL;
    j->attempts = 0;
    j->has_child = 0;
    /* 目录与基名在加入时确定 */
    char *st = sanitize_filename(j->title);
    snprintf(j->dir, sizeof(j->dir), "%s/%s", j->outdir, st);
    if (j->kind == 0) {
        if (j->npages > 1) {
            char *sp = sanitize_filename(j->part);
            snprintf(j->base, sizeof(j->base), "%s_P%d_%s", st, j->page_no, sp);
            free(sp);
        } else {
            snprintf(j->base, sizeof(j->base), "%s", st);
        }
    } else {
        char *sp = sanitize_filename(j->part);
        snprintf(j->base, sizeof(j->base), "EP%02d_%s", j->page_no, sp);
        free(sp);
    }
    free(st);
    mkdir_p(j->dir);
    return q->n - 1;
}

int bq_active_count(const bqueue_t *q)
{
    int n = 0;
    for (int i = 0; i < q->n; i++) {
        job_state_t s = q->jobs[i].state;
        if (s != JOB_DONE && s != JOB_FAILED && s != JOB_CANCELED) {
            n++;
        }
    }
    return n;
}

void bq_cancel(bqueue_t *q, int idx)
{
    if (idx < 0 || idx >= q->n) {
        return;
    }
    bjob_t *j = &q->jobs[idx];
    if (j->state == JOB_DONE || j->state == JOB_CANCELED) {
        return;
    }
    if (j->has_child && j->pid > 0) {
        kill(j->pid, SIGTERM);
        waitpid(j->pid, NULL, 0);
        j->has_child = 0;
    }
    free(j->vurl);
    free(j->aurl);
    j->vurl = j->aurl = NULL;
    j->state = JOB_CANCELED;
    snprintf(j->note, sizeof(j->note), "已取消（保留已下载部分）");
}

void bq_remove_finished(bqueue_t *q)
{
    int w = 0;
    for (int i = 0; i < q->n; i++) {
        job_state_t s = q->jobs[i].state;
        if (s == JOB_DONE || s == JOB_FAILED || s == JOB_CANCELED) {
            free(q->jobs[i].vurl);
            free(q->jobs[i].aurl);
        } else {
            q->jobs[w++] = q->jobs[i];
        }
    }
    q->n = w;
}

/* ---------- 任务推进 ---------- */

/* 取播放地址并选流，生成路径；成功则 spawn 第一个下载，返回 0 */
static int job_start(bqueue_t *q, bjob_t *j)
{
    snprintf(j->note, sizeof(j->note), "获取播放地址...");
    bili_playurl_t pu;
    int rc;
    if (j->kind == 0) {
        rc = bili_playurl(j->bvid, j->cid, j->qn ? j->qn : 125,
                          q->cookie, q->img_key, q->sub_key, q->verbose, &pu);
    } else {
        rc = bangumi_playurl(j->cid, j->ep_id, j->qn ? j->qn : 125,
                             q->cookie, q->verbose, &pu);
    }
    if (rc != 0) {
        j->state = JOB_FAILED;
        snprintf(j->err, sizeof(j->err), "获取播放地址失败");
        return -1;
    }

    const bili_stream_t *v = j->audio_only ? NULL : q_pick_video(&pu, j->qn ? j->qn : (1 << 30));
    const bili_stream_t *a = j->video_only ? NULL : q_pick_audio(&pu);
    if (!v && !a) {
        bili_playurl_free(&pu);
        j->state = JOB_FAILED;
        snprintf(j->err, sizeof(j->err), "没有可用的媒体流");
        return -1;
    }

    /* 期望输出 */
    snprintf(j->outpath, sizeof(j->outpath), "%s/%s.%s", j->dir, j->base,
             (j->no_mux || !v || !a) ? (v ? (j->no_mux ? "video.m4s" : "mp4")
                                           : (j->no_mux ? "audio.m4s" : "m4a"))
                                     : (j->mp4 ? "mp4" : "mkv"));

    /* 已完整的流跳过下载 */
    int need_v = v != NULL, need_a = a != NULL;
    if (v) {
        snprintf(j->vpath, sizeof(j->vpath), "%s/%s.f%d.video.m4s", j->dir, j->base, v->id);
        j->vsize = v->size;
        if (v->size > 0 && fsize(j->vpath) == (long long)v->size) {
            need_v = 0;
        }
    }
    if (a) {
        snprintf(j->apath, sizeof(j->apath), "%s/%s.f%d.audio.m4s", j->dir, j->base, a->id);
        j->asize = a->size;
        if (a->size > 0 && fsize(j->apath) == (long long)a->size) {
            need_a = 0;
        }
    }

    free(j->vurl);
    free(j->aurl);
    j->vurl = v ? xstrdup(v->url) : NULL;
    j->aurl = a ? xstrdup(a->url) : NULL;
    bili_playurl_free(&pu);
    j->attempts = 0;

    if (need_v) {
        j->pid = spawn_curl(j->vurl, j->vpath, q->cookie);
        j->has_child = j->pid > 0;
        j->state = JOB_DL_VIDEO;
        snprintf(j->note, sizeof(j->note), "下载视频流");
        if (j->pid < 0) {
            j->state = JOB_FAILED;
            snprintf(j->err, sizeof(j->err), "无法启动 curl");
        }
    } else if (need_a) {
        j->pid = spawn_curl(j->aurl, j->apath, q->cookie);
        j->has_child = j->pid > 0;
        j->state = JOB_DL_AUDIO;
        snprintf(j->note, sizeof(j->note), "下载音频流");
        if (j->pid < 0) {
            j->state = JOB_FAILED;
            snprintf(j->err, sizeof(j->err), "无法启动 curl");
        }
    } else if (j->no_mux || !v || !a) {
        /* 全部已存在，直接改名 */
        const char *src = v ? j->vpath : j->apath;
        rename(src, j->outpath);
        j->state = JOB_DONE;
        snprintf(j->note, sizeof(j->note), "已完成（文件已存在）");
    } else {
        j->pid = spawn_mux(j->vpath, j->apath, j->outpath, j->mp4);
        j->has_child = j->pid > 0;
        j->state = JOB_MUXING;
        snprintf(j->note, sizeof(j->note), "混流");
        if (j->pid < 0) {
            j->state = JOB_FAILED;
            snprintf(j->err, sizeof(j->err), "无法启动 ffmpeg");
        }
    }
    return 0;
}

/* 进入混流阶段 */
static void job_to_mux(bjob_t *j)
{
    if (j->no_mux || !j->vurl || !j->aurl) {
        /* 单流：改名即完成 */
        const char *src = j->vurl ? j->vpath : j->apath;
        if (rename(src, j->outpath) != 0) {
            j->state = JOB_FAILED;
            snprintf(j->err, sizeof(j->err), "无法命名输出文件");
            return;
        }
        j->state = JOB_DONE;
        snprintf(j->note, sizeof(j->note), "已完成");
        return;
    }
    j->pid = spawn_mux(j->vpath, j->apath, j->outpath, j->mp4);
    j->has_child = j->pid > 0;
    j->state = JOB_MUXING;
    snprintf(j->note, sizeof(j->note), "混流中...");
    if (j->pid < 0) {
        j->state = JOB_FAILED;
        snprintf(j->err, sizeof(j->err), "无法启动 ffmpeg");
    }
}

/* 下载阶段子进程结束处理：0=进入下一阶段，-1=失败需重试，1=仍在运行 */
static int job_dl_finished(bqueue_t *q, bjob_t *j, int status)
{
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int is_video = (j->state == JOB_DL_VIDEO);
    const char *path = is_video ? j->vpath : j->apath;
    uint64_t want = is_video ? j->vsize : j->asize;
    long long got = fsize(path);

    int ok = (rc == 0) || (want > 0 && got >= (long long)want);
    if (!ok) {
        if (++j->attempts <= MAX_ATTEMPTS) {
            /* 断点续传重试 */
            j->pid = spawn_curl(is_video ? j->vurl : j->aurl, path, q->cookie);
            j->has_child = j->pid > 0;
            snprintf(j->note, sizeof(j->note), "重试 %d/%d", j->attempts, MAX_ATTEMPTS);
            return 1;
        }
        j->state = JOB_FAILED;
        snprintf(j->err, sizeof(j->err), "下载失败 (curl 退出码 %d)", rc);
        return -1;
    }
    j->attempts = 0;
    if (is_video && j->aurl) {
        j->pid = spawn_curl(j->aurl, j->apath, q->cookie);
        j->has_child = j->pid > 0;
        j->state = JOB_DL_AUDIO;
        snprintf(j->note, sizeof(j->note), "下载音频流");
        return 0;
    }
    job_to_mux(j);
    return 0;
}

static void job_update_progress(bjob_t *j)
{
    if (j->state != JOB_DL_VIDEO && j->state != JOB_DL_AUDIO) {
        return;
    }
    uint64_t want = j->state == JOB_DL_VIDEO ? j->vsize : j->asize;
    const char *path = j->state == JOB_DL_VIDEO ? j->vpath : j->apath;
    long long got = fsize(path);
    if (got < 0) {
        got = 0;
    }
    if (want > 0) {
        snprintf(j->note, sizeof(j->note), "下载中 %s/%s",
                 human_size((uint64_t)got), human_size(want));
    } else {
        snprintf(j->note, sizeof(j->note), "下载中 %s", human_size((uint64_t)got));
    }
}

void bq_tick(bqueue_t *q)
{
    /* 提升一个等待任务（取地址会阻塞约1秒，一次只做一个）。
     * 注意只统计真正在跑的任务（下载/混流），PENDING 不能算，
     * 否则单个任务也会因 running==max 而永远无法开始。 */
    int running = 0;
    for (int i = 0; i < q->n; i++) {
        job_state_t s = q->jobs[i].state;
        if (s == JOB_DL_VIDEO || s == JOB_DL_AUDIO || s == JOB_MUXING) {
            running++;
        }
    }
    if (running < q->max_active) {
        for (int i = 0; i < q->n; i++) {
            if (q->jobs[i].state == JOB_PENDING) {
                job_start(q, &q->jobs[i]);
                break;
            }
        }
    }
    /* 推进活动任务 */
    for (int i = 0; i < q->n; i++) {
        bjob_t *j = &q->jobs[i];
        if (j->state == JOB_DL_VIDEO || j->state == JOB_DL_AUDIO || j->state == JOB_MUXING) {
            int st = 0;
            pid_t r = j->has_child ? waitpid(j->pid, &st, WNOHANG) : 0;
            if (r == 0) {
                job_update_progress(j);
                continue;
            }
            if (r < 0) {
                continue;
            }
            j->has_child = 0;
            if (j->state == JOB_MUXING) {
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                    remove(j->vpath);
                    remove(j->apath);
                    j->state = JOB_DONE;
                    long long sz = fsize(j->outpath);
                    snprintf(j->note, sizeof(j->note), "完成 %s",
                             human_size((uint64_t)(sz > 0 ? sz : 0)));
                } else {
                    j->state = JOB_FAILED;
                    snprintf(j->err, sizeof(j->err), "混流失败");
                }
            } else {
                job_dl_finished(q, j, st);
            }
        }
    }
}
