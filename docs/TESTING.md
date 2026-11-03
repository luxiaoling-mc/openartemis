# openartemis 测试与运行指南 (TESTING)

本文件是 openartemis 的**测试布局**与**仍然生效的 `OA_*` 环境变量清单**。
仓库内唯一权威清单：改动源码里的 `getenv("OA_...")` 时同步本表；删掉的开关
不再保留历史说明（历史研究记录见 `docs/CORE_NOTES.md`）。

---

## 1. app 程序

只有一个：`openartemis`（`src/app/main.cpp`）。ctest 不跑 app；app 层验证
= 跑真包冒烟并 `--dump` 一帧。

### CLI

```
openartemis [options] [project.pfs]     # pfs 或解包目录（含 system.ini）
  --frames N          跑 N 帧后退出（结束时写 --dump）
  --dump PATH         把最后一帧写成 PPM
  --headless          无窗口（固定 16ms tick）
  --fixed-delta-ms N  确定性步长（窗口模式也可用）
  --fps N             目标帧率（缺省按真实时钟）
  --platform NAME     windows / android / wasm（缺省=构建平台）
  --renderer NAME     sdl（默认）/ gles
  --mute              静音
  -h, --help
```

窗口模式 Esc / 关窗 / 游戏自身 exit 退出；存档根默认 = 游戏数据所在目录
（可用 `OA_SAVE_ROOT` 覆盖）。离屏/无人值守跑一律加 `--mute`：设备、解码、
混音队列照旧，只把主音量置 0。

---

## 2. 测试布局

- `tests/` — **通用引擎单测**（自含 fixture，不需要真实游戏包）：
  compositor / layer_semantics / transform / render2 / text_domain / text_reveal /
  media_state / media_runtime / video_decode / video_mask / decode_pool /
  save_domain / save_compat / save_thumb_readback / p1c2_control /
  backlog_var_bridge / misc_b / emote_track_sample / emote_warp 等。
- `tests/fpm/` — **真实游戏回归**（缺包时自跳 exit 77）。只依赖 **4 个包**：
  `OA_TEST_FPM_PFS`（ハミダシクリエイティブ 中文版，UI/剧情/文本主包）、
  `OA_TEST_NEKOMIKO_PFS`（NekoMiko，emote / wmv / 字体）、
  `OA_TEST_RR_PFS`（常轨脱离ReReCall，影片音频：OP 引擎级 + 启动链 +
  driver idle-gap + 音频 bound/EOF flush）、
  `OA_TEST_TG3_PFS`（甜蜜女友3，timeline 手势/循环语义）。
- 全量：`ctest --test-dir build/windows -C Release`（**69 项**；真包测试需按
  上文准备，缺包只是 skip 不是 fail）。

包路径在 configure 期用 `-D` 或环境变量给（cache 优先于 env）；两个都没给时
相关测试自跳。例：

```sh
cmake -S . -B build/windows -DOA_ENABLE_TESTS=ON \
  -DOA_TEST_FPM_PFS=/games/fpm/root.pfs \
  -DOA_TEST_NEKOMIKO_PFS=/games/nm/root.pfs \
  -DOA_TEST_RR_PFS=/games/rr/root.pfs \
  -DOA_TEST_TG3_PFS=/games/tg3/root.pfs
```

---

## 3. 环境变量

### A. 宿主 / app

| 变量 | 作用 | 示例 |
|---|---|---|
| `OA_PFS` | 无位置参数时的默认项目（没有本机硬编码默认值） | `OA_PFS=/x/root.pfs ./openartemis --frames 900` |
| `OA_SAVE_ROOT` | 覆盖可写存档根目录（默认=平台默认：desktop 用游戏数据目录，android 用 app 私有目录，wasm 用 IDBFS `/save`） | `OA_SAVE_ROOT=/tmp/s1 ./openartemis a.pfs` |
| `OA_RENDERER` | 默认渲染后端 `sdl`（默认）/ `gles`，等价于 `--renderer` | `OA_RENDERER=gles ./openartemis a.pfs` |

### B. 回归门控（configure 期）

| 变量 | 作用 |
|---|---|
| `OA_TEST_FPM_PFS` | 主回归包路径（ハミダシクリエイティブ 中文版）。`tests/fpm/` 大多数测试与部分通用测试（asb/ini/script/video_decode 等）靠它工作 |
| `OA_TEST_NEKOMIKO_PFS` | NekoMiko 包：`nekomiko_p0` / `emote_static` / `emote_chain` / `emote_playback` / `font_face` / `wmv_decode` |
| `OA_TEST_RR_PFS` | 影片音频包：`rr_movie_audio` / `tm_movie_gap` / `movie_audio_bound` |
| `OA_TEST_TG3_PFS` | E-mote timeline 包：`emote_timeline` |

### C. 引擎诊断（core 内 env 门控，不设即零开销零输出）

