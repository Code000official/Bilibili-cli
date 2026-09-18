/* main.c - bili-cli 入口：B 站视频解析与下载命令行工具
 *
 * 输入 URL/BV/av -> WBI 签名 -> view/playurl -> DASH 下载 -> ffmpeg 混流。
 */
#include "bili.h"
#include "bangumi.h"
#include "bvid.h"
#include "http.h"
#include "login.h"
#include "md5.h"
#include "tui.h"
#include "util.h"
#include "wbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BILI_VERSION "0.0.1"

typedef struct {
    const char *input;
    int page;        /* 0=第1P, -1=全部分P, >0=指定P */
    const char *ep_spec; /* 番剧集数选择: "all" / "1,3,5-8"，NULL=默认 */
    int qn;          /* 0=最高可用 */
    const char *outdir;
    const char *sessdata;
    int info;
    int no_mux;
    int video_only;
    int audio_only;
    int mp4;
    int verbose;
    int selftest;
} opts_t;

static void usage(FILE *out)
{
    fprintf(out,
        "bili - B 站视频解析下载工具\n\n"
        "用法:\n"
        "  bili                     进入全屏界面（推荐）\n"
        "  bili shell               行交互模式（REPL）\n"
        "  bili [选项] <目标>       单次下载/查看\n"
        "  bili login|logout|whoami 扫码登录 / 退出登录 / 账号状态\n\n"
        "目标: URL | BV号 | av号 | ep号 | ss号（支持 b23.tv 短链）\n\n"
        "选项:\n"
        "  -p, --page <N|all>   指定分P，all 为全部分P（默认第1P）\n"
        "  -e, --episode <选择>  番剧集数，如 3 / 1,3,5-8 / all（默认第1集）\n"
        "  -q, --quality <清晰度>  如 1080p/720p/480p/4k/hdr 或 qn 数字（默认最高可用）\n"
        "  -o, --outdir <目录>  输出目录（默认当前目录）\n"
        "  -s, --sessdata <值>  手动指定 SESSDATA（通常用 login 扫码即可）\n"
        "      --info           仅显示视频/番剧信息，不下载\n"
        "      --no-mux         保留分离的音视频流，不混流\n"
        "      --video-only     仅下载视频流\n"
        "      --audio-only     仅下载音频流\n"
        "      --mp4            混流为 mp4（默认 mkv）\n"
        "  -v, --verbose        输出请求 URL 等调试信息\n"
        "      --selftest       运行内置自检（MD5/av-bv 转换）\n"
        "  -V, --version        显示版本号\n"
        "  -h, --help           显示帮助\n\n"
        "示例:\n"
        "  bili login\n"
        "  bili \"https://www.bilibili.com/video/BV1GJ411x7h7\"\n"
        "  bili BV1GJ411x7h7 -q 1080p -o ~/Videos\n"
        "  bili av80433022 -p 2\n"
        "  bili ss33379 -e 1-12\n");
}

