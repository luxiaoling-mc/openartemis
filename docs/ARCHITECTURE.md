# ARCHITECTURE — openartemis 整体架构

> 本文是**工程整体结构**的权威入口：分层、数据流、模块职责、宿主接线、
> 平台外壳、构建与测试布局。**描述的是"现在长什么样"**，不是路线图。
>
> 读代码时的入口顺序建议：`README.md`（构建与运行）→ 本文（结构）→
> `docs/TESTING.md`（`OA_*` 环境变量与测试布局）→ `docs/PLATFORMS.md`
> （Android / wasm 打包）。

## 1. 它是什么

`openartemis` 是 **Artemis 系视觉小说引擎**的独立 C++20 运行时：直接读取
Artemis 系游戏的数据包（`root.pfs`，可带分卷）并**按真机表现**复刻引擎行为。
仓库不含任何游戏资产；真包回归由 `OA_TEST_*_PFS` 指向本机游戏包。

**开发基线 = 真实运行验证**。正确性判定不靠"看起来对"，而靠：真实包上的
旅程复现 + 像素/结构证据 + 与参照资料（krkrsdl3、E-mote SDK 手册）的逐项对照。
凡是手册/参照没给依据的语义，**不猜不改**，登记为待裁定。

## 2. 分层总览

```
┌───────────────────────────────────────────────────────────────────────┐
│ 平台外壳（每平台一份宿主入口）                                          │
│   desktop: build/<preset>/src/app/openartemis                          │
│   Android: android/ (Gradle + Java 启动器 + SDLActivity)                │
│   wasm:    web/index.html + web/serve.py                               │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 一律经 src/app/main.cpp（同一份宿主实现）
┌───────────────────────────────▼───────────────────────────────────────┐
│ 宿主层 src/app（SDL 主层）                                             │
│   main.cpp            SDL 初始化/窗口/事件转换/CLI/启动与退出            │
│   platform/           oa::plat：每 OS 一个实现 TU                       │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 只交互：启动 / 事件 / 每帧 / 结束
┌───────────────────────────────▼───────────────────────────────────────┐
│ 引擎内核 src/core                                                      │
│   runtime/runtime.cpp App→GameRuntime；GameRuntime 直接持有 RenderEngine │
│   render/             scene/text/transition + SDL 后端/纹理都在这里       │
│                                                                       │
│   runtime/    oa::runtime：宿主编排 + Artemis 等待状态机 + 域实现         │
│      runtime.h                App 宿主契约（引擎面见下一行）             │
│      runtime_game.h           GameRuntime 引擎面（探针/测试 include 它） │
│      runtime_internal.h       实现面 RuntimeState（三桶维护契约）        │
│      runtime_iet.{h,cpp}      值/表达式、.iet/.ast 解析、.asb 解码、      │
│                               解释器（原 oa::script，六 TU 合并为单 TU）  │
│      runtime_lua.{h,cpp}      Lua 宿主面（原 oa::lua；桥对象 + 注册段）   │
│      runtime_media.cpp        媒体域实现（视频/emote 派发）             │
│      runtime_save.{h,cpp}     存档域：SaveStore 接缝 + SaveData 二进制编解码│
│   render/     oa::render：渲染与场景（原 oa::scene / oa::text 已并入）    │
│      backend.h                **渲染后端唯一接缝**（不含 SDL 类型）      │
│      backend_sdl.cpp          SDL_Render 实现                          │
│      backend_gles.{h,cpp}     原生 GLES3 实现（第二条后端）              │
│      renderer.{h,cpp}         RenderEngine：**只栅格化帧计划** + 合成 + 纹理缓存   │
│      layer.{h,cpp}            层模型（属性/子树/事件行/烘焙）            │
│      （内容读取面并入 layer.h：textured_content / content_texture_key /
│       content_quad 直接谓词，无角色类、无分类枚举）                        │
│      texture_key.h            纹理键 {域,名}（按域分桶，撞名不可能）      │
│      text.{h,cpp} font.{h,cpp}  文本域：消息层/表意/字形与描边           │
│   media/      oa::media：音频/视频/解码池/图像                            │
│      audio.{h,cpp}            音频引擎 + 播放器 + vorbis（合并 TU）      │
│      video.{h,cpp}            视频：theora + 可选 ffmpeg（合并 TU）      │
│      decode_pool.{h,cpp} image.{h,cpp} media_internal.h                 │
│   emote/      oa::emote：E-mote 立绘（原 oa::media::emote / ::psb 并入）  │
│      psb_reader.{h,cpp}       PSB v4 容器（含加密 PSB 的 seed 解密）     │
│      emote_file.{h,cpp}       E-mote 语义模型 + 图集（DXT5/BC7/RGBA8）   │
│      emote_player.{h,cpp}     播放器：时间轴/变量域/槽混合               │
│      emote_render.cpp         姿态求值（CPU 光栅 + GPU 网格收集）        │
│      bc7decomp.{h,cpp}        vendored BC7 软解（第三方，见 PROVENANCE） │
│   fs/         oa::fs：虚拟文件系统 + 工程配置（原 oa::config/project 并入）│
│      fs.h                     **IFileSystem 唯一接缝**                  │
│      physfs_fs.{h,cpp}        PhysicsFS 实现 + 目录层叠加 + 可写挂载     │
│      project.{h,cpp}          system.ini → ProjectConfig（容错 INI）     │
│   util/       oa::util：binary_stream / charset / sha1（同一命名空间）    │
└───────────────────────────────┬───────────────────────────────────────┘
┌───────────────────────────────▼───────────────────────────────────────┐
│ 依赖与第三方                                                           │
│   vcpkg: SDL3 freetype libpng libjpeg-turbo libvorbis libtheora physfs  │
│           （可选 ffmpeg 6.1.1：桌面/Android 有，wasm 自动剔除）          │
│   vendored: third_party/lua-5.1.5（Lua 5.1.5，仅 lgc.c 一处本地补丁）    │
│             third_party/bc7decomp（BC7 软解）                           │
└───────────────────────────────────────────────────────────────────────┘
```

