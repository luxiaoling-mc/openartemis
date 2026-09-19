# openartemis 测试与运行指南 (TESTING)

本文件是 openartemis 仓库的**测试/运行环境变量总表**与测试布局说明：
两个 app 程序、tests/ 布局、以及全部 `OA_*` 环境变量（名称/作用/所属模块/示例/分类）。
仓库内唯一权威清单——改动源码中 `getenv("OA_...")` 时请同步本表。

---

## 1. 两个 app 程序

同一份实现（`src/app/main.cpp`）编译两次：

| 程序 | 源 | 内容 | 谁用 |
|---|---|---|---|
| `openartemis` | `src/app/main.cpp`（默认无宏） | **干净用户版**：正常 CLI（数据源、`--headless/--frames/--fps/--dump`、`OA_SAVE_ROOT`、Esc 退出、约 5s 状态行、--frames 结束统计）；**无** autodrive/探针/合成输入/OA_* 测试钩子 | 用户/真机验证 |
| `openartemis_test` | `src/app/main_test.cpp`（`#define OA_TEST_BUILD 1` 后 include main.cpp，宿主内小钩子）+ `src/app/app_test_drive.cpp`（第二 TU：autodrive 实现，入口声明见 `app_host.h`） | 测试超集 = 用户版 + **全部测试设施**（下节 B/C） | AI/回归/验收（autodrive 流） |

两份程序除测试设施外行为一致；ctest 不直接跑 app（app 层验证见 §3 验收流）。
构建：`cmake --build build/default -j8` → `build/default/src/app/openartemis[_test]`。

### 用户版 CLI（两程序相同）

```
openartemis [options] [project.pfs]     # pfs 或解包目录（含 system.ini）
  --frames N    跑 N 帧后打印结束统计并退出（无此参数则持续运行）
  --fps N       窗口模式目标帧率
  --headless    无窗口驱动（虚拟 16ms tick）；可配 --frames
  --dump PATH   结束时把最后一帧写为 PPM
  -h,--help
```
窗口模式 Esc / 窗口关闭 / 游戏自身 exit 请求退出；默认存档根 = 游戏数据所在目录。

---

## 2. 测试布局

- `tests/` — **通用引擎单测**（自含 fixture，不需要真实游戏包）：
  compositor/layer_semantics/transform/event_tags/ini/dir_fs/pfs(小样本)/script/
  interpreter/lua*/variable/asb/input_dispatch/render2/text_domain/text_reveal/
  media_state/media_runtime/video_decode/save_domain/save_thumb_readback/
  p1c2_control/backlog_var_bridge/misc_b 等。
- `tests/fpm/` — **真实游戏回归测试**（判据：需要 `OA_TEST_FPM_PFS`（Madosoft《ハミダシクリエイティブ》中文版）或
  `OA_TEST_NEKOMIKO_PFS`（NekoMiko，U6b）指向真实 Artemis 包 / 只对该游戏
  有效；缺包时自跳 exit 77）：
  folder_start/runtime_frames/boot_fpm/pfs_real/title_drain/p3b_body_probe/
  text_layout_probe/ux_journey/ux_flow_controls/
  qload_dock/save_load_ui/backlog_saveload/char_visual_probe/p1_chain_skip/
  p3_select/u13_wheel/r10*/r38*/nekomiko_p0/
  emote_static/emote_chain（P1，research/40-emote：PSB 统计 parity + 静态姿态光栅 +
  真实 Lua 链 e:createEmoteLayer → 层绑定验收）/
  emote_playback（P2/P3，research/40-emote：timeline/变量播放 + bezier 网格形变
  状态机，呼吸/表情/口型帧差）/
  font_face（P4U，research/40-emote：真实项目默认字形面解析——NekoMiko 自带 font/
  回退；引擎默认字体为 FPM 资产）
  （CMake：`add_subdirectory(fpm)`）。
