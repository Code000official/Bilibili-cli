/* tui.c - htop 式全屏界面
 *
 * 结构：顶栏（登录状态/快捷键）+ 主区域（首页输入框 或 分P/剧集多选列表）
 *       + 底部下载队列面板 + 按键提示行。
 * 事件循环单线程：term_poll_key(200ms) 超时即 bq_tick() 推进队列并差量重绘。
 */
#include "tui.h"

#include "bangumi.h"
#include "bili.h"
#include "bvid.h"
#include "http.h"
#include "login.h"
#include "queue.h"
#include "term.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void show_loading(const char *text);

/* ---------- 调色板（256 色） ---------- */

#define C_DEFAULT   (-1)
#define C_BAR_BG    236
#define C_FG_DIM    244
#define C_FG_TEXT   255
#define C_SEL_BG    24
#define C_ACCENT    214
#define C_GREEN     114
#define C_RED       203
#define C_CYAN      81
#define C_BORDER    240

#define MAX_ITEMS 512
#define QUEUE_ROWS 8

typedef struct {
    char label[512];   /* 列表显示（含序号） */
    char part[256];    /* 文件名用的分P/集名 */
    long cid;
    long ep_id;        /* kind==1 */
    int page_no;       /* kind==0 的分P号 / kind==1 的集序号 */
    char dur[16];
    int selected;
} ui_item_t;

static struct {
    int screen;            /* 0=首页 1=详情 */
    int quit;
    char input[4096];      /* 首页输入（UTF-8 字节，NUL 结尾） */
    int editing_outdir;    /* 首页输入框当前编辑的是输出目录 */
    char outdir[1024];
    int focus;             /* 详情页焦点：0=列表 1=队列 */
    int list_cursor, list_top;
    int queue_cursor;
    ui_item_t items[MAX_ITEMS];
    int nitems;
    char dtitle[512];
    char dsub[256];
    int kind;              /* 当前详情类型 0=视频 1=番剧 */
    char bvid[16];
    long season_id;
    int qaccept[24];
    int nq;
    int qsel;
    bqueue_t q;
    char *cookie, *img, *sub;
    int logged;
    char uname[128];
    char toast[256];
    time_t toast_until;
} U;

/* ---------- 小工具 ---------- */

static void set_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.toast, sizeof(U.toast), fmt, ap);
    va_end(ap);
    U.toast_until = time(NULL) + 4;
}

/* 截断显示到最大显示宽度（宽字符感知，截断加省略号） */
static void clip_text(char *out, size_t outn, const char *s, int maxw)
{
    int w = 0;
    size_t bi = 0;
    while (s[bi]) {
        uint32_t cp;
        int n = term_utf8_decode(s + bi, &cp);
        int cw = term_char_width(cp);
        if (w + cw > maxw - 1) {
            break;
        }
        if (bi + n >= outn) {
            break;
        }
        w += cw;
        bi += (size_t)n;
    }
    if (s[bi]) {
        /* 补省略号 */
        while (w > maxw - 2 && bi > 0) {
            /* 回退一个字符 */
            size_t p = bi - 1;
            while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) {
                p--;
            }
            uint32_t cp;
            int n = term_utf8_decode(s + p, &cp);
            w -= term_char_width(cp);
            bi = p;
            (void)n;
        }
        memcpy(out, s, bi);
        out[bi] = '\0';
        strcat(out, "…");
    } else {
        memcpy(out, s, bi);
        out[bi] = '\0';
    }
}

/* ---------- Cookie / 状态 ---------- */

/* 联网完整刷新：WBI 密钥 + 登录状态验证（Ctrl-R / 登录后使用） */
static void refresh_session(void)
{
    free(U.cookie);
    U.cookie = login_full_cookie();
    U.q.cookie = U.cookie;
    free(U.img);
    free(U.sub);
    U.img = U.sub = NULL;
    if (bili_get_wbi_keys(U.cookie, &U.img, &U.sub, 0) != 0) {
        set_toast("获取 WBI 密钥失败，解析功能暂不可用");
    }
    U.q.img_key = U.img;
    U.q.sub_key = U.sub;
    U.uname[0] = '\0';
    char *un = NULL;
    U.logged = login_status(U.cookie, &un) == 1;
    if (un) {
        snprintf(U.uname, sizeof(U.uname), "%s", un);
        free(un);
    }
}