/* 清晰度参数解析：支持名称与数字 */
static int parse_quality(const char *s, int *qn)
{
    static const struct { const char *name; int qn; } tbl[] = {
        { "8k", 127 }, { "dolby", 126 }, { "hdr", 125 }, { "4k", 120 },
        { "1080p60", 116 }, { "1080p", 80 }, { "720p60", 74 },
        { "720p", 64 }, { "480p", 32 }, { "360p", 16 },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcasecmp(s, tbl[i].name) == 0) {
            *qn = tbl[i].qn;
            return 0;
        }
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end && *end == '\0' && v > 0) {
        *qn = (int)v;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv, opts_t *o)
{
    int have_input = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT_VAL() (i + 1 < argc ? argv[++i] : NULL)
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("bili %s\n", BILI_VERSION);
            return 200; /* 已打印版本，正常结束 */
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 200; /* 已打印帮助，正常结束 */
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--page") == 0) {
            const char *v = NEXT_VAL();
            if (!v) {
                fprintf(stderr, "错误: %s 需要参数\n", a);
                return -1;
            }
            if (strcasecmp(v, "all") == 0) {
                o->page = -1;
            } else {
                o->page = atoi(v);
                if (o->page <= 0) {
                    fprintf(stderr, "错误: 无效分P号 '%s'\n", v);
                    return -1;
                }
            }
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--episode") == 0) {
            o->ep_spec = NEXT_VAL();
            if (!o->ep_spec) {
                fprintf(stderr, "错误: --episode 需要参数\n");
                return -1;
            }
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quality") == 0) {
            const char *v = NEXT_VAL();
            if (!v || parse_quality(v, &o->qn) != 0) {
                fprintf(stderr, "错误: 无效清晰度 '%s'\n", v ? v : "");
                return -1;
            }
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--outdir") == 0) {
            o->outdir = NEXT_VAL();
            if (!o->outdir) {
                fprintf(stderr, "错误: --outdir 需要参数\n");
                return -1;
            }
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--sessdata") == 0) {
            o->sessdata = NEXT_VAL();
            if (!o->sessdata) {
                fprintf(stderr, "错误: --sessdata 需要参数\n");
                return -1;
            }
        } else if (strcmp(a, "--info") == 0) {
            o->info = 1;
        } else if (strcmp(a, "--no-mux") == 0) {
            o->no_mux = 1;
        } else if (strcmp(a, "--video-only") == 0) {
            o->video_only = 1;
        } else if (strcmp(a, "--audio-only") == 0) {
            o->audio_only = 1;
        } else if (strcmp(a, "--mp4") == 0) {
            o->mp4 = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            o->verbose = 1;
        } else if (strcmp(a, "--selftest") == 0) {
            o->selftest = 1;
        } else if (strcmp(a, "--sign-test") == 0) {
            /* 隐藏选项: --sign-test <img_key> <sub_key> <wts> k=v... 输出签名结果 */
            if (i + 4 > argc - 1) {
                fprintf(stderr, "sign-test 参数不足\n");
                return -1;
            }
            const char *img = argv[++i];
            const char *sub = argv[++i];
            const char *wts = argv[++i];
            kv_t *ps = NULL;
            size_t np = 0;
            while (i + 1 < argc) {
                char *kv = argv[++i];
                char *eq = strchr(kv, '=');
                if (!eq) {
                    return -1;
                }
                size_t kl = (size_t)(eq - kv);
                char k[64];
                snprintf(k, sizeof(k), "%.*s", (int)kl, kv);
                kv_add(&ps, &np, k, eq + 1);
            }
            char *q = NULL;
            if (wbi_sign_query_wts(img, sub, ps, np, wts, &q) != 0) {
                return -1;
            }
            printf("%s\n", q);
            return 100; /* 特殊退出：直接结束 */
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "错误: 未知选项 %s\n", a);
            return -1;
        } else {
            if (have_input) {
                fprintf(stderr, "错误: 只支持一个输入\n");
                return -1;
            }
            o->input = a;
            have_input = 1;
        }
#undef NEXT_VAL
    }
    if (o->video_only && o->audio_only) {
        fprintf(stderr, "错误: --video-only 与 --audio-only 不能同时使用\n");
        return -1;
    }
    return (have_input || o->selftest) ? 0 : -1;
}

static int dispatch(opts_t *o);

/* ---------- 自检 ---------- */

static int selftest(void)
{
    int fail = 0;
    struct { const char *in, *want; } md5_cases[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "The quick brown fox jumps over the lazy dog",
          "9e107d9d372bb6826bd81d3542a419d6" },
    };
    for (size_t i = 0; i < sizeof(md5_cases) / sizeof(md5_cases[0]); i++) {
        char got[33];
        md5_hex((const uint8_t *)md5_cases[i].in, strlen(md5_cases[i].in), got);
        if (strcmp(got, md5_cases[i].want) != 0) {
            fprintf(stderr, "MD5 失败: \"%s\" => %s (期望 %s)\n",
                    md5_cases[i].in, got, md5_cases[i].want);
            fail++;
        }
    }
    char *bv = bvid_av2bv(80433022);
    if (!bv || strcmp(bv, "BV1GJ411x7h7") != 0) {
        fprintf(stderr, "av2bv 失败: 80433022 => %s (期望 BV1GJ411x7h7)\n", bv ? bv : "null");
        fail++;
    }
    free(bv);
    uint64_t aid = bvid_bv2av("BV1GJ411x7h7");
    if (aid != 80433022) {
        fprintf(stderr, "bv2av 失败: BV1GJ411x7h7 => %llu (期望 80433022)\n",
                (unsigned long long)aid);
        fail++;
    }
    if (fail == 0) {
        printf("自检通过: MD5 与 av/BV 转换正常\n");
        return 0;
    }
    fprintf(stderr, "自检失败: %d 项\n", fail);
    return 1;
}

/* ---------- Cookie ---------- */

/* 组装 Cookie 头（buvid + 已保存登录凭证 + 可选 --sessdata 覆盖），调用方 free */
static char *build_cookie(const char *sessdata_override)
{
    char *b3 = NULL, *b4 = NULL;
    bili_get_buvid(&b3, &b4, 0);
    char *ck = login_build_cookie(b3, b4, sessdata_override);
    free(b3);
    free(b4);
    return ck;
}