- 全量：`cd build/default && ctest`（自动探测 OA_TEST_FPM_PFS / OA_TEST_NEKOMIKO_PFS，
  见下）。

---

## 3. 环境变量总表

### A. 运行时宿主（用户版 openartemis 即生效）

| 变量 | 作用 | 所属模块 | 示例 |
|---|---|---|---|
| `OA_PFS` | 无位置参数时的默认项目（数据源选择不再有本机硬编码默认；两者皆无则 usage 报错退出） | src/app/main.cpp | `OA_PFS=/x/root.pfs ./openartemis --frames 900` |
| `OA_SAVE_ROOT` | 覆盖存档根目录（默认=平台默认：本机(desktop)=游戏数据所在目录；android=app 私有目录、wasm=IDBFS /save，经 oa::plat::default_save_root，research/79）。测试/autodrive 一律显式指向 /tmp（autodrive 未显式给根时兜底 OS 临时目录 oa_autodrive_save） | src/app/main.cpp（host_save_root → oa::plat，platform/Platform.h） | `OA_SAVE_ROOT=/tmp/s1 ./openartemis a.pfs` |
| `OA_DUMP_FRAME` | `--dump` 的别名，路径取 OS 临时目录（$TMPDIR/%TEMP%）下 oa_frame.ppm（窗口有限运行结束写 PPM；不再固定 /tmp） | src/app/main.cpp | `OA_DUMP_FRAME=1 ./openartemis --frames 400 a.pfs` |

### B. 回归门控（ctest）

| 变量 | 作用 | 所属模块 | 示例 |
|---|---|---|---|
| `OA_TEST_FPM_PFS` | 指向真实 Artemis 包的路径；tests/fpm/ 全部测试 + 部分通用测试靠它工作。无本机自动探测：configure 时 `-D OA_TEST_FPM_PFS=<path>` 或环境变量 `OA_TEST_FPM_PFS` 提供（cache 优先于 env）；都无 = 空，相关测试 exit 77 自跳。**该包 = Madosoft《ハミダシクリエイティブ》(中文补丁版)**——判定/准备测试资产以该游戏为准；CMake 缓存变量 | 顶层 CMakeLists → tests{,/fpm}/CMakeLists | `cd build && OA_TEST_FPM_PFS=/x/root.pfs cmake -B . && ctest` |
| `OA_TEST_NEKOMIKO_PFS` | 指向 NekoMiko root.pfs（U6b 资产；同 FPM 规则：-D 或 configure 时环境变量提供，无本机自动探测；CMake 缓存变量）。`nekomiko_p0`/`emote_static`/`emote_chain`/`emote_playback` 测试靠它工作 | 顶层 CMakeLists → tests/fpm/CMakeLists | `OA_TEST_NEKOMIKO_PFS=/x/root.pfs cmake -B build/default && ./build/default/tests/fpm/emote_chain_test` |

### C. 测试二进制（openartemis_test）专用