/* 本地会话加载（0 网络）：启动期使用，离线也能立即出界面。
 * 登录态以 SESSDATA 本地判断，用户名读缓存；WBI 未命中时留空，
 * 由 load_target 在解析前联网补取。 */
static void load_session_local(void)
{
    free(U.cookie);
    char *b3 = NULL, *b4 = NULL;
    bili_get_buvid_cached(&b3, &b4);
    U.cookie = login_build_cookie(b3, b4, NULL);
    free(b3);
    free(b4);
    U.q.cookie = U.cookie;

    free(U.img);
    free(U.sub);
    U.img = U.sub = NULL;
    bili_get_wbi_keys_cached(&U.img, &U.sub);
    U.q.img_key = U.img;
    U.q.sub_key = U.sub;

    U.uname[0] = '\0';
    char *un = NULL;
    U.logged = login_status_local(U.cookie, &un) == 1;
    if (un) {
        snprintf(U.uname, sizeof(U.uname), "%s", un);
        free(un);
    }
}

/* ---------- 首页加载目标 ---------- */

static void load_target(const char *text)
{
    /* 启动期 WBI 缓存未命中（或已过期）时，在解析前联网补取 */
    if (!U.img || !U.sub) {
        show_loading("获取 WBI 密钥...");
        if (bili_get_wbi_keys(U.cookie, &U.img, &U.sub, 0) != 0) {
            set_toast("获取 WBI 密钥失败，请检查网络");
            return;
        }
        U.q.img_key = U.img;
        U.q.sub_key = U.sub;
    }
    char *input = xstrdup(text);
    if (strstr(input, "b23.tv")) {
        char *final_url = NULL;
        if (http_resolve(input, &final_url) != 0) {
            set_toast("无法解析短链接");
            free(input);
            return;
        }
        free(input);
        input = final_url;
    }
    char *bvid = NULL;
    int url_page = 0;
    long num_id = 0;
    bvid_result_t pr = bvid_parse_input(input, &bvid, &url_page, &num_id);
    free(input);
    if (pr == BVID_UNSUPPORTED) {
        set_toast("暂不支持课程(cheese)链接");
        return;
    }
    if (pr != BVID_OK && pr != BVID_EP && pr != BVID_SS) {
        set_toast("无法识别的输入（支持 URL/BV/av/ep/ss）");
        return;
    }

    U.nitems = 0;
    U.nq = 0;
    U.qsel = 0;
    U.focus = 0;
    U.list_cursor = U.list_top = 0;

    if (pr == BVID_OK) {
        bili_view_t view;
        if (bili_view(bvid, U.cookie, U.img, U.sub, 0, &view) != 0) {
            set_toast("获取视频信息失败");
            free(bvid);
            return;
        }
        snprintf(U.dtitle, sizeof(U.dtitle), "%s", view.title);
        snprintf(U.dsub, sizeof(U.dsub), "UP: %s   时长: %s   %s",
                 view.owner ? view.owner : "-",
                 human_duration(view.duration), view.bvid);
        U.kind = 0;
        snprintf(U.bvid, sizeof(U.bvid), "%s", view.bvid);
        for (int i = 0; i < view.npages && U.nitems < MAX_ITEMS; i++) {
            ui_item_t *it = &U.items[U.nitems++];
            memset(it, 0, sizeof(*it));
            it->cid = view.pages[i].cid;
            it->page_no = view.pages[i].page;
            snprintf(it->part, sizeof(it->part), "%s",
                     view.pages[i].part ? view.pages[i].part : "-");
            char p_part[256];
            snprintf(p_part, sizeof(p_part), "%s", it->part);
            snprintf(it->label, sizeof(it->label), "P%d  %s",
                     view.pages[i].page, p_part);
        }
        /* 用第一个分P取可用清晰度 */
        if (view.npages > 0) {
            bili_playurl_t pu;
            if (bili_playurl(view.bvid, view.pages[0].cid,
                             125, U.cookie, U.img, U.sub,
                             0, &pu) == 0) {
                for (int i = 0; i < pu.naccept && i < 24; i++) {
                    U.qaccept[i] = pu.accept[i];
                }
                U.nq = pu.naccept < 24 ? pu.naccept : 24;
                /* 默认选中当前实际可得的档位（未登录时为 480P） */
                for (int i = 0; i < U.nq; i++) {
                    if (U.qaccept[i] == pu.quality) {
                        U.qsel = i;
                        break;
                    }
                }
                bili_playurl_free(&pu);
            }
        }
        bili_view_free(&view);
    } else {
        bangumi_season_t season;
        if (bangumi_season(pr == BVID_SS ? num_id : -1,
                           pr == BVID_EP ? num_id : -1,
                           U.cookie, 0, &season) != 0) {
            set_toast("获取番剧信息失败");
            return;
        }
        snprintf(U.dtitle, sizeof(U.dtitle), "%s", season.title);
        snprintf(U.dsub, sizeof(U.dsub), "番剧 ss%ld   共 %d 集", season.season_id, season.neps);
        U.kind = 1;
        U.season_id = season.season_id;
        int default_idx = 0;
        if (pr == BVID_EP) {
            for (int i = 0; i < season.neps; i++) {
                if (season.episodes[i].id == num_id) {
                    default_idx = i;
                    break;
                }
            }
        }
        for (int i = 0; i < season.neps && U.nitems < MAX_ITEMS; i++) {
            ui_item_t *it = &U.items[U.nitems++];
            memset(it, 0, sizeof(*it));
            it->cid = season.episodes[i].cid;
            it->ep_id = season.episodes[i].id;
            it->page_no = i + 1;
            snprintf(it->part, sizeof(it->part), "%s",
                     season.episodes[i].long_title ? season.episodes[i].long_title
                                                   : (season.episodes[i].title ? season.episodes[i].title : "-"));
            char ep_part[256];
            snprintf(ep_part, sizeof(ep_part), "%s", it->part);
            snprintf(it->label, sizeof(it->label), "EP%-2d %s", i + 1, ep_part);
            snprintf(it->dur, sizeof(it->dur), "%s", human_duration(season.episodes[i].duration));
        }
        if (season.neps > 0) {
            bili_playurl_t pu;
            const bangumi_episode_t *ep = &season.episodes[default_idx];
            if (bangumi_playurl(ep->cid, ep->id, 125, U.cookie, 0, &pu) == 0) {
                for (int i = 0; i < pu.naccept && i < 24; i++) {
                    U.qaccept[i] = pu.accept[i];
                }
                U.nq = pu.naccept < 24 ? pu.naccept : 24;
                for (int i = 0; i < U.nq; i++) {
                    if (U.qaccept[i] == pu.quality) {
                        U.qsel = i;
                        break;
                    }
                }
                bili_playurl_free(&pu);
            }
        }
        bangumi_season_free(&season);
    }
    free(bvid);
    U.screen = 1;
    U.list_cursor = U.list_top = 0;
}