/* ---------- 流选择 ---------- */

static const bili_stream_t *pick_video(const bili_playurl_t *pu, int want)
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
            best = s; /* 已选的太高，当前更合适 */
        } else if (!b_ok) {
            best = s->id < best->id ? s : best; /* 都超了，取最低 */
        }
    }
    return best;
}

static const bili_stream_t *pick_audio(const bili_playurl_t *pu)
{
    static const int prefer[] = { 30280, 30232, 30216 }; /* 192k > 132k > 64k */
    for (size_t k = 0; k < sizeof(prefer) / sizeof(prefer[0]); k++) {
        for (int i = 0; i < pu->naudios; i++) {
            if (pu->audios[i].id == prefer[k]) {
                return &pu->audios[i];
            }
        }
    }
    return pu->naudios ? &pu->audios[0] : NULL;
}

/* ---------- 下载与混流 ---------- */

static long long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static int run_child(char **argv)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "错误: 找不到 %s 命令\n", argv[0]);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int mux_streams(const char *v, const char *a, const char *out, int mp4)
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
    return run_child(argv);
}

static int dl_stream(const bili_stream_t *s, const char *path,
                     const char *cookie, const char *label)
{
    long long have = file_size(path);
    if (s->size > 0 && have == (long long)s->size) {
        printf("%s 已存在且完整，跳过\n", label);
        return 0;
    }
    /* 部分响应不含 size 字段，此时只显示标签 */
    if (s->size > 0) {
        printf("%s: %s\n", label, human_size(s->size));
    } else {
        printf("%s\n", label);
    }
    int rc = http_download(s->url, path, cookie);
    if (rc != 0) {
        /* 已完整下载时服务器对 Range 请求返回 4xx，视为成功 */
        long long now = file_size(path);
        if (now > 0 && (s->size == 0 || now >= (long long)s->size)) {
            return 0;
        }
        fprintf(stderr, "%s 下载失败 (curl 退出码 %d)\n", label, rc);
        return -1;
    }
    return 0;
}

/* durl 兜底：老视频非 DASH 流 */
static int download_durl(const bili_playurl_t *pu, const char *dir,
                         const char *base, const char *cookie, const opts_t *o)
{
    printf("该视频无 DASH 流，使用 durl 模式（%d 段）\n", pu->ndurls);
    char *list_path = xmalloc(strlen(dir) + strlen(base) + 16);
    sprintf(list_path, "%s/%s.concat.txt", dir, base);
    FILE *lp = fopen(list_path, "w");
    if (!lp) {
        free(list_path);
        return -1;
    }
    int ok = 0;
    for (int i = 0; i < pu->ndurls; i++) {
        char *seg = xmalloc(strlen(dir) + strlen(base) + 32);
        sprintf(seg, "%s/%s.seg%d.mp4", dir, base, i);
        if (dl_stream(&(bili_stream_t){ .url = pu->durls[i], .size = pu->dsizes[i] },
                      seg, cookie, "下载分段") != 0) {
            free(seg);
            goto out;
        }
        fprintf(lp, "file '%s'\n", seg);
        if (o->verbose) {
            fprintf(stderr, "[verbose] 分段: %s\n", seg);
        }
        free(seg);
    }
    fclose(lp);
    lp = NULL;

    char *out = xmalloc(strlen(dir) + strlen(base) + 8);
    sprintf(out, "%s/%s.mp4", dir, base);
    char *argv[16];
    int n = 0;
    argv[n++] = (char *)"ffmpeg";
    argv[n++] = (char *)"-y";
    argv[n++] = (char *)"-nostdin";
    argv[n++] = (char *)"-hide_banner";
    argv[n++] = (char *)"-loglevel";
    argv[n++] = (char *)"error";
    argv[n++] = (char *)"-f";
    argv[n++] = (char *)"concat";
    argv[n++] = (char *)"-safe";
    argv[n++] = (char *)"0";
    argv[n++] = (char *)"-i";
    argv[n++] = list_path;
    argv[n++] = (char *)"-c";
    argv[n++] = (char *)"copy";
    argv[n++] = (char *)out;
    argv[n] = NULL;
    printf("合并分段...\n");
    if (run_child(argv) == 0) {
        long long sz = file_size(out);
        printf("已完成: %s (%s)\n", out, human_size((uint64_t)(sz > 0 ? sz : 0)));
        /* 清理分段 */
        for (int i = 0; i < pu->ndurls; i++) {
            char *seg = xmalloc(strlen(dir) + strlen(base) + 32);
            sprintf(seg, "%s/%s.seg%d.mp4", dir, base, i);
            remove(seg);
            free(seg);
        }
        ok = 0;
    } else {
        fprintf(stderr, "合并失败，分段文件保留在 %s\n", dir);
        ok = -1;
    }
    free(out);
out:
    if (lp) {
        fclose(lp);
    }
    remove(list_path);
    free(list_path);
    return ok;
}

