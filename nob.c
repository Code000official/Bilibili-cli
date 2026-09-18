/* nob.c - 构建脚本（tsoding/nob.h v3.10.0，Public Domain）
 *
 * 本文件是全部构建逻辑的唯一来源。修改后无需手动重编：
 * NOB_GO_REBUILD_URSELF 会在运行时检测 nob.c/nob.h 变化并自动重建自身。
 *
 * 用法:
 *   cc nob.c -o nob     # 首次引导（或 make）
 *   ./nob               # 默认：构建 bili 与 bili-static
 *   ./nob static        # 只构建静态版 bili-static
 *   ./nob clean         # 清理产物
 */
#define NOB_IMPLEMENTATION
#include "nob.h"

#include <unistd.h>

#define MUSL_CC ".musl/install/bin/musl-gcc"

/* 收集全部源文件：src/*.c 自动扫描 + vendor 两个内嵌库 */
static bool collect_sources(Nob_Cmd *cmd)
{
    Nob_File_Paths children = { 0 };
    if (!nob_read_entire_dir("src", &children)) {
        return false;
    }
    for (size_t i = 0; i < children.count; i++) {
        const char *name = children.items[i];
        size_t len = strlen(name);
        if (len > 2 && strcmp(name + len - 2, ".c") == 0) {
            Nob_String_Builder sb = { 0 };
            nob_sb_appendf(&sb, "src/%s", name);
            nob_sb_append_null(&sb);
            nob_da_append(cmd, sb.items); /* 字符串随构建进程生命周期，不释放 */
        }
    }
    nob_cmd_append(cmd, "src/vendor/cJSON.c", "src/vendor/qrcodegen.c");
    return true;
}

/* cc -O2 -std=c11 -Wall -Wextra [-static] -o out <全部源文件> [&& strip out] */
static bool build_one(const char *cc, const char *out, bool link_static)
{
    Nob_Cmd cmd = { 0 };
    nob_cmd_append(&cmd, cc, "-O2", "-std=c11", "-Wall", "-Wextra",
                   "-D_GNU_SOURCE", "-Isrc", "-Isrc/vendor");
    if (link_static) {
        nob_cmd_append(&cmd, "-static");
    }
    nob_cmd_append(&cmd, "-o", out);
    if (!collect_sources(&cmd)) {
        return false;
    }
    if (!nob_cmd_run(&cmd)) {
        return false;
    }
    if (link_static) {
        Nob_Cmd strip = { 0 };
        nob_cmd_append(&strip, "strip", out);
        if (!nob_cmd_run(&strip)) {
            return false;
        }
    }
    nob_log(NOB_INFO, "构建完成: %s", out);
    return true;
}

int main(int argc, char **argv)
{
    /* 加固：把 argv[0] 固定为真实绝对路径，自重建永远只作用于 nob 自己，
     * 不受调用方式（相对路径/PATH 查找）影响 */
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        argv[0] = self;
    }
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *what = argc > 1 ? argv[1] : "all";

    if (strcmp(what, "clean") == 0) {
        static const char *artifacts[] = { "bili", "bili-static", "bili-tui.log", "nob.old" };
        for (size_t i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
            if (nob_file_exists(artifacts[i])) {
                nob_delete_file(artifacts[i]);
            }
        }
        nob_log(NOB_INFO, "已清理");
        return 0;
    }

    if (strcmp(what, "static") == 0) {
        if (!nob_file_exists(MUSL_CC)) {
            nob_log(NOB_ERROR,
                    "静态构建需要 musl 工具链，未找到 %s。引导方法见 README 构建"
                    "一节，或运行: make static 前先完成 musl 自举", MUSL_CC);
            return 1;
        }
        return build_one(MUSL_CC, "bili-static", true) ? 0 : 1;
    }

    if (strcmp(what, "all") == 0) {
        bool ok = build_one("cc", "bili", false);
        if (!nob_file_exists(MUSL_CC)) {
            nob_log(NOB_WARNING,
                    "跳过 bili-static：未找到 musl 工具链 %s（引导方法见 README 构建一节）",
                    MUSL_CC);
        } else {
            ok = build_one(MUSL_CC, "bili-static", true) && ok;
        }
        return ok ? 0 : 1;
    }

    nob_log(NOB_ERROR, "未知命令 '%s'（可用: all[默认] / static / clean）", what);
    return 1;
}