/* ---------- 入队 ---------- */

static void enqueue_selected(void)
{
    int qn = U.nq > 0 ? U.qaccept[U.qsel] : 0;
    int added = 0;
    int only_cursor = 1;
    for (int i = 0; i < U.nitems; i++) {
        if (U.items[i].selected) {
            only_cursor = 0;
            break;
        }
    }
    for (int i = 0; i < U.nitems; i++) {
        if (only_cursor ? (i != U.list_cursor) : (!U.items[i].selected)) {
            continue;
        }
        ui_item_t *it = &U.items[i];
        bjob_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.kind = U.kind;
        desc.cid = it->cid;
        desc.ep_id = it->ep_id;
        desc.page_no = it->page_no;
        desc.npages = U.kind == 0 ? U.nitems : 0;
        snprintf(desc.bvid, sizeof(desc.bvid), "%s", U.bvid);
        snprintf(desc.title, sizeof(desc.title), "%s", U.dtitle);
        snprintf(desc.part, sizeof(desc.part), "%s", it->part);
        snprintf(desc.outdir, sizeof(desc.outdir), "%s", U.outdir);
        desc.qn = qn;
        bq_add(&U.q, &desc);
        added++;
    }
    for (int i = 0; i < U.nitems; i++) {
        U.items[i].selected = 0;
    }
    set_toast("已加入 %d 个下载任务", added);
}