/* 对已获取的 playurl 执行：流选择、下载、混流。返回 0 成功 */
static int download_playurl(const bili_playurl_t *pu, const opts_t *o,
                            const char *cookie, const char *dir, const char *base)
{
    if (o->verbose) {
        strbuf_t acc;
        sb_init(&acc);
        for (int i = 0; i < pu->naccept; i++) {
            sb_appendf(&acc, "%s%d(%s)", i ? " " : "", pu->accept[i], bili_qn_name(pu->accept[i]));
        }
        fprintf(stderr, "[verbose] 可用清晰度: %s\n", acc.data);
        sb_free(&acc);
    }

    if (pu->ndurls > 0 && pu->nvideos == 0) {
        return download_durl(pu, dir, base, cookie, o);
    }

    const bili_stream_t *v = pick_video(pu, o->qn ? o->qn : (1 << 30));
    const bili_stream_t *a = o->video_only ? NULL : pick_audio(pu);
    if (o->audio_only) {
        v = NULL;
    }
    if (!v && !a) {
        fprintf(stderr, "错误: 没有可选的媒体流\n");
        return -1;
    }

    if (v) {
        printf("视频流: %s (%dx%d, %s, %lu kb/s)\n", bili_qn_name(v->id),
               v->width, v->height, v->codecs ? v->codecs : "?",
               (unsigned long)(v->bandwidth / 1000));
    }
    if (a) {
        printf("音频流: id=%d (%s)\n", a->id, a->codecs ? a->codecs : "?");
    }

    char *vpath = NULL, *apath = NULL;
    if (v) {
        vpath = xmalloc(strlen(dir) + strlen(base) + 32);
        sprintf(vpath, "%s/%s.f%d.video.m4s", dir, base, v->id);
        char label[128];
        snprintf(label, sizeof(label), "下载视频流 (f%d)", v->id);
        if (dl_stream(v, vpath, cookie, label) != 0) {
            goto fail;
        }
    }
    if (a) {
        apath = xmalloc(strlen(dir) + strlen(base) + 32);
        sprintf(apath, "%s/%s.f%d.audio.m4s", dir, base, a->id);
        if (dl_stream(a, apath, cookie, "下载音频流") != 0) {
            goto fail;
        }
    }

    if (o->no_mux || !v || !a) {
        /* 不混流或只有单流：保留/改名最终文件 */
        const char *src = v ? vpath : apath;
        char *final = xmalloc(strlen(dir) + strlen(base) + 8);
        sprintf(final, "%s/%s.%s", dir, base, v ? (o->no_mux ? "video.m4s" : "mp4")
                                                : (o->no_mux ? "audio.m4s" : "m4a"));
        if (rename(src, final) != 0) {
            fprintf(stderr, "错误: 无法命名输出文件 %s\n", final);
            free(final);
            goto fail;
        }
        long long sz = file_size(final);
        printf("已完成: %s (%s)\n", final, human_size((uint64_t)(sz > 0 ? sz : 0)));
        free(final);
        free(vpath);
        free(apath);
        return 0;
    }

    char *out = xmalloc(strlen(dir) + strlen(base) + 8);
    sprintf(out, "%s/%s.%s", dir, base, o->mp4 ? "mp4" : "mkv");
    printf("混流: ffmpeg -> %s\n", out);
    if (mux_streams(vpath, apath, out, o->mp4) != 0) {
        fprintf(stderr, "混流失败，临时流文件保留:\n  %s\n  %s\n", vpath, apath);
        free(out);
        goto fail;
    }
    remove(vpath);
    remove(apath);
    long long sz = file_size(out);
    printf("已完成: %s (%s)\n", out, human_size((uint64_t)(sz > 0 ? sz : 0)));
    free(out);
    free(vpath);
    free(apath);
    return 0;

fail:
    free(vpath);
    free(apath);
    return -1;
}

/* 下载单个分P，返回 0 成功 */
static int download_page(const bili_view_t *view, const bili_page_t *pg,
                         const opts_t *o, const char *cookie,
                         const char *img, const char *sub,
                         const char *dir, const char *base)
{
    printf("\n-- 第 %d 分P: %s (cid=%ld)\n",
           pg->page, pg->part ? pg->part : view->title, pg->cid);

    bili_playurl_t pu;
    if (bili_playurl(view->bvid, pg->cid, o->qn ? o->qn : 125,
                     cookie, img, sub, o->verbose, &pu) != 0) {
        return -1;
    }
    int rc = download_playurl(&pu, o, cookie, dir, base);
    bili_playurl_free(&pu);
    return rc;
}