| 变量 | 作用 | 所属模块 | 示例 |
|---|---|---|---|
| `OA_AUTODRIVE` | 窗口化确定性旅程流：`exit`（退出像素流）`title`（回标题像素流）`help`（dock help 悬停探针）`conf`（config 页 hover/拖动探针，VERDICT ALIVE）`r10save`（save 屏 help 位置压力，800 交替）`r10t`（标题往返 2 轮）`r10blog`（backlog 翻页）`qld`（现象 C quickload dock 验证）`d38`（R38 dock 三问题探针）`scale`（U25/research/60 展示层梯子：固定离屏 stage 回读 + letterbox 呈现 + 真实 SDL 事件映射验证）`bt119cg`（research/119 btjy extra CG 鉴赏差分切换：标题语言选择→标题→Lua 桥强制解锁鉴赏→点 bt_extra→点缩略图→鉴赏器内连续点击并逐次 dump Lua 状态/600&700 层树/整帧哈希；旋钮 `OA_BT119_CG`（默认 cg03=ev_com_01，7 变体）`OA_BT119_CLICKS`（默认 6）`OA_BT119_AT`（中性点，默认 1310,880）`OA_BT119_THUMB=1`（首击落缩略图中心）`OA_BT119_FAKEKEYCODE=1`（A/B：点击前重写 flg.keycode））`fontscan`（research/120 现象 J 跨作正文边缘普查：可选首启语言页 → 标题 bt_start → 首个停驻的剧情页，dump 每个可见文本层的字表 raw + 冻结帧 PNG；整程与项目无关，任何 iMel 系包可直接跑；旋钮 `OA_FONTSCAN_PAGES=N`（默认 0，先点过 N 页再冻结，用来找亮背景页））`kdemote`（research/127 KukkoroDays 首启语言页 → 品牌 logo → 标题 bt_start → gamestart 名字对话（逻辑补全）+ Yes/No 选择 → 逐页走 00_共通1 到第一个真实 E-mote `[fg]` 场景（第 87 页，`:fg/cat/tca_a00.psb`）：到达后停驻并 dump 层清单/合成变量样本/CPU 姿态画布（不透明像素+bbox）与窗口 PNG，中段自动翻一页（首个 fg 带 `pass=1` → 框架 `skip()` 双槽结束是设计行为）以采「前景表情 + 通常待機 idle 真的在播」的姿态 revision 增长证据；旋钮 `OA_KDE_PAGES`（默认 400，推进点击上限）`OA_KDE_HOLD`（默认 240，emote 停驻帧数）`OA_KDE_GAP`（默认 10，两次推进点击间隔帧））`langidle`（research/131 首启语言页**停驻**旅程：等语言页出现后**不点**，纯待机 `OA_LANGIDLE_FRAMES` 帧并对每 `OA_LANGIDLE_BEAT` 帧打一条有界心跳（帧号/墙钟/最差单帧墙钟/wait/层数/处理器层数/活动 tween/transition/整帧 luma+hash/场景·文本·tween 事件计数），待机结束再点该语言行并判定"页面是否真的离开"（= 停驻后还点得动），随后标题 `bt_start` → 首个停驻剧情页；退出码 1 + `VERDICT FAIL` 表示"停驻后点击无效/行消失"。旋钮 `OA_LANGIDLE_FRAMES`（待机帧数，默认 1800）`OA_LANGIDLE_BEAT`（心跳间隔帧，默认 60）`OA_LANGIDLE_WALK`（剧情推进点击上限，默认 200）`OA_LANGIDLE_HOVER=1`（整个待机期把合成指针停在语言行中心 —— 真机"鼠标停在页面上"的情形，也是本缺陷的必要条件））`slnywalk`（research/132 きら☆かの slny：首启语言页 → 品牌 logo → 标题 `bt_start` → **逐句推进剧情**，每停驻页对每个活着的 E-mote 层打一份**部件级**指纹：`collect_pose_parts` 的逐部件画布 bbox + 全姿态顶点 hash、同运行 CPU 姿态画布的轮廓指标（不透明像素/alpha bbox/8 连通域数/图内最大空行带）、以及合成变量域全量 + 前景槽列表(含时钟) + idle 槽 + rev；跑满 `OA_SLW_PAGES` 后打印**页表 / 变量漂移表（第 1 页 vs 各页逐 label）/ 部件漂移表（部件中心相对该页中位部件中心的位移，"哪个部件相对整体偏了多少 px、在第几页"）**，并逐页落 `slnywalk_sNN_canvas_*.png`（CPU 画布 1/2）与 `slnywalk_sNN_stage.png`（整帧）。旋钮 `OA_SLW_PAGES`（默认 24，指纹页上限）`OA_SLW_SETTLE`（默认 20，停驻后静止帧数再打"已停驻"指纹）`OA_SLW_EARLY`（默认 2，每句的**早期**指纹帧号——采表情入场/手势在飞的姿态）`OA_SLW_GAP`（默认 20，指纹后两次推进点击的间隔帧）`OA_SLW_CANVAS=0`（不打 PNG）`OA_SLW_BOOTCAP`（默认 40000，引导/未停驻的帧上限）） | src/app/main.cpp（OA_TEST_BUILD 段） | `OA_AUTODRIVE=title ./openartemis_test a.pfs` |
| `OA_WIN_W` / `OA_WIN_H` | 覆盖窗口创建尺寸（任意像素/比例；内容仍按项目 stage 逻辑分辨率离屏渲染并 letterbox 呈现——任意窗口尺寸的回读/呈现验证用，research/60）。缺省 = 项目 stage 尺寸 | src/app/main.cpp（OA_TEST_BUILD 段） | `OA_WIN_W=1024 OA_WIN_H=768 ./openartemis_test a.pfs` |
| `OA_SCALE_SIZES` | `scale` 流的窗口梯子（逗号分隔 `WxH`；缺省 `1600x900,1024x768,720x1280`；需要可见窗口 `OA_AD_VISIBLE=1`） | 同上 | `OA_SCALE_SIZES=1920x1080,1024x768 OA_AD_VISIBLE=1 OA_AUTODRIVE=scale ./openartemis_test a.pfs` |
| `OA_UI_OUT` | autodrive 流的证据输出目录（PNG/PPM/日志画面），自动创建 | 同上 | `OA_UI_OUT=/tmp/ev ./openartemis_test` |
| `OA_R10_ALTER` | r10/r10save 交替次数（默认 800） | 同上 | `OA_R10_ALTER=100` |
| `OA_R10_DWELL` | r10 每次悬停停留帧数（默认 1） | 同上 | `OA_R10_DWELL=3` |
| `OA_R10_SEED` | r10 伪随机种子（默认 7） | 同上 | `OA_R10_SEED=1` |
| `OA_R10_PNG` | r10 周期/异常帧 PNG（默认 1） | 同上 | `OA_R10_PNG=0` |
| `OA_R10_PAIR` | r10 限定压力对 `"keyA,keyB"` | 同上 | `OA_R10_PAIR=bt_save,bt_load` |
| `OA_R10_TRACE` | r10 逐帧 T 采样条数（默认 0） | 同上 | `OA_R10_TRACE=120` |
| `OA_TXT_SMOKE` | 窗口有限运行中驱动 bt_start → 剧情文本并周期打印文本状态 | 同上 | `OA_TXT_SMOKE=1 ./openartemis_test --frames 4000 a.pfs` |
| `OA_P1B_DEBUG` | 标题停驻探针日记（每 30 帧 hover/wait/trans/layers） | 同上 | `OA_P1B_DEBUG=1` |
| `OA_STATE_LOG` | 每 120 帧状态心跳（wait/layers/glyphs；外部 Xvfb 驱动用） | 同上 | `OA_STATE_LOG=1` |
| `OA_GLYPH_LOG` | 每 20 帧打印绘制字形数 | 同上 | `OA_GLYPH_LOG=1` |
| `OA_VIDEO_DEMO` | 引导后自动全屏播放指定逻辑视频文件（U9 宿主演示，EOF 退出） | 同上 | `OA_VIDEO_DEMO=:movie/x.ogg` |
| `OA_VIDEO_DEBUG` | 视频解码/上传诊断打印（main+core media/renderer 各处读取） | src/app/main.cpp、src/core/media/video.cpp、src/core/render/renderer.cpp | `OA_VIDEO_DEBUG=1` |