/* ---------- 绘制 ---------- */

static void draw_header(void)
{
    char line[512];
    if (U.logged) {
        snprintf(line, sizeof(line), " bili │ 已登录: %s │ Ctrl-R 刷新 │ Ctrl-C 退出",
                 U.uname[0] ? U.uname : "（未验证）");
    } else {
        snprintf(line, sizeof(line), " bili │ 未登录（仅 480P）│ Ctrl-L 登录 │ Ctrl-R 刷新");
    }
    term_hline(0, 0, term_width(), ' ', C_FG_TEXT, C_BAR_BG);
    term_puts(0, 0, line, C_FG_TEXT, C_BAR_BG);
}

static void draw_footer(const char *keys)
{
    int y = term_height() - 1;
    term_hline(0, y, term_width(), ' ', C_FG_TEXT, C_BAR_BG);
    term_puts(0, y, keys, C_FG_TEXT, C_BAR_BG);
    if (U.toast[0] && time(NULL) < U.toast_until) {
        char t[256];
        clip_text(t, sizeof(t), U.toast, term_width() / 2);
        term_puts(term_width() - (int)strlen(t) - 2 > 0 ? term_width() - (int)term_str_width(t) - 1 : 0,
                  y, t, C_ACCENT, C_BAR_BG);
    }
}

static void draw_box(int x, int y, int w, int h, const char *title, int16_t cborder)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            uint32_t cp = ' ';
            if (j == y || j == y + h - 1) {
                cp = (i == x) ? 0x250C : (i == x + w - 1) ? 0x2510 : 0x2500;   /* ┌ ┐ ─ */
                if (i != x && i != x + w - 1) {
                    cp = (j == y + h - 1 && i == x) ? 0x2514 : cp;
                }
            } else if (i == x) {
                cp = (j == y + h - 1) ? 0x2514 : 0x2502; /* └ │ */
            } else if (i == x + w - 1) {
                cp = (j == y + h - 1) ? 0x2518 : 0x2502; /* ┘ │ */
            }
            term_putc(i, j, cp, cborder, C_DEFAULT);
        }
    }
    if (title && *title) {
        char t[256];
        clip_text(t, sizeof(t), title, w - 4);
        term_puts(x + 2, y, t, cborder, C_DEFAULT);
    }
}