/* ---------- info 输出 ---------- */

static int show_info(const bili_view_t *view, const opts_t *o,
                     const char *cookie, const char *img, const char *sub)
{
    printf("标题: %s\n", view->title);
    printf("UP主: %s\n", view->owner ? view->owner : "-");
    printf("时长: %s   aid: %lld   bvid: %s\n",
           human_duration(view->duration), view->aid, view->bvid);
    printf("分P (%d):\n", view->npages);
    for (int i = 0; i < view->npages; i++) {
        printf("  P%d  %s (cid=%ld)\n", view->pages[i].page,
               view->pages[i].part ? view->pages[i].part : "-", view->pages[i].cid);
    }

    /* 用第一个相关分P的 playurl 展示可用清晰度 */
    const bili_page_t *pg = &view->pages[0];
    if (o->page > 0) {
        for (int i = 0; i < view->npages; i++) {
            if (view->pages[i].page == o->page) {
                pg = &view->pages[i];
                break;
            }
        }
    }
    bili_playurl_t pu;
    if (bili_playurl(view->bvid, pg->cid, o->qn ? o->qn : 125,
                     cookie, img, sub, o->verbose, &pu) != 0) {
        return -1;
    }
    printf("清晰度 (P%d 可用):", pg->page);
    for (int i = 0; i < pu.naccept; i++) {
        printf(" %d(%s)", pu.accept[i], bili_qn_name(pu.accept[i]));
    }
    printf("\n");
    if (pu.nvideos > 0) {
        const bili_stream_t *v = pick_video(&pu, o->qn ? o->qn : (1 << 30));
        if (pu.quality < 80) {
            printf("提示: 未登录最高约 480P，运行 bili login 扫码登录可解锁\n");
        }
        printf("将选择: %s (%dx%d, %s)\n", bili_qn_name(v->id),
               v->width, v->height, v->codecs ? v->codecs : "?");
    }
    bili_playurl_free(&pu);
    return 0;
}

/* ---------- 番剧 ---------- */

/*
 * 解析集数选择 "all" / "3" / "1,3,5-8"，返回 0 基索引数组（调用方 free）。
 * 无效输入返回 NULL。
 */
static int *parse_episode_spec(const char *spec, int neps, int *out_n)
{
    if (!spec || neps <= 0) {
        return NULL;
    }
    int *sel = xmalloc(sizeof(int) * (size_t)neps);
    int n = 0;
    if (strcasecmp(spec, "all") == 0) {
        for (int i = 0; i < neps; i++) {
            sel[n++] = i;
        }
    } else {
        const char *p = spec;
        while (*p) {
            char *end = NULL;
            long a = strtol(p, &end, 10);
            long b = a;
            if (end == p || a < 1 || a > neps) {
                goto bad;
            }
            p = end;
            if (*p == '-') {
                p++;
                b = strtol(p, &end, 10);
                if (end == p || b < a) {
                    goto bad;
                }
                p = end;
            }
            for (long v = a; v <= b && v <= neps; v++) {
                sel[n++] = (int)v - 1;
            }
            if (*p == ',') {
                p++;
            } else if (*p) {
                goto bad;
            }
        }
    }
    if (n == 0) {
        goto bad;
    }
    *out_n = n;
    return sel;
bad:
    free(sel);
    return NULL;
}

