# bili-cli — B 站视频解析下载命令行工具

B 站视频解析下载命令行工具，仅依赖系统 `curl` 与 `ffmpeg`，
编译本身零外部依赖（第三方库已内嵌于 `src/vendor/`）。

## 构建

构建逻辑统一在 [nob.c](nob.c)（基于 [tsoding/nob.h](https://github.com/tsoding/nob.h)，
Public Domain 单头文件）。`nob.c` 变化后 `./nob` 会自动重建自身，无需手动干预。

```bash
cc nob.c -o nob     # 首次引导
./nob               # 默认：构建 bili（动态）+ bili-static（musl 静态单文件）
./nob static        # 只构建静态版
./nob clean         # 清理产物
```

需要：C11 编译器（gcc/clang）。运行需要系统装有 `curl` 和 `ffmpeg`。

静态构建使用内嵌工具链 `.musl/install/bin/musl-gcc`（musl 1.2.5，源码自动编译，
无需 root）。如需从零引导：

```bash
mkdir -p .musl && cd .musl
curl -O https://musl.libc.org/releases/musl-1.2.5.tar.gz && tar xzf musl-1.2.5.tar.gz
mkdir build && cd build && ../musl-1.2.5/configure --prefix=$PWD/../install && make -j8 && make install
cd ../.. && ./nob static
``
番剧接口与普通视频共用一套 DASH 下载与混流逻辑（`pgc/player/web/playurl`，
无需 WBI 签名）；`-e` 支持选集（`3` / `1,3,5-8` / `all`）。

## 使用

```bash
./bili                 # 全屏 TUI 界面（推荐；htop 式，方向键 + 空格勾选 + 底部队列）
./bili shell           # 行交互模式（REPL）
./bili login           # 单独扫码登录（终端二维码，凭证保存到 state 目录）
./bili whoami          # 查看登录状态
./bili logout          # 退出登录

# 交互模式命令: info/dl/set/login/whoami/quit，如:
#   dl BV1GJ411x7h7 -q 1080p
#   set qn 1080p
#   set outdir ~/Videos

# 查看视频信息与可用清晰度
./bili --info "https://www.bilibili.com/video/BV1GJ411x7h7"

# 下载（默认第1P、最高可用清晰度、混流为 mkv）
./bili BV1GJ411x7h7

# 指定清晰度 / 分P / 输出目录 / 登录凭证
./bili av80433022 -q 1080p -p 2 -o ~/Videos -s "<SESSDATA>"

# 全部分P / 仅音频 / 不混流
./bili BV1GJ411x7h7 -p all
./bili BV1GJ411x7h7 --audio-only
./bili BV1GJ411x7h7 --no-mux --mp4
```

输出结构：`<输出目录>/<视频标题>/<标题>[_P<n>].mkv`，已完成的流文件自动跳过（断点续传）。

## 登录

`bili login` 通过 B 站 Web 二维码登录协议获取凭证（qrcode/generate -> 轮询
qrcode/poll），终端以半块字符 + 固定 ANSI 颜色渲染二维码（不依赖终端主题，
可被 APP 正常识别）。凭证（SESSDATA/bili_jct 等）保存于
`$XDG_STATE_HOME/bili-cli/cookie.txt`，之后所有请求自动携带，解除 480P 限制。
登录后可下载 1080P 及以上清晰度（以账号权益为准）；`--sessdata` 仍可手动覆盖。

## 实现的协议链路

1. `GET /x/frontend/finger/spi` 获取设备指纹 buvid3/buvid4（缓存在
   `$XDG_CACHE_HOME/bili-cli/buvid.txt`，长期有效）
2. `GET /x/web-interface/nav` 获取 WBI 密钥（未登录亦返回；缓存 6 小时）
3. WBI 签名（mixin key 重排 + 参数过滤 + `w_rid = md5(query + mixin_key)`）请求
   `x/web-interface/wbi/view` 与 `x/player/wbi/playurl`
4. DASH 流选择（视频取 `qn <= 期望值` 的最高档，音频优先 30280 > 30232 > 30216）
5. `curl`（`Referer: https://www.bilibili.com`）下载音视频流，`ffmpeg -c copy` 混流

模块：`md5`（RFC 1321）、`wbi`（签名）、`bvid`（输入解析与 av/BV 互转）、
`http`（系统 curl 封装）、`bili`（API 层）、`bangumi`（番剧）、`login`（扫码登录）、
`term`（终端后端：raw 输入 + 差量重绘，零依赖）、`queue`（下载队列引擎，无线程）、
`tui`（全屏界面）、`main`（入口与一次性命令）。

TUI 快捷键（统一为 Ctrl+字母；导航键除外）：
  - 首页：回车 解析；Backspace 删除；Ctrl-O 输出目录；Ctrl-L 登录；Ctrl-R 刷新状态；Ctrl-C 退出
  - 列表：↑↓/PgUp/PgDn 选择；空格 勾选；Ctrl-A 全选/反选；Ctrl-D 下载；Ctrl-Q 切换清晰度；Tab 进队列；Esc 返回首页
  - 队列：↑↓ 选择任务；Ctrl-X 取消；Ctrl-F 清理已完成；Tab 回列表；Esc 返回
  - 全局：Ctrl-L 登录；Ctrl-R 刷新登录状态；Ctrl-C 退出

## 已知限制

- 课程（cheese）链接暂不支持
- 弹幕下载未实现（B 站弹幕为 protobuf 格式）
- 番剧付费内容需登录且持有有效权益，未登录仅能获取预览（试看）片段
- 4K/8K/HDR 等高清晰度同样取决于登录账号权益

内嵌第三方库：[cJSON](https://github.com/DaveGamble/cJSON)、
[qrcodegen](https://www.nayuki.io/page/qr-code-generator-library)（均为 MIT License）。

## 测试

```bash
./bili --selftest                    # 内置自检（MD5 / av-BV 转换）
python3 test/tui_smoke_test.py       # TUI 端到端冒烟测试（pty 驱动，真实下载一条）
```