static void draw_home(void)
{
    int W = term_width(), H = term_height();
    int bw = W > 76 ? 72 : W - 4;
    int bh = 10;
    int bx = (W - bw) / 2;
    int by = (H - bh) / 2 - 1;
    if (by < 1) {
        by = 1;
    }

    draw_box(bx, by, bw, bh, " bili — B 站视频解析下载 ", C_CYAN);

    const char *label = U.editing_outdir ? "输出目录:" : "链接/BV/av/ep/ss:";
    term_puts(bx + 2, by + 2, label, C_FG_DIM, C_DEFAULT);

    /* 输入行（长文本显示尾部，光标在末尾） */
    char shown[1024];
    const char *editbuf = U.editing_outdir ? U.outdir : U.input;
    int srclen = (int)strlen(editbuf);
    int maxw = bw - 6;
    /* 找到从尾部起宽度 <= maxw 的起始字节 */
    int start = 0;
    {
        int w = 0, i = srclen;
        while (i > 0) {
            size_t p = (size_t)i - 1;
            while (p > 0 && ((unsigned char)editbuf[p] & 0xC0) == 0x80) {
                p--;
            }
            uint32_t cp;
            term_utf8_decode(editbuf + p, &cp);
            int cw = term_char_width(cp);
            if (w + cw > maxw) {
                break;
            }
            w += cw;
            i = (int)p;
            start = i;
        }
    }
    snprintf(shown, sizeof(shown), "%s", editbuf + start);
    term_puts(bx + 2, by + 4, shown, C_FG_TEXT, C_DEFAULT);
    /* 光标：输入末尾 */
    term_setcursor(bx + 2 + term_str_width(shown), by + 4);

    char info[1200];
    snprintf(info, sizeof(info), "输出目录: %s", U.outdir);
    clip_text(info, sizeof(info), info, bw - 4);
    term_puts(bx + 2, by + 6, info, C_FG_DIM, C_DEFAULT);

    const char *hint = U.logged ? "已登录账号可下载高清晰度" : "未登录仅 480P，按 L 扫码登录解锁";
    term_puts(bx + 2, by + 7, hint, U.logged ? C_GREEN : C_ACCENT, C_DEFAULT);

    draw_footer(U.editing_outdir
                    ? "回车 确认目录 │ Esc 取消"
                    : (U.input[0] ? "回车 解析 │ Backspace 删除 │ Ctrl-O 目录 │ Ctrl-L 登录 │ Ctrl-C 退出"
                                  : "输入 链接/BV/av/ep/ss 后回车 │ Ctrl-L 登录 │ Ctrl-O 目录 │ Ctrl-R 刷新 │ Ctrl-C 退出"));
}

static void draw_progress_bar(int x, int y, int w, double pct, int16_t fg, int16_t bg)
{
    if (w < 4) {
        return;
    }
    int fill = (int)(pct * (w - 2) + 0.5);
    if (fill < 0) fill = 0;
    if (fill > w - 2) fill = w - 2;
    term_putc(x, y, '[', fg, bg);
    for (int i = 0; i < w - 2; i++) {
        term_putc(x + 1 + i, y, i < fill ? '=' : ' ', fg, bg);
    }
    term_putc(x + w - 1, y, ']', fg, bg);
}

static double job_progress(const bjob_t *j)
{
    if (j->state == JOB_DONE) {
        return 1.0;
    }
    if (j->state != JOB_DL_VIDEO && j->state != JOB_DL_AUDIO) {
        return 0.0;
    }
    uint64_t want = j->state == JOB_DL_VIDEO ? j->vsize : j->asize;
    if (want == 0) {
        return 0.0;
    }
    return 1.0; /* 具体百分比由 note 展示，条上给近似值 */
}

static void draw_queue_panel(int y0, int h)
{
    char title[128];
    snprintf(title, sizeof(title), " 队列 (进行 %d / 共 %d)",
             bq_active_count(&U.q), U.q.n);
    draw_box(0, y0, term_width(), h, title, U.focus == 1 ? C_CYAN : C_BORDER);

    int rows = h - 2;
    if (U.q.n == 0) {
        term_puts(2, y0 + 1, "空闲 — 在列表中按 D 下载", C_FG_DIM, C_DEFAULT);
        return;
    }
    /* 滚动 */
    if (U.queue_cursor < 0) {
        U.queue_cursor = 0;
    }
    if (U.queue_cursor >= U.q.n) {
        U.queue_cursor = U.q.n - 1;
    }
    if (U.queue_cursor < 0) {
        U.queue_cursor = 0;
    }
    int top = 0;
    if (U.q.n > rows) {
        top = U.queue_cursor - rows + 1;
        if (top < 0) top = 0;
    }
    for (int i = 0; i < rows && top + i < U.q.n; i++) {
        int ji = top + i;
        bjob_t *j = &U.q.jobs[ji];
        int y = y0 + 1 + i;
        int selected = (U.focus == 1 && ji == U.queue_cursor);
        int16_t fg = C_FG_TEXT;
        const char *mark = "  ";
        if (selected) {
            term_fill(1, y, term_width() - 2, 1, C_FG_TEXT, C_SEL_BG);
            mark = "> ";
        }
        switch (j->state) {
        case JOB_DONE: fg = C_GREEN; break;
        case JOB_FAILED: fg = C_RED; break;
        case JOB_CANCELED: fg = C_FG_DIM; break;
        default: fg = C_CYAN; break;
        }
        char name[128];
        clip_text(name, sizeof(name), j->base, term_width() / 2 - 10);
        term_puts(1, y, mark, fg, selected ? C_SEL_BG : C_DEFAULT);
        term_puts(3, y, name, fg, selected ? C_SEL_BG : C_DEFAULT);
        int nx = term_width() / 2 - 8;
        draw_progress_bar(nx, y, 20, job_progress(j), fg, selected ? C_SEL_BG : C_DEFAULT);
        char note[160];
        clip_text(note, sizeof(note), j->note, term_width() - nx - 22);
        term_puts(nx + 22, y, note, fg, selected ? C_SEL_BG : C_DEFAULT);
        if (j->state == JOB_FAILED && j->err[0]) {
            char err[128];
            clip_text(err, sizeof(err), j->err, 40);
            term_puts(nx + 22 + (int)term_str_width(note) + 2, y, err, C_RED,
                      selected ? C_SEL_BG : C_DEFAULT);
        }
    }
}