## 3. 数据流

### 3.1 一帧（宿主 → 解释器 → 场景 → 像素）

```
SDL 事件 ──► AppState 输入 ──► GameRuntime::tick
                                  │
                   ┌──────────────┼───────────────────────────┐
                   ▼              ▼                           ▼
            等待状态机      解释器 next_line()            媒体推进
            （wait/drain）  .iet/.ast → Event/Tag          （音频/视频/emote）
                   │              │                           │
                   │              ▼                           ▼
                   │        Lua 宿主面回调            音频流 / 解码帧 / 姿态画布
                   │        （tag/event filter、            │
                   │          calllua、onEnterFrame）        │
                   └──────────────┴───────────┬───────────────┘
                                              ▼
                                   场景树 Layer（属性/子树/事件行）
                                              ▼
                                   RenderEngine 绘制 + 合成
                                              ▼
                                     render backend（sdl 或 gles）
                                              ▼
                                      窗口 / 离屏 target
```

### 3.2 资源装载（同一个 VFS）

```
可执行参数（或 OA_PFS）
   │
   ├─ 指向 root.pfs ──► PhysFileSystem：PhysicsFS 挂载归档（含分卷 .000/.001…）
   │                         └─ 旁挂"同名目录层"（散装 movie/ 等覆盖包内）
   └─ 指向含 system.ini 的目录 ──► DirFileSystem（解包工程 / 文件夹启动）
                                   │
                                   ▼
                     IFileSystem（虚拟路径，'/' 分隔，大小写不敏感）
                                   │
                ┌──────────────────┼───────────────────┐
                ▼                  ▼                   ▼
          ProjectConfig        脚本/图像/音频        存档（WritableMount）
          (system.ini)         按虚拟路径读取        独立根：save_root（主机策略）
```

**规则**：引擎只认 `IFileSystem`；**目录层优先于包**（patch/覆盖语义）。

## 4. 核心不变量（改代码前必读）

| 不变量 | 载体 | 说明 |
|---|---|---|
| **后端唯一接缝** | `render/backend.h` | 头里**不含**任何 `SDL_*` 渲染类型；后端原语 = 引擎真实调用面的最小投影 |
| **内容读取面** | `render/layer.h` | `textured_content` / `content_texture_key` / `content_quad` 直接谓词（无角色类）；宿主供帧键名 = 层 id |
| **渲染流水线单向** | `render/renderer.h`（RenderPlan 段） | 生产者写层 → 计划求值 → 栅格化；renderer 不遍历层树、不读 props、不决定顺序（细节 `docs/PIPELINE.md`） |
| **纹理按域分桶** | `render/texture_key.h` | `TextureKey{域,名}`，跨域撞名在结构上不可能 |
| **文件系统唯一接缝** | `fs/fs.h` | 引擎只谈虚拟路径；存档是同一 PhysicsFS 会话上的可写挂载 |
| **运行时状态三桶** | `docs/RUNTIME_STATE.md` | `[A] Serializable` / `[B] RebuiltOnLoad` / `[C] Ephemeral`，每字段一个恢复责任人。A 面只放纯数据，禁止句柄/指针/资源 |
| **读档顺序契约** | 同上 | B-preserve 回写必须在场景快照恢复**之后**；load 顺序两处为既有实现依赖 |
| **Lua 面** | `runtime/runtime_lua.{h,cpp}` | Lua 5.1 语义精确对齐；标准库做**屏蔽清单**（保留 `os.date`/`io.open`/`io.close` 等，移除 `os.execute`/`os.system`/`os.exit`/`io.popen`/`loadstring`/`package.loadlib` …） |
| **引擎→游戏 Lua 错误策略** | `LuaBridge::report_dispatch_error` | 默认"记日志后继续"；`OA_LUA_STRICT=1` 恢复 fail-fast |