/* 番剧流程入口（--info 或下载），is_ss 标识输入为 ss 号还是 ep 号 */
static int run_bangumi(const opts_t *o, long id, int is_ss)
{
    char *cookie = build_cookie(o->sessdata);

    bangumi_season_t season;
    if (bangumi_season(is_ss ? id : -1, is_ss ? -1 : id,
                       cookie, o->verbose, &season) != 0) {
        free(cookie);
        return 1;
    }

    printf("番剧: %s\n", season.title);
    printf("season_id: ss%ld   集数: %d\n", season.season_id, season.neps);

    /* 输入为 ep 号时定位对应集，作为默认下载/展示对象 */
    int default_idx = 0;
    if (!is_ss) {
        for (int i = 0; i < season.neps; i++) {
            if (season.episodes[i].id == id) {
                default_idx = i;
                break;
            }
        }
    }

    if (o->info) {
        for (int i = 0; i < season.neps; i++) {
            const bangumi_episode_t *ep = &season.episodes[i];
            printf("  EP%-2d %-24s (ep%ld, %s)%s\n", i + 1,
                   ep->long_title ? ep->long_title : (ep->title ? ep->title : "-"),
                   ep->id, human_duration(ep->duration),
                   i == default_idx ? "  <- 输入的集" : "");
        }
    }

    if (o->info) {
        /* --info：补打默认集的可用清晰度后结束 */
        const bangumi_episode_t *ep = &season.episodes[default_idx];
        bili_playurl_t pu;
        if (bangumi_playurl(ep->cid, ep->id, o->qn ? o->qn : 125,
                            cookie, o->verbose, &pu) == 0) {
            printf("清晰度 (EP%d 可用):", default_idx + 1);
            for (int i = 0; i < pu.naccept; i++) {
                printf(" %d(%s)", pu.accept[i], bili_qn_name(pu.accept[i]));
            }
            printf("\n");
            if (pu.quality < 80) {
                printf("提示: 未登录最高约 480P，运行 bili login 扫码登录可解锁\n");
            }
            bili_playurl_free(&pu);
        }
        bangumi_season_free(&season);
        free(cookie);
        return 0;
    }

    /* 选集 */
    int *sel = NULL, nsel = 0;
    if (o->ep_spec) {
        sel = parse_episode_spec(o->ep_spec, season.neps, &nsel);
        if (!sel) {
            fprintf(stderr, "错误: 无效集数选择 '%s'（该季共 %d 集）\n", o->ep_spec, season.neps);
            bangumi_season_free(&season);
            free(cookie);
            return 1;
        }
    } else {
        sel = xmalloc(sizeof(int));
        sel[0] = default_idx;
        nsel = 1;
        if (season.neps > 1) {
            printf("提示: 共 %d 集，默认下载第 %d 集（-e all 下载全部，-e 1,3-5 选择）\n",
                   season.neps, default_idx + 1);
        }
    }

    /* 输出目录: outdir/番剧名/ */
    char *safe_title = sanitize_filename(season.title);
    char *dir = xmalloc(strlen(o->outdir) + strlen(safe_title) + 2);
    sprintf(dir, "%s/%s", o->outdir, safe_title);
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "错误: 无法创建输出目录 %s\n", dir);
        free(dir);
        free(safe_title);
        free(sel);
        bangumi_season_free(&season);
        free(cookie);
        return 1;
    }

    int rc = 0;
    for (int k = 0; k < nsel; k++) {
        const bangumi_episode_t *ep = &season.episodes[sel[k]];
        printf("\n-- EP%d: %s (ep%ld, cid=%ld)\n", sel[k] + 1,
               ep->long_title ? ep->long_title : (ep->title ? ep->title : "-"),
               ep->id, ep->cid);

        char *name = sanitize_filename(ep->long_title && *ep->long_title
                                           ? ep->long_title
                                           : (ep->title ? ep->title : ""));
        char *base = xmalloc(strlen(name) + 16);
        sprintf(base, "EP%02d_%s", sel[k] + 1, name);
        free(name);

        bili_playurl_t pu;
        if (bangumi_playurl(ep->cid, ep->id, o->qn ? o->qn : 125,
                            cookie, o->verbose, &pu) != 0) {
            rc = 1;
        } else {
            if (pu.is_preview) {
                fprintf(stderr, "警告: 该集为预览（试看）内容，可能不完整\n");
            }
            if (download_playurl(&pu, o, cookie, dir, base) != 0) {
                rc = 1;
            }
            bili_playurl_free(&pu);
        }
        free(base);
    }

    free(dir);
    free(safe_title);
    free(sel);
    bangumi_season_free(&season);
    free(cookie);
    return rc;
}

/* ---------- 主流程 ---------- */

/* ---------- 交互模式（REPL） ---------- */

/* 带引号支持的简单分词，返回词数 */
static int tokenize(char *s, char **tok, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\n') {
            s++;
        }
        if (!*s) {
            break;
        }
        if (*s == '"') {
            tok[n++] = ++s;
            while (*s && *s != '"') {
                s++;
            }
        } else {
            tok[n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                s++;
            }
        }
        if (*s) {
            *s++ = '\0';
        }
    }
    return n;
}

static void repl_help(void)
{
    printf(
        "命令:\n"
        "  info <目标> [选项]   查看视频/番剧信息\n"
        "  dl <目标> [选项]     下载（目标也可直接输入，等价 dl）\n"
        "  set                  查看会话默认值\n"
        "  set outdir <目录>    设置默认输出目录\n"
        "  set qn <清晰度|auto> 设置默认清晰度\n"
        "  login | logout | whoami   扫码登录 / 退出登录 / 账号状态\n"
        "  quit                 退出\n"
        "选项与一次性模式相同: -p 分P / -e 集数 / -q 清晰度 / --mp4 / --audio-only 等\n");
}