static void draw_detail(void)
{
    int W = term_width(), H = term_height();
    int qh = QUEUE_ROWS;
    int list_h = H - qh - 2; /* 顶栏1 + 列表 + 队列 + 底行 */

    char hdr[512];
    snprintf(hdr, sizeof(hdr), "%s", U.dtitle);
    char hdr2[1200];
    snprintf(hdr2, sizeof(hdr2), "%s   │   %s", hdr, U.dsub);
    char clipped[512];
    clip_text(clipped, sizeof(clipped), hdr2, W - 2);
    term_puts(1, 1, clipped, C_CYAN, C_DEFAULT);

    /* 清晰度行 */
    char qline[256] = "";
    int off = 0;
    for (int i = 0; i < U.nq && off < (int)sizeof(qline) - 24; i++) {
        char one[32];
        (void)snprintf(one, sizeof(one), "%s%d(%s)",
                         i ? " " : "", U.qaccept[i], bili_qn_name(U.qaccept[i]));
        if (i == U.qsel) {
            snprintf(qline + off, sizeof(qline) - off, "[%s]", one + (i ? 1 : 0));
            off = (int)strlen(qline);
        } else {
            snprintf(qline + off, sizeof(qline) - off, "%s", one);
            off = (int)strlen(qline);
        }
    }
    if (U.nq == 0) {
        snprintf(qline, sizeof(qline), "清晰度: 最高可用");
    }
    term_puts(1, 2, qline, C_ACCENT, C_DEFAULT);

    /* 列表 */
    int lh = list_h - 3;
    if (lh > U.nitems) {
        lh = U.nitems;
    }
    if (U.list_cursor >= U.nitems) {
        U.list_cursor = U.nitems - 1;
    }
    if (U.list_cursor < 0) {
        U.list_cursor = 0;
    }
    if (U.list_cursor < U.list_top) {
        U.list_top = U.list_cursor;
    }
    int max_top = U.nitems > lh ? U.nitems - lh : 0;
    if (U.list_cursor >= U.list_top + lh) {
        U.list_top = U.list_cursor - lh + 1;
    }
    if (U.list_top > max_top) {
        U.list_top = max_top;
    }
    if (U.list_top < 0) {
        U.list_top = 0;
    }

    for (int r = 0; r < lh; r++) {
        int i = U.list_top + r;
        if (i >= U.nitems) {
            break;
        }
        ui_item_t *it = &U.items[i];
        int y = 4 + r;
        int cursor = (U.focus == 0 && i == U.list_cursor);
        if (cursor) {
            term_fill(0, y, W, 1, C_FG_TEXT, C_SEL_BG);
        }
        int16_t fg = C_FG_TEXT;
        term_puts(1, y, cursor ? "▸ " : "  ", C_CYAN, cursor ? C_SEL_BG : C_DEFAULT);
        term_puts(3, y, it->selected ? "[x]" : "[ ]",
                  it->selected ? C_ACCENT : C_FG_DIM, cursor ? C_SEL_BG : C_DEFAULT);
        char label[512];
        clip_text(label, sizeof(label), it->label, W / 2);
        term_puts(8, y, label, fg, cursor ? C_SEL_BG : C_DEFAULT);
        if (it->dur[0]) {
            term_puts(W - 10, y, it->dur, C_FG_DIM, cursor ? C_SEL_BG : C_DEFAULT);
        }
    }
    if (U.nitems == 0) {
        term_puts(2, 4, "没有可下载的条目", C_FG_DIM, C_DEFAULT);
    }

    draw_queue_panel(H - qh - 1, qh);

    char keys[256];
    if (U.focus == 0) {
        snprintf(keys, sizeof(keys),
                 "↑↓ 选择 │ 空格 勾选 │ Ctrl-A 全选 │ Ctrl-D 下载(%s) │ Ctrl-Q 清晰度 │ Ctrl-L 登录 │ Ctrl-R 刷新 │ Tab 队列 │ Esc 返回",
                 U.nq > 0 ? bili_qn_name(U.qaccept[U.qsel]) : "自动");
    } else {
        snprintf(keys, sizeof(keys),
                 "↑↓ 选择任务 │ Ctrl-X 取消 │ Ctrl-F 清理已完成 │ Tab 列表 │ Esc 返回 │ Ctrl-C 退出");
    }
    draw_footer(keys);
}