## 5. 宿主接线（`src/app`）

**main.cpp 只做 SDL 层**：命令行解析、SDL 初始化/窗口、SDL 事件转换为
引擎输入（按键 vk / 鼠标 / 滚轮 / 触屏）、启动 `oa::runtime::App`、每帧调用
`App::tick()`、退出清理。它不包含渲染或运行时逻辑。

**内核 `oa::runtime::App`**（`src/core/runtime/runtime.cpp`）：只拥有一个
`GameRuntime`，负责打开项目、输入转发与每帧推进。`GameRuntime` 直接持有
`RenderEngine`；scene/text/transition/绘制状态归 render，runtime 在标签派发和
`tick` 内完成同步并调用 `renderer.render_frame()`。

**公开面分两级**：`runtime.h` 只给宿主（`main.cpp`）——`AppConfig`/`AppResult`/
`App`，不含 SDL 类型也不含引擎类型；`runtime_game.h` 是引擎面 `GameRuntime`
（域访问 + 探针/诊断计数），测试与兼容适配器才 include 它。

**事件派发**：解释器事件在 `RuntimeState::dispatch_interpreter_event` 一处收口，
固定域序 `engine → media → text → scene → control → host stream`；只有该入口
决定事件是否暂停脚本。宿主流事件再按 `save → control` 顺序在 tick 内派发。

**启动链**：main 解析 argv/OA_* → 挂 `oa::plat` → `App::boot()` 打开项目、
建 `GameRuntime` + `RenderEngine` → main 用 stage 尺寸建 SDL 窗口 →
`App::attach_window(handle)` 创建后端 → 主循环。

**平台层 `oa::plat`**（`src/app/platform/`）：每 OS 一个实现 TU —
`platform_desktop.cpp`（存档根 = 游戏目录）、`platform_android.cpp`
（应用私有存档根 + 生命周期闩锁）、`platform_wasm.cpp`（IDBFS `/save` 挂载 +
页面持久化/可见性钩子）。

## 6. 平台与构建

| 目标 | 预设 | 产物 | 备注 |
|---|---|---|---|
| Linux/Windows 桌面 | `default`（Ninja Release）、`debug`、`asan`、`windows`（MSVC） | `build/<preset>/src/app/openartemis`，测试在 `tests/` | 全功能：工具 + 测试 |
| Android arm64-v8a | `script\build-android.bat`（Gradle + AGP 驱动 CMake） | `android\app\build\outputs\apk\debug\*.apk`，原生侧 `libopenartemis.so` | 只编**用户宿主**；存档根 = 应用私有目录；需 `VCPKG_ROOT`/`ANDROID_NDK_HOME`/`ANDROID_HOME`/JDK17 |
| Android x64（模拟器） | `android-x64` | 同上 | 用于模拟器验证 |
| 浏览器 WebAssembly | `script\build-wasm.bat`（emsdk） | `build/wasm/src/app/index.html|js|wasm` | 真 pthreads（全局 `-pthread` + COOP/COEP）；ffmpeg 剔除 |

**共享约定**

- `src/app` 是 SDL 主层（窗口/事件/平台差异）；SDL 类型不进内核接口，
  只在 `src/core/render` 的后端实现里出现。
- `tools/` 与 `tests/` 只用桌面目标（Android/wasm 顶层 CMake 跳过）。
- 构建选项：`OA_BUILD_TOOLS` / `OA_ENABLE_TESTS` / `OA_ENABLE_ASAN`；
  测试资源门：`OA_TEST_*_PFS`（`-D` 或同名环境变量，缺包自跳 exit 77）。
- Android 图标由 `tools/gen_icon.cs` 生成、`tools/check_icon.cs` 复核，
  见 `docs/PLATFORMS.md`。

## 7. 测试布局

| 目录 | 内容 | 资产需求 |
|---|---|---|
| `tests/` | **通用引擎单测**：自包含 fixture（compositor / layer_semantics / transform / event_tags / ini / dir_fs / pfs / script / interpreter / lua* / variable / asb / input_dispatch / render2 / text_domain / text_reveal / media_state / media_runtime / video_decode / save_domain / save_compat / save_thumb_readback / p1c2_control / backlog_var_bridge / misc_b 等） | 无 |
| `tests/fpm/` | **真实游戏回归**：`OA_TEST_FPM_PFS`（Madosoft《まどそふと》系中文版）或 `OA_TEST_NEKOMIKO_PFS`（NekoMiko）或 rr/HCT/tg3/slny 专项目标；含 emote 三层（static/chain/playback/timeline）、存档读取网（`save_compat`）、UI 旅程（r10*/r38*/qload/backlog/select）等 | 需真包，缺则 77 自跳 |

