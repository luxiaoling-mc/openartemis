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
│   desktop: build/<preset>/src/app/openartemis[_test]                   │
│   Android: android/ (Gradle + Java 启动器 + SDLActivity)                │
│   wasm:    web/index.html + web/serve.py                               │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 一律经 src/app/main.cpp（同一份宿主实现）
┌───────────────────────────────▼───────────────────────────────────────┐
│ 宿主层 src/app                                                        │
│   main.cpp            窗口/输入/主循环/CLI/资源装载（干净用户版）        │
│   main_test.cpp       同一 TU 以 OA_TEST_BUILD=1 再编一次（测试超集版）  │
│   app_test_drive.cpp  测试专用：autodrive 旅程/合成输入/像素度量          │
│   app_host.h          Options + AppState（两宿主共享）                  │
│   platform/           oa::plat：每 OS 一个实现 TU                       │
└───────────────────────────────┬───────────────────────────────────────┘
┌───────────────────────────────▼───────────────────────────────────────┐
│ 引擎核心 src/core（**不含任何窗口依赖**）                                │
│                                                                       │
│   runtime/    oa::runtime：宿主编排 + Artemis 等待状态机 + 域实现         │
│      runtime.{h,cpp}          公开契约 + 薄转发 + 内核/控制域            │
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
│      renderer.{h,cpp}         RenderEngine：场景绘制 + 合成 + 纹理缓存   │
│      layer.{h,cpp}            层模型（属性/子树/事件行/烘焙）            │
│      layer_kind.h             **分类权威** kind_of()（纯函数）          │
│      content_role.h           **层消费面等价委托**（角色=无状态单例）     │
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
          (system.ini)         按虚拟路径读取        独立根：OA_SAVE_ROOT
```

**规则**：引擎只认 `IFileSystem`；**目录层优先于包**（patch/覆盖语义）。

## 4. 核心不变量（改代码前必读）

| 不变量 | 载体 | 说明 |
|---|---|---|
| **分类唯一权威** | `render/layer_kind.h` 的 `kind_of()` | 纯函数；任何判定调整必须同步 `layer_kind` 的断言组与矩阵测试 |
| **后端唯一接缝** | `render/backend.h` | 头里**不含**任何 `SDL_*` 渲染类型；后端原语 = 引擎真实调用面的最小投影 |
| **层消费面等价委托** | `render/content_role.h` | 角色 = 无状态单例，派生态由 `kind_of` 直接投影；节点内不嵌角色指针 |
| **纹理按域分桶** | `render/texture_key.h` | `TextureKey{域,名}`，跨域撞名在结构上不可能 |
| **文件系统唯一接缝** | `fs/fs.h` | 引擎只谈虚拟路径；存档是同一 PhysicsFS 会话上的可写挂载 |
| **运行时状态三桶** | `runtime/runtime_internal.h` | `[A] Serializable` / `[B] RebuiltOnLoad` / `[C] Ephemeral`，每字段一个恢复责任人；缺标注不通过。A 面只放纯数据，禁止句柄/指针/资源 |
| **读档顺序契约** | 同上 | B-preserve 回写必须在场景快照恢复**之后**；load 顺序两处为既有实现依赖 |
| **Lua 面** | `runtime/runtime_lua.{h,cpp}` | Lua 5.1 语义精确对齐；标准库做**屏蔽清单**（保留 `os.date`/`io.open`/`io.close` 等，移除 `os.execute`/`os.system`/`os.exit`/`io.popen`/`loadstring`/`package.loadlib` …） |
| **引擎→游戏 Lua 错误策略** | `LuaBridge::report_dispatch_error` | 默认"记日志后继续"；`OA_LUA_STRICT=1` 恢复 fail-fast |

## 5. 宿主接线（`src/app`）

**两个可执行文件，同一份实现**：

| 程序 | 源 | 内容 | 用途 |
|---|---|---|---|
| `openartemis` | `main.cpp` | 干净用户版：CLI、输入、渲染、状态行 | 真机验证 |
| `openartemis_test` | `main_test.cpp`（=`main.cpp` + `OA_TEST_BUILD=1`）+ `app_test_drive.cpp` | 超集：autodrive 旅程、合成输入、像素度量、诊断 env | AI/回归/验收 |

`OA_TEST_BUILD=1` 只加测试钩子；**两个二进制在测试设施之外行为一致**。
`AppState` / `Options` 由 `app_host.h` 在两宿主间共享（同一二进制各 TU 的
布局必须一致）。

**启动链**：解析 argv/OA_* → 选数据源（pfs 或目录）→ 挂 `IFileSystem`
（+ 目录层）→ `Project::open` → 建 `GameRuntime`（其内建 Lua VM 与解释器）
→ 建 `RenderEngine`（选 sdl/gles 后端）→ 主循环 `tick → 推进媒体 → 绘制 → present`。

**平台层 `oa::plat`**（`src/app/platform/`）：每 OS 一个实现 TU —
`platform_desktop.cpp`（存档根 = 游戏目录）、`platform_android.cpp`
（应用私有存档根 + 生命周期闩锁）、`platform_wasm.cpp`（IDBFS `/save` 挂载 +
页面持久化/可见性钩子）。两个宿主都编进同一份实现。

## 6. 平台与构建

| 目标 | 预设 | 产物 | 备注 |
|---|---|---|---|
| Linux/Windows 桌面 | `default`（Ninja Release）、`debug`、`asan`、`windows`（MSVC） | `build/<preset>/src/app/openartemis[_test]`，测试在 `tests/` | 全功能：工具 + 测试 + autodrive |
| Android arm64-v8a | `script\build-android.bat`（Gradle + AGP 驱动 CMake） | `android\app\build\outputs\apk\debug\*.apk`，原生侧 `libopenartemis.so` | 只编**用户宿主**；存档根 = 应用私有目录；需 `VCPKG_ROOT`/`ANDROID_NDK_HOME`/`ANDROID_HOME`/JDK17 |
| Android x64（模拟器） | `android-x64` | 同上 | 用于模拟器验证 |
| 浏览器 WebAssembly | `script\build-wasm.bat`（emsdk） | `build/wasm/src/app/index.html|js|wasm` | 真 pthreads（全局 `-pthread` + COOP/COEP）；ffmpeg 剔除 |

**共享约定**

- `src/core` 无窗口依赖；`src/app` 是唯一平台相关面。
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
- 注释用中文/英文混合是本仓库既有风格；机制性注释应指向证据
  （文件:行、测试名、旅程名、环境变量）。

## 10. 相关文档

| 文档 | 内容 |
|---|---|
| `README.md` | 构建与运行（含各平台命令） |
| `docs/ARCHITECTURE.md` | 本文：整体结构 |
| `docs/TESTING.md` | `OA_*` 唯一权威表 + 测试布局与验收流 |
| `docs/PLATFORMS.md` | Android APK / WebAssembly 构建、打包与壳层 |
| `tools/README.md` | 工具清单与用法 |