| 变量 | 作用 | 模块 |
|---|---|---|
| `OA_SCENEDUMP` | 按 id 前缀转储场景节点（`id left top w h vis alpha eff file`），`""` = 全部。核对"某层到底在不在/位置对不对" | `src/core/runtime/runtime.cpp` |
| `OA_VARDUMP` | 结束前按前缀导出引擎变量表（local/temp/global/system），`""` = 全部 | 同上 |
| `OA_TXTDUMP` | 转储每个消息层（`chars/reveal/pending/hidden/positioned/page/alpha/vis/box/size/align/sample`）。区分"文本没派发进来"（全层 `chars=0`）与"进了错的层/没定位" | 同上 |
| `OA_PLAN_REUSE` | 帧计划复用开关（默认开）。计划按"层容器版本 + 文本版本 + 全局淡出 + stage 尺寸"缓存；宿主供帧不失效计划。`=0` 强制每帧重建，用于定位"复用门漏失效信号"（A/B：同帧开/关比像素与 `[scene]`） | `src/core/render/renderer.cpp` |

### D. 探针 / 工具的旋钮（非 ctest，report-only）

这些只被对应探针/工具读取，**不进 ctest**；每个文件头部注释就是它的完整文档：

| 变量族 | 归属 |
|---|---|
| `OA_BT91_*`（`PAGES` / `PRE` / `KEY` / `LOAD` / `LUABLOG` / `F8TICKS` / `SAVE` / `PFS` / `NOF8` / `DBGKEYS` / `LUABLOG_IMMED`） | `tests/fpm/bt91_backlog_face_probe.cpp`（backlog 行头像几何/世界 AABB） |
| `OA_SELDEEP` / `OA_P2DEEP` / `OA_P2TRACE` / `OA_SELBACK*` / `OA_SELDBL*` / `OA_LOG_ALL_PAGES` / `OA_DUMP_EMOTE_PAGE` / `OA_DUMP_EMOTE_LATE` / `OA_TEST_STORE_DIR` | `tests/fpm/nekomiko_p0_test.cpp`（深跑/选择支/表情帧导出） |
| `OA_PERF_DUMP` | `tests/fpm/emote_perf_probe.cpp` |
| `OA_MEM_NOPUMP` / `OA_MEM_GPU` | `tests/fpm/mem_churn_probe.cpp` |
| `OA_VPROBE_FRAMES` | `tests/fpm/video_perf_probe.cpp` |
| `OA_ES_PAGES` / `OA_ES_SAVE` | `tests/fpm/emote_state_probe.cpp` |
| `OA_SO_PAGES` / `OA_SO_LOAD` / `OA_SO_SAVEDIR` / `OA_TEST_SO_PFS` | `tests/fpm/so90_face_probe.cpp` |
| `OA_HCT_PROBE_*` / `OA_TEST_HCT_PFS` | `tests/fpm/hct_journey_probe.cpp` |
| `OA_SN101_CLICKDURING` / `OA_SN101_SAVESRC` / `OA_TEST_SNLL_PFS` | `tests/fpm/sn101_montage_probe.cpp` |
| `OA_SAKURA_PROBE_*` | `tests/fpm/sakura_mask_probe.cpp` |
| `OA_LAT_ARM` / `OA_LAT_LOAD` / `OA_LAT_RUNS` / `OA_LAT_SEED` | `tests/fpm/audio_latency_dist_probe.cpp` |
| `OA_AM_PROBE_PAGES` | `tests/fpm/automode_probe.cpp` |
| `OA_WMV_DUMP` | `tests/fpm/wmv_decode_test.cpp` |
| `OA_FRAMES` / `OA_SAMPLE_ALL` / `OA_SAMPLE_LEGACY` | `tools/psb.cpp` |
| `OA_UX_TRACE` | `tests/fpm/ux_journey_test.cpp` |
| `OA_TEST_OFFICIAL_SAVE_DIR` | `tests/official_save_test.cpp` |

> `OA_TEST_FPM_PFS` 也被少量通用测试的 CMake 条目注入（asb/ini/script/
> video_decode 等）——它们确实读这个变量，不再是历史遗留。

---

## 4. 快速验证

```sh
cmake -S . -B build/windows -DOA_ENABLE_TESTS=ON   # 或 --preset
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release          # 69 项

# app 冒烟（离屏 + 一帧 PPM）
OA_SAVE_ROOT=/tmp/s ./build/windows/src/app/Release/openartemis \
  --frames 300 --mute --fps 60 --dump /tmp/frame.ppm /path/root.pfs

# 视觉核对某层/某段文本（结束前转储）
OA_SCENEDUMP="500" OA_TXTDUMP=1 OA_SAVE_ROOT=/tmp/s \
  ./build/windows/src/app/Release/openartemis --headless --frames 600 /path/root.pfs
```