**资源分层**：重型真包测试默认**串行**（内存安全）。
内存充足的机器可用 `-DOA_TEST_SERIALIZE_HEAVY=OFF` 后 `ctest -jN` 并行。

运行：

```bash
cd build/default && ctest                       # 全量（真包门未配置时相关项自跳）
ctest -R ^emote_                        # 按名筛选
```

`OA_*` 环境变量（运行时宿主 / 回归门 / 测试二进制 / 引擎诊断）**唯一权威表** =
`docs/TESTING.md`；改代码新增 env 必须同步该表。

## 8. 工具与脚本

| 工具 | 形态 | 用途 |
|---|---|---|
| `opfs` | C++ CLI | PFS：info/find/get/dump/extract（`extract` 是"文件夹启动"的搭档） |
| `psb` | C++ CLI | PSB：RAW 容器树 + E-mote 语义模型（info/tree/json） |
| `asb` | C++ CLI | ASB 二进制脚本 |
| `oasave` | C++ CLI | 存档检查/转换 |
| `fontedge` | C++ CLI | 字形描边度量（描边回归用） |
| `pfs.py` / `pfs_subset.py` | Python | 只读 PFS 查看 / 造子集包（与 C++ 工具互验） |
| `mkfixture.py` | Python | 造测试 fixture 包 |
| `tagcensus.py` / `tagaudit.py` / `tagmatrix.py` | Python | 标签面普查/审计/矩阵（数据驱动排查） |
| `cdp_dump.js` | Node | 无头浏览器观测（wasm 线） |
| `gen_icon.cs` / `check_icon.cs` | C# | Android 图标生成与复核 |

C++ 工具**复用引擎自己的读取器**（不在工具里重写格式逻辑），因此工具即
"引擎解析面的第二个消费者"，天然互验。

## 9. 代码组织约定

- **"小接口，大文件"**：公开头只留契约，实现按内聚子系统**合并**成大 TU
  （`runtime_iet.cpp` 合并六个 script TU、`runtime_lua.cpp` 合并九个 lua TU、
  `media/audio.cpp`、`media/video.cpp`、`render`）。
  目标是减少跨 TU 接口面，而不是拆得越碎越好。
- **`renderer.h` 公开面**：按域分节（生命周期/域访问/帧泵/内容与命中/探针段）；
  纹理上传、绘制 helper、回读全部 private。事件打标只经 `note()`，计数只经
  `stats()`/`cache_stats()`；`render_begin/draw_scene/progress_transition/
  render_end` 仅供性能探针，宿主统一走 `render_frame()`。
- **命名空间按目录对齐**（一个目录一个顶层命名空间）：`src/core/fs`→`oa::fs`、
  `emote`→`oa::emote`、`util`→`oa::util`、`render`→`oa::render`、
  `runtime`→`oa::runtime`、`media`→`oa::media`、`src/app/platform`→`oa::plat`；
  `src/core/version.*` 留在顶层 `oa`。合并来源：`config`/`project`→`oa::fs`，
  `media::emote`/`media::psb`→`oa::emote`，`scene`/`text`→`oa::render`，
  `script`/`lua`/`save`→`oa::runtime`，`charset`/`binary`/`sha1`→`oa::util`
  （`fs/store.*` 已并入 `runtime/runtime_save.*`）。
- **第三方**：`third_party/` 尽量保持上游原样；确需补丁必须在此处或紧邻
  README 记录**原因 + 证据**（例：`third_party/lua-5.1.5/README.openartemis.md`
  记录 `lgc.c` 的一处 Android FORTIFY 补丁）。
- 注释用中文/英文混合是本仓库既有风格。**机制性长文、历史考古、真实包证据
  链迁到 `docs/`**（见 `RUNTIME_STATE` / `RUNTIME_NOTES` / `RENDERING` /
  `EMOTE`）；代码里只留 1–3 行不变量、顺序或非直观原因。

## 10. 相关文档

| 文档 | 内容 |
|---|---|
| `README.md` | 构建与运行（含各平台命令） |
| `docs/ARCHITECTURE.md` | 本文：整体结构 |
| `docs/RUNTIME_STATE.md` | RuntimeState 三桶归属、读档原子性与顺序契约 |
| `docs/TESTING.md` | `OA_*` 唯一权威表 + 测试布局与验收流 |
| `docs/PLATFORMS.md` | Android APK / WebAssembly 构建、打包与壳层 |
| `tools/README.md` | 工具清单与用法 |