static int repl(void)
{
    char line[4096];
    char *session_outdir = xstrdup(".");
    int session_qn = 0;

    printf("bili 交互模式 - 输入 help 查看命令，quit 退出\n");
    for (;;) {
        printf("\nbili> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        char *tok[64];
        int n = tokenize(line, tok, 64);
        if (n == 0) {
            continue;
        }
        const char *cmd = tok[0];

        if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 ||
            strcmp(cmd, "exit") == 0) {
            break;
        }
        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            repl_help();
            continue;
        }
        if (strcmp(cmd, "login") == 0) {
            login_qr_flow(0);
            continue;
        }
        if (strcmp(cmd, "logout") == 0) {
            login_logout();
            continue;
        }
        if (strcmp(cmd, "whoami") == 0) {
            char *ck = build_cookie(NULL);
            login_whoami(ck);
            free(ck);
            continue;
        }
        if (strcmp(cmd, "set") == 0) {
            if (n == 1) {
                printf("outdir = %s\n", session_outdir);
                if (session_qn) {
                    printf("qn = %d (%s)\n", session_qn, bili_qn_name(session_qn));
                } else {
                    printf("qn = auto（最高可用）\n");
                }
                continue;
            }
            if (n >= 3 && strcmp(tok[1], "outdir") == 0) {
                free(session_outdir);
                session_outdir = xstrdup(tok[2]);
                printf("outdir = %s\n", session_outdir);
                continue;
            }
            if (n >= 3 && strcmp(tok[1], "qn") == 0) {
                if (strcmp(tok[2], "auto") == 0) {
                    session_qn = 0;
                } else if (parse_quality(tok[2], &session_qn) != 0) {
                    fprintf(stderr, "无效清晰度 '%s'\n", tok[2]);
                    continue;
                }
                if (session_qn) {
                    printf("qn = %d (%s)\n", session_qn, bili_qn_name(session_qn));
                } else {
                    printf("qn = auto（最高可用）\n");
                }
                continue;
            }
            fprintf(stderr, "用法: set [outdir <目录> | qn <清晰度|auto>]\n");
            continue;
        }

        /* info / dl / download 或直接输入目标 */
        char **args = tok;
        int nargs = n;
        int is_info = 0;
        if (strcmp(cmd, "info") == 0) {
            args = tok + 1;
            nargs = n - 1;
            is_info = 1;
        } else if (strcmp(cmd, "dl") == 0 || strcmp(cmd, "download") == 0) {
            args = tok + 1;
            nargs = n - 1;
        }
        if (nargs == 0) {
            fprintf(stderr, "缺少目标参数，输入 help 查看用法\n");
            continue;
        }

        opts_t o = { .input = NULL, .page = 0, .ep_spec = NULL,
                     .qn = session_qn, .outdir = session_outdir,
                     .sessdata = NULL, .info = is_info, .no_mux = 0,
                     .video_only = 0, .audio_only = 0, .mp4 = 0,
                     .verbose = 0, .selftest = 0 };
        /* parse_args 沿用 main 约定：argv[0] 为程序名，从下标 1 开始 */
        char *pargv[65];
        pargv[0] = (char *)"bili";
        for (int i = 0; i < nargs; i++) {
            pargv[i + 1] = args[i];
        }
        int prc = parse_args(nargs + 1, pargv, &o);
        if (prc == 200 || prc == 100) {
            continue;
        }
        if (prc != 0) {
            fprintf(stderr, "参数有误，输入 help 查看用法\n");
            continue;
        }
        (void)dispatch(&o);
    }
    free(session_outdir);
    return 0;
}

int main(int argc, char **argv)
{
    /* 内置子命令 */
    if (argc >= 2) {
        if (strcmp(argv[1], "login") == 0) {
            return login_qr_flow(0);
        }
        if (strcmp(argv[1], "logout") == 0) {
            return login_logout();
        }
        if (strcmp(argv[1], "whoami") == 0) {
            char *ck = build_cookie(NULL);
            int rc = login_whoami(ck);
            free(ck);
            return rc;
        }
    }
    /* 无参数进入全屏 TUI；bili shell 进入行交互模式 */
    if (argc >= 2 && strcmp(argv[1], "shell") == 0) {
        return repl();
    }
    if (argc == 1) {
        return tui_main();
    }

    opts_t o = { .input = NULL, .page = 0, .ep_spec = NULL, .qn = 0,
                 .outdir = ".", .sessdata = NULL, .info = 0, .no_mux = 0,
                 .video_only = 0, .audio_only = 0, .mp4 = 0, .verbose = 0,
                 .selftest = 0 };

    int prc = parse_args(argc, argv, &o);
    if (prc == 100 || prc == 200) {
        return 0; /* --sign-test 已输出 / --help 已打印 */
    }
    if (prc != 0) {
        fprintf(stderr, "\n");
        usage(stderr);
        return 1;
    }
    return dispatch(&o);
}