### D. 引擎诊断（core 内 env 门控；两份二进制都编译在内，**不设即零开销零输出**）

| 变量 | 作用 | 所属模块 | 示例 |
|---|---|---|---|
| `OA_DEBUG_VAR` | e:var 度量/同步变量解析轨迹 | src/core/runtime/runtime_iet.cpp、src/core/render/renderer.cpp | `OA_DEBUG_VAR=1 ./openartemis_test` |
| `OA_DEBUG_FONT` | FreeType 字体/字形度量诊断 | src/core/render/font.cpp、text.cpp | `OA_DEBUG_FONT=1` |
| `OA_FONT_OUTLINE` | 文字描边编码（research/126）：`stroke`（默认，FT_Stroker 连续描边层）`dilate`（圆盘膨胀退路）`legacy`（前 120 的 4 对角副本，仅 PRE 像素对照/回退用） | src/core/render/font.cpp | `OA_FONT_OUTLINE=legacy ./openartemis_test` |
| `OA_DEBUG_LYTW` | [lytween]/[lytweendel] 请求流（id/param/from/to/time） | src/core/runtime/runtime.cpp | `OA_DEBUG_LYTW=1` |
| `OA_DEBUG_HOVQ` | 指针 hover 命中序/hovered 集合逐帧轨迹（dock 区域） | src/core/runtime/runtime.cpp | `OA_DEBUG_HOVQ=1` |
| `OA_DEBUG_RESET` | Lua e:tag{"reset"} 来源栈打印 | src/core/runtime/runtime_iet.cpp | `OA_DEBUG_RESET=1` |
| `OA_DEBUG_TRANSQ` | Lua e:tag{"trans"} 来源 Lua 栈打印 | src/core/runtime/runtime_iet.cpp | `OA_DEBUG_TRANSQ=1` |
| `OA_LUA_DEBUG` | Lua 运行/调用诊断（run_code/错误上下文） | src/core/runtime/runtime_lua.cpp | `OA_LUA_DEBUG=1` |
| `OA_LUA_STRICT` | **引擎→游戏 Lua 的错误策略**（research/130）：默认"记日志后继续"（游戏 Lua 抛错只放弃该次调用，不退出进程 ，见 `LuaBridge::report_dispatch_error`）；`=1` 恢复 130 之前的 fail-fast（错误照旧走 `[app] tick error` + 退出）。排障/回归想抓"nil 崩溃面"时建议显式打开 | src/core/runtime/runtime_lua.cpp | `OA_LUA_STRICT=1 OA_AUTODRIVE=kdemote ./openartemis_test a.pfs` |
| `OA_LUA_STDLIB` | **标准库屏蔽清单的 A/B 臂**（research/134）：缺省=屏蔽生效 —— `os.date`/`os.clock`/`os.time`/`os.getenv`、`io.open`/`io.close` 及句柄读写保留可用，`os.execute`/`os.system`/`os.exit`/`os.remove`/`os.rename`/`os.setlocale`/`os.tmpname`/`io.popen`/`io.tmpfile`/`io.input`/`io.output`、`dofile`/`loadfile`/`loadstring`/`load`/`package.loadlib` 被移除；`=stock` 交回**未屏蔽**的标准库面 | src/core/runtime/runtime_lua.cpp | `OA_LUA_STDLIB=stock` |
| `OA_ANDROID_FULLSCREEN` | **Android 沉浸式全屏开关**（缺省 1=开）：Android 上真正进全屏的只有 `SDL_SetWindowFullscreen`（建窗的 `SDL_WINDOW_FULLSCREEN` flag 在 Android 后端**不生效**，`Android_CreateWindow` 不碰窗口样式）。`=0` 完全不进全屏，用于把"黑屏"问题从全屏这条链上摘出去：`OA_ANDROID_FULLSCREEN=0` 还黑，就与全屏无关 | src/app/main.cpp | `OA_ANDROID_FULLSCREEN=0` |
| `OA_NO_VSYNC` | 关闭每帧 Lua `onEnterFrame` 事件（无 vsync 的确定性窗口测试用） | src/core/runtime/runtime.cpp | `OA_NO_VSYNC=1` |
| `OA_TRANSDBG` | [trans] 捕获路径打点：每次捕获打印 GPU 拷贝（旧场景来自离屏 stage target，零 ReadPixels） | src/app/main.cpp、src/core/render/renderer.cpp | `OA_TRANSDBG=1 OA_AUTODRIVE=exit ./openartemis_test a.pfs` |
| `OA_SELTRACE` | select_exit 链入口引擎/脚本状态探针（研究/53 E15：482 二路径用户复现用；runtime/interpreter/lua 各处） | src/core/runtime/runtime.cpp、src/core/runtime/runtime_iet.cpp、src/core/runtime/runtime_lua.cpp | `OA_SELTRACE=1 ./openartemis_test a.pfs` |
| `OA_EXSKIPDBG` | e:debugSkip 快进起始/边界释放打点（研究/56 §5 复现） | src/core/runtime/runtime.cpp | `OA_EXSKIPDBG=1` |
| `OA_DEBUG_ICON` | emote 形变链图标 warp 起点打点（研究/48 P4U3 探针） | src/core/emote/emote_render.cpp | `OA_DEBUG_ICON=<icon path> OA_DEBUG_EVAL=1` |
| `OA_DEBUG_EVAL` | emote 逐节点帧选择/世界包围盒/条目统计（研究/48 P4U3 探针；=debugPath 填充开关） | src/core/emote/emote_render.cpp | `OA_DEBUG_EVAL=1` |