static void draw(void)
{
    term_clear(C_DEFAULT, C_DEFAULT);
    draw_header();
    if (U.screen == 0) {
        draw_home();
    } else {
        draw_detail();
    }
    term_flush();
}

/* 加载遮罩（阻塞操作前显示） */
static void show_loading(const char *text)
{
    draw_header();
    int W = term_width(), H = term_height();
    term_puts((W - (int)term_str_width(text)) / 2, H / 2, text, C_CYAN, C_DEFAULT);
    term_flush();
}

/* ---------- 按键处理 ---------- */

static void input_append(uint32_t cp)
{
    char *buf = U.editing_outdir ? U.outdir : U.input;
    size_t cap = U.editing_outdir ? sizeof(U.outdir) : sizeof(U.input);
    size_t l = strlen(buf);
    if (l + 5 >= cap || cp < 0x20) {
        return;
    }
    char tmp[4];
    int n = term_utf8_encode(cp, tmp);
    memcpy(buf + l, tmp, (size_t)n);
    buf[l + (size_t)n] = '\0';
}

static void input_backspace(void)
{
    char *buf = U.editing_outdir ? U.outdir : U.input;
    size_t l = strlen(buf);
    if (l == 0) {
        return;
    }
    size_t p = l - 1;
    while (p > 0 && ((unsigned char)buf[p] & 0xC0) == 0x80) {
        p--;
    }
    buf[p] = '\0';
}

static void do_refresh(void)
{
    show_loading("刷新登录状态...");
    refresh_session();
    set_toast("已刷新：%s", U.logged ? "已登录" : "未登录");
}

static void do_login(void)
{
    term_suspend();
    printf("正在获取登录二维码...\n");
    fflush(stdout);
    login_qr_flow(0);
    refresh_session();
    term_resume();
}