/* 对一组选项执行完整流程（输入解析 -> 信息/下载）。返回退出码 */
static int dispatch(opts_t *o)
{
    if (o->selftest) {
        return selftest();
    }
    if (!o->input) {
        fprintf(stderr, "错误: 缺少目标\n");
        return 1;
    }

    /* b23.tv 短链先解析 */
    char *input = xstrdup(o->input);
    if (strstr(input, "b23.tv")) {
        char *final_url = NULL;
        if (http_resolve(input, &final_url) != 0) {
            fprintf(stderr, "错误: 无法解析短链接 %s\n", input);
            free(input);
            return 1;
        }
        if (o->verbose) {
            fprintf(stderr, "[verbose] 短链解析: %s -> %s\n", input, final_url);
        }
        free(input);
        input = final_url;
    }

    char *bvid = NULL;
    int url_page = 0;
    long num_id = 0;
    bvid_result_t pr = bvid_parse_input(input, &bvid, &url_page, &num_id);
    free(input);
    if (pr == BVID_EP || pr == BVID_SS) {
        return run_bangumi(o, num_id, pr == BVID_SS);
    }
    if (pr == BVID_UNSUPPORTED) {
        fprintf(stderr, "错误: 暂不支持课程(cheese)链接\n");
        return 1;
    }
    if (pr != BVID_OK) {
        fprintf(stderr, "错误: 无法识别的输入，请提供 B 站视频 URL、BV/av 号或番剧 ep/ss 号\n");
        return 1;
    }
    int page = o->page ? o->page : (url_page ? url_page : 0);

    /* buvid + WBI 密钥 */
    char *cookie = build_cookie(o->sessdata);

    char *img = NULL, *sub = NULL;
    if (bili_get_wbi_keys(cookie, &img, &sub, o->verbose) != 0) {
        fprintf(stderr, "错误: 获取 WBI 密钥失败，无法继续\n");
        return 1;
    }

    bili_view_t view;
    if (bili_view(bvid, cookie, img, sub, o->verbose, &view) != 0) {
        fprintf(stderr, "错误: 获取视频信息失败\n");
        free(bvid);
        return 1;
    }
    free(bvid);

    if (o->info) {
        int rc = show_info(&view, o, cookie, img, sub);
        bili_view_free(&view);
        free(cookie);
        free(img);
        free(sub);
        return rc == 0 ? 0 : 1;
    }

    /* 选定要下载的分P */
    int first = 1, last = view.npages;
    if (page > 0) {
        first = last = page;
        if (page > view.npages) {
            fprintf(stderr, "错误: 视频只有 %d 个分P，没有第 %d P\n", view.npages, page);
            bili_view_free(&view);
            return 1;
        }
    } else if (page == 0 && view.npages > 1) {
        printf("提示: 共 %d 个分P，默认下载第 1P（-p all 下载全部分P）\n", view.npages);
    }

    /* 输出目录: outdir/标题/ */
    char *safe_title = sanitize_filename(view.title);
    char *dir = xmalloc(strlen(o->outdir) + strlen(safe_title) + 2);
    sprintf(dir, "%s/%s", o->outdir, safe_title);
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "错误: 无法创建输出目录 %s\n", dir);
        free(dir);
        free(safe_title);
        bili_view_free(&view);
        return 1;
    }

    int rc = 0;
    for (int p = first; p <= last; p++) {
        const bili_page_t *pg = NULL;
        for (int i = 0; i < view.npages; i++) {
            if (view.pages[i].page == p) {
                pg = &view.pages[i];
                break;
            }
        }
        if (!pg) {
            continue;
        }
        /* 多P时文件名带 P 号 */
        char *base;
        if (view.npages > 1) {
            base = xmalloc(strlen(safe_title) + 16);
            sprintf(base, "%s_P%d", safe_title, pg->page);
        } else {
            base = xstrdup(safe_title);
        }
        if (download_page(&view, pg, o, cookie, img, sub, dir, base) != 0) {
            rc = 1;
        }
        free(base);
    }

    free(dir);
    free(safe_title);
    bili_view_free(&view);
    free(cookie);
    free(img);
    free(sub);
    return rc;
}