### E. 测试内部

| 变量 | 作用 | 所属模块 | 示例 |
|---|---|---|---|
| `OA_UX_TRACE` | ux_journey_test 内部步骤轨迹 | tests/fpm/ux_journey_test.cpp | `OA_UX_TRACE=1` |
| `OA_DIRTY_AB` | layer-model S2 统一 invalidate 簿记的同进程 A/B（research/105）：每帧按"现状判据"与"簿记集合"各产出去重集合并逐帧比对；=1 只报（失配帧 + 退出汇总），=2 在首个会翻转消费者等价布尔（重绘门/烘焙失效）的帧 exit(2)。渲染门不受影响（双轨并存,输出未接） | src/app/main.cpp | `OA_DIRTY_AB=1 OA_AUTODRIVE=title ./openartemis_test a.pfs` |
| `OA_P2DEEP` | nekomiko_p0 深跑：翻页推进到真实 fg 立绘场景并测 idle 呼吸 rev 增长 | tests/fpm/nekomiko_p0_test.cpp | `OA_P2DEEP=1` |
| `OA_P2TRACE` | nekomiko_p0 翻页步进/等待轨迹（诊断） | tests/fpm/nekomiko_p0_test.cpp | `OA_P2TRACE=1` |
| `OA_EMOTE_FPS` | EmotePlayer 姿态重渲染帧率覆盖：CPU 光栅路径节流（<8fps 用 1000/v，≥8fps 125ms 保底）；GPU 合成路径（external-pose）默认 30fps、该变量可到 60fps（research/40-emote） | src/core/emote/emote_player.cpp | `OA_EMOTE_FPS=60` |
| `OA_AD_VISIBLE` | autodrive 可见窗口（缺省 hidden；像素自动化的 WM-less 注意见 research/40-emote） | src/app/main.cpp | `OA_AD_VISIBLE=1` |
| `OA_AD_MOUSE_ASSERT` | exit/title autodrive 旅程在确认框落定后硬断言鼠标自动落点：warp 请求 ≥10 且引擎指针落在最后一次 `[mouse]` 请求目标（±1px；隐藏窗口/headless 由运行时仿真推进指针）。注：nene 退出确认框存在脚本 csv 目标与引擎 clip 命中区的既有几何偏差（≈+270,+277），见 research/57 | src/app/main.cpp | `OA_AD_MOUSE_ASSERT=1 OA_AUTODRIVE=exit ./openartemis_test a.pfs` |
| `OA_NM_DIAG` | nmfg 旅程诊断：窗口 PNG + 消息层几何/字形计数 | src/app/main.cpp | `OA_NM_DIAG=1` |
| `OA_NM_ASSERT` | nmfg 旅程窗口像素文本断言（字形>0 且消息窗区暗像素≥800）+ 立绘断言（P4U2/P4U3 重校，research/40-emote：隐藏-立绘 diff rows60-900>15万 / feet>4万 / rows0-50<1.5万 且头顶≥30 行 / 呼吸窗 diff>4万；hidden 窗口强制，失败 exit 1） | src/app/main.cpp | `OA_NM_ASSERT=1` |
| `OA_NM_SELTEST` | nmfg 旅程 stage-3 续进到 fg 立绘场景（研究/49 双击崩溃深跑；OA_NM_SELGAP 设点击间隔帧） | src/app/main.cpp | `OA_NM_SELTEST=1` |
| `OA_NM_SELGAP` | nmfg stage-3 选择支点击间隔帧（默认 ~6 = ~200ms；研究/49） | src/app/main.cpp | `OA_NM_SELGAP=3` |
| `OA_NM_SELDBG` | 选择支 select_* Lua 调用/引擎点击链诊断（研究/49） | src/core/runtime/runtime_lua.cpp、src/core/runtime/runtime.cpp | `OA_NM_SELDBG=1` |
| `OA_EMOTE_HALFRES` | EmotePlayer 半分辨率回退（P4U3/research/40-emote 起默认全分辨率；弱机/调试用） | src/core/emote/emote_player.cpp | `OA_EMOTE_HALFRES=1` |
| `OA_EMOTE_FULLRES` | 显式强制 1:1（现为默认；保留兼容） | src/core/emote/emote_player.cpp | `OA_EMOTE_FULLRES=1` |
| `OA_EMOTE_THREADS` | 全分辨率 CPU 光栅行带并行线程数与图集解码线程数（默认 min(硬件并发,16)） | src/core/emote/emote_render.cpp、emote_file.cpp、psb_reader.cpp | `OA_EMOTE_THREADS=4` |
| `OA_EMOTE_GPU` | GPU 贴图合成开关（默认开：SDL_RenderGeometry 画离屏画布；=0 回退 CPU 光栅+上传，research/40-emote）。开启且渲染器创建成功时，宿主锁存 `emote_set_default_external_pose(true)`——新加载的 EmotePlayer 跳过 load() 内的整幅 CPU 初始姿态光栅（大 PSB 曾一次性付出 40+ms），首个泵由宿主按几何合成 | src/app/main.cpp、src/core/emote/emote_player.cpp | `OA_EMOTE_GPU=0` |
| `OA_EMOTE_DEBUG` | emote 层创建/方法/渲染诊断 | src/core/runtime/runtime_media.cpp | `OA_EMOTE_DEBUG=1` |
| `OA_EMOTE_ATLAS_KEY` | E-mote 图集纹理缓存键（research/132）：默认把 **PSB 身份**（`EmoteFile::uid()`）并入键 `{层画布键, source, uid}`，并在每层下次绘制时销毁属于**另一份 PSB** 的陈旧图集纹理。`=legacy` 回到修复前的"仅按层键"缓存（A/B 对照臂；游戏在同一层 id 下换 `file=` 时 GPU 会用上一份 PSB 的图集按新图的图标矩形采样 ⇒ 立绘"零件错乱"） | src/core/render/renderer.{h,cpp} | `OA_EMOTE_ATLAS_KEY=legacy OA_EMOTE_DEBUG=1 ./openartemis_test a.pfs` |
| `OA_WAITDBG` | 运行时等待解析/揭示阻塞诊断 | src/core/runtime/runtime.cpp | `OA_WAITDBG=1` |
| `OA_PERF_PSB` / `OA_PERF_GPU` / `OA_PERF_DUMP` | 无头 emote 性能探针（report-only，非 ctest）：`emote_perf_probe <root.pfs> [psb子串]` 经游戏自身 Lua 桥注入 `e:createEmoteLayer`，播放该 PSB 自己的 idle/表情时间线，测运行时 tick / advance_ms / collect_pose_parts / CPU 光栅 / 加载分解（psb 解析·图集解码·初始姿态）。`OA_PERF_GPU=1` 另建隐藏窗口测全帧路径（泵+合成+场景绘制+present）；`OA_PERF_DUMP=<目录>` 把当前姿态画布落 PPM（像素对比基线用）；`OA_PERF_STEADY`/`OA_PERF_GPU_FRAMES` 调采样帧数 | tests/fpm/emote_perf_probe.cpp | `emote_perf_probe game/root.pfs jsa_6` |
| `OA_VPROBE_FRAMES` | 无头视频性能探针（report-only，非 ctest）：`video_perf_probe <root.pfs> <视频名子串>` 分级实测 主流解码+YCbCr→RGBA / `_m` 蒙版流解码+alpha 烘焙 / 引擎频道实时循环（decode pool + 驱动节拍，报告达成 fps vs 片源 fps）。mp4/wmv 走 ffmpeg 后端仅测引擎循环。蒙版合成已由驱动线程移入解码池 worker（锁相烘焙；worker 退出交还解码器位，同步回退零追赶），驱动线程每 tick 只剩 memcpy | tests/fpm/video_perf_probe.cpp | `video_perf_probe game/root.pfs sakura` |
| `OA_DRAINSKIP` | 停驻期标签队列排水诊断（research/119）：排队来源的**点击型等待**扣留队列时打印 `[drainskip] hold kind=… queued=…`（含队首标签样本），队列排水命中暂停时打印 `paused remain=…` | src/core/runtime/runtime.cpp | `OA_DRAINSKIP=1 OA_AUTODRIVE=bt119cg ./openartemis_test a.pfs` |
| `OA_SAVE_ROOT` | （同 A）部分 tests/fpm 测试运行时也显式设置以隔离存档 | tests/fpm/* | — |

> 注意：`tests/fpm/` 之外还有少量通用测试的 CMake 条目带着
> `OA_TEST_FPM_PFS` 环境属性（asb/transition/input_dispatch/render2/text_*/
> media_runtime/save_domain/save_thumb/p1c2/misc_b/ini 等）——属历史遗留的
> 无害环境注入（测试代码不读取它），保持原样以零改动。