static void handle_key(const term_key_t *k)
{
    /* 全局退出 */
    if (k->type == KEY_CTRL && k->cp == 'c') {
        U.quit = 1;
        return;
    }

    if (U.screen == 0) {
        /* 首页：输入框始终聚焦，可见字符一律作为文本输入 */
        switch (k->type) {
        case KEY_CHAR:
            if (k->cp >= 0x20) {
                input_append(k->cp);
            }
            break;
        case KEY_BACKSPACE:
            input_backspace();
            break;
        case KEY_ENTER:
            if (U.editing_outdir) {
                U.editing_outdir = 0;
            } else if (U.input[0]) {
                show_loading("解析中，请稍候...");
                load_target(U.input);
                if (U.screen == 1) {
                    U.input[0] = '\0';
                }
            }
            break;
        case KEY_CTRL:
            if (k->cp == 'o') {
                U.editing_outdir = !U.editing_outdir;
            } else if (k->cp == 'l') {
                do_login();
            } else if (k->cp == 'r') {
                do_refresh();
            }
            break;
        case KEY_ESC:
            if (U.editing_outdir) {
                U.editing_outdir = 0;
            }
            break;
        default:
            break;
        }
        return;
    }

    /* 详情页 */
    switch (k->type) {
    case KEY_UP:
        if (U.focus == 0) {
            U.list_cursor--;
        } else {
            U.queue_cursor--;
        }
        break;
    case KEY_DOWN:
        if (U.focus == 0) {
            U.list_cursor++;
        } else {
            U.queue_cursor++;
        }
        break;
    case KEY_PGUP:
        if (U.focus == 0) {
            U.list_cursor -= 10;
        } else {
            U.queue_cursor -= 5;
        }
        break;
    case KEY_PGDN:
        if (U.focus == 0) {
            U.list_cursor += 10;
        } else {
            U.queue_cursor += 5;
        }
        break;
    case KEY_HOME:
        if (U.focus == 0) U.list_cursor = 0; else U.queue_cursor = 0;
        break;
    case KEY_END:
        if (U.focus == 0) U.list_cursor = U.nitems - 1; else U.queue_cursor = U.q.n - 1;
        break;
    case KEY_TAB:
        U.focus = !U.focus;
        break;
    case KEY_ESC:
        if (U.focus == 1) {
            U.focus = 0; /* 队列 → 列表 */
        } else {
            U.screen = 0; /* 列表 → 首页 */
        }
        break;
    case KEY_CHAR:
        /* 空格是唯一的非 Ctrl 功能键（勾选光标项） */
        if (k->cp == ' ' && U.focus == 0) {
            U.items[U.list_cursor].selected = !U.items[U.list_cursor].selected;
        }
        break;
    case KEY_CTRL:
        switch (k->cp) {
        case 'a':
            if (U.focus == 0) {
                int all = 1;
                for (int i = 0; i < U.nitems; i++) {
                    if (!U.items[i].selected) {
                        all = 0;
                        break;
                    }
                }
                for (int i = 0; i < U.nitems; i++) {
                    U.items[i].selected = !all;
                }
            }
            break;
        case 'd':
            if (U.focus == 0) {
                enqueue_selected();
            }
            break;
        case 'q':
            if (U.focus == 0 && U.nq > 0) {
                U.qsel = (U.qsel + 1) % U.nq;
                set_toast("清晰度: %d (%s)%s", U.qaccept[U.qsel],
                          bili_qn_name(U.qaccept[U.qsel]),
                          U.qaccept[U.qsel] > 80 && !U.logged ? "（需登录，实际可能受限）" : "");
            }
            break;
        case 'x':
            if (U.focus == 1) {
                bq_cancel(&U.q, U.queue_cursor);
            }
            break;
        case 'f':
            if (U.focus == 1) {
                bq_remove_finished(&U.q);
            }
            break;
        case 'l':
            do_login();
            break;
        case 'r':
            do_refresh();
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

/* ---------- 主循环 ---------- */

int tui_main(void)
{
    memset(&U, 0, sizeof(U));
    U.screen = 0;
    U.outdir[0] = '.';
    U.outdir[1] = '\0';

    /* TUI 模式下库层错误输出重定向到日志，避免破坏屏幕 */
    freopen("bili-tui.log", "a", stderr);

    term_init();

    /* 本地会话加载（0 网络，瞬时完成），无需加载遮罩 */
    load_session_local();
    bq_init(&U.q, U.cookie, U.img, U.sub, 0);

    term_key_t k;
    while (!U.quit) {
        if (term_check_resized()) {
            /* 尺寸变化后 term 内部已重置缓冲，下一帧全量重绘 */
        }
        bq_tick(&U.q);
        draw();
        int have = term_poll_key(200, &k);
        if (have) {
            handle_key(&k);
        } else {
            /* 无按键的周期刷新（进度变化）由下一轮 draw 完成 */
            ;
        }
    }
    term_shutdown();
    free(U.cookie);
    free(U.img);
    free(U.sub);
    return 0;
}