---

## 4. 快速验证

```sh
cmake -B build/default -DCMAKE_BUILD_TYPE=Debug   # 或 Release
cmake --build build/default -j8
cd build/default && ctest                            # 61 项（含 tests/fpm；真包测试需按上文准备）
# 用户版冒烟（headless 900 基线）
OA_SAVE_ROOT=/tmp/s ./src/app/openartemis --headless --frames 900 /path/root.pfs
# 窗口化回归流（测试版）
Xvfb :97 &  # 无显示环境时
OA_SAVE_ROOT=/tmp/s OA_AUTODRIVE=title ./src/app/openartemis_test /path/root.pfs
# P3 NekoMiko 窗口证据（翻页 → fg 立绘场景 → 呼吸像素捕获，research/40-emote）
DISPLAY=:97 OA_SAVE_ROOT=/tmp/nm OA_AUTODRIVE=nmfg OA_UI_OUT=/tmp/nm_out \
  ./src/app/openartemis_test /path/to/NekoMiko/root.pfs
# P4U2 立绘窗口回归（文本+立绘区像素断言，research/40-emote；失败 exit 1）
DISPLAY=:97 OA_SAVE_ROOT=/tmp/nm OA_AUTODRIVE=nmfg OA_NM_ASSERT=1 \
  OA_UI_OUT=/tmp/nm_out ./src/app/openartemis_test /path/to/NekoMiko/root.pfs
```
