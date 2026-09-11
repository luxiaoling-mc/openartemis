# openartemis 平台构建说明（桌面 / Android / WebAssembly）

同一份 `src/core` + `src/app/main.cpp` 编到三个平台；差异全部集中在
`src/app/platform/`（`Platform.h` 定义的 `oa::plat` 面，research/74/78/79）与
两个壳目录 `android/`、`web/`。

| 目标 | 入口 | 存档根默认 | 数据源 |
|---|---|---|---|
| 桌面 (linux/windows) | `openartemis`（+ `openartemis_test`） | 游戏数据所在目录 | 命令行/`OA_PFS` |
| Android (arm64-v8a) | `libopenartemis.so` + `GameActivity` | 应用私有目录（`SDL_GetAndroidInternalStoragePath`） | 启动器 argv |
| 浏览器 (wasm32) | `index.html` + `index.js/.wasm` | IDBFS `/save`（IndexedDB） | 页面参数/文件选择 |

平台默认段（`--platform`）：构建自带 —— 桌面 `windows`、APK `android`、
浏览器 `wasm`（`src/app/app_host.h` 按目标预处理宏取值），可在命令行覆盖。

---

## 1. 桌面（基线）

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset default && cmake --build --preset default
./build/default/src/app/openartemis /path/to/game/root.pfs
```

细节见 `README.md` 与 `docs/TESTING.md`。

---

## 2. Android APK

### 2.1 依赖

| 项 | 环境变量 | 本机示例 |
|---|---|---|
| vcpkg | `VCPKG_ROOT` | `/path/to/vcpkg` |
| Android NDK r28 | `ANDROID_NDK_HOME` | `/path/to/android/sdk/ndk/28.0.13004108` |
| Android SDK | `ANDROID_HOME` | `/path/to/android/sdk`（platform android-35 / build-tools 34.0.0） |
| JDK 17 | `JAVA_HOME`/PATH | `/path/to/jdk` |

### 2.2 构建（单段式，Gradle 驱动 native）

```bat
script\build-android.bat            :: debug（可直接安装）
script\build-android.bat release    :: release（minify+shrink，未签名）
```

手工等价：

```bat
cd android && gradlew.bat assembleDebug
```

- **产物**：`build/android-gradle/app/outputs/apk/debug/openartemis_v0.1.0.apk`
  （release 变体同目录下的 `release/`；Gradle 的 buildDirectory 在
  `android/build.gradle` 里统一重定向到仓库根 `build/`，所以 `android/` 下不会
  再长出一棵 build 树）。release 未签名，可用
  `zipalign -p 4` + `apksigner sign --ks %USERPROFILE%\.android\debug.keystore` 自签。
- **原生库**：由 AGP 的 `externalNativeBuild.cmake` 编译，产物在
  `build/android-gradle/app/intermediates/cxx/Debug/<hash>/obj/arm64-v8a/libopenartemis.so`，
  AGP 自动塞进 APK 的 `lib/arm64-v8a/`。
- **原生调试**：AGP 会把未 strip 的符号表(`.debug_info`/`.debug_line`/`.symtab`)
  留在 `intermediates/cxx/.../obj/` 下，Android Studio 打开 `android\` 目录、
  选 `app` + LLDB 即可下断点 / 单步 / 看变量。这正是采用 Gradle 驱动 native
  的原因 —— 两段式(CMake 先出 .so 再由 Gradle 打包)不带这套符号管理。
- **`ANDROID_PLATFORM` 必须显式传 `android-28`**（见 `android/app/build.gradle`
  的 `arguments`）：AGP 默认拿 `minSdk`(21) 当 `ANDROID_PLATFORM`，而 vcpkg 的
  `arm64-android` triplet 里 `VCPKG_CMAKE_SYSTEM_VERSION` 是 28。两者不一致时
  依赖按 28 编、App 按 21 链，`libSDL3.a` / `libavformat.a` 会报
  `undefined symbol: iconv_* / glob / fseeko64 / __getcwd_chk`。
- vcpkg toolchain 经 Gradle `arguments` 注入后再 chainload 到 NDK 的
  `android.toolchain.cmake`，依赖解析仍走 vcpkg（与 `android-arm64` preset 等价）。
- 模拟器（x86_64）：`android/app/build.gradle` 的 `abiFilters` 加 `'x86_64'`，
  `arguments` 里把 `VCPKG_TARGET_TRIPLET` 换成 `x64-android`。
- 依赖：`VCPKG_ROOT`（arm64-android triplet，含 ffmpeg：`find_package(FFMPEG)`
  命中即启用 WMV/MP4 解码）、`ANDROID_NDK_HOME`（r28）、`ANDROID_HOME`
  （platform android-35 / build-tools 34.0.0）、JDK 17。

### 2.3 壳层结构（改编自 krkrsdl3，命名刻意区分）

```
android/app/
  AndroidManifest.xml        .LauncherActivity(启动器) + .GameActivity(引擎)
  java/org/openartemis/
    LauncherActivity.java    游戏库界面(游戏目录/封面/标签/历史/单人参数)
    GameActivity.java        SDLActivity 子类:加载 SDL3 + libopenartemis.so,
                             传 argv,并可在原生启动前设 OA_SAVE_ROOT
    LauncherDatabase.java    启动器本地库(SQLite/SharedPreferences)
  java/org/libsdl/app/       SDL3 官方 Java 胶水(包名必须保持:
                             JNI 符号 Java_org_libsdl_app_SDLActivity_*)
  res/                       布局/主题/图标(主题 Theme.OpenArtemis)
```

**启动器图标**（自绘，与 krkrsdl3 无关）：

图形 = 夜空 + 新月（Artemis）+ 播放三角（引擎"播放"剧本）+ 三行剧本文字，
配色取本 App 自己的蓝（`#1565C0`/`#42A5F5`）与墨色（`#1a1a2e`）。
资源布局：

```
res/mipmap-<density>/ic_launcher.png             旧式全幅（mdpi 48 → xxxhdpi 192）
res/mipmap-<density>/ic_launcher_round.png       圆形版（四角透明，API 25 前）
res/mipmap-<density>/ic_launcher_foreground.png  自适应前景（108dp 栅格，画在 72dp 安全区内）
res/mipmap-<density>/ic_launcher_background.png  自适应背景
res/mipmap-<density>/ic_launcher_monochrome.png  Android 13+ 主题图标（单色遮罩）
res/mipmap-anydpi-v26/ic_launcher.xml            <adaptive-icon> 组装上面三层
```

生成与复核脚本（源码入库，`tools/` 下；编译出的 .exe 不入库）：

```powershell
csc /out:tools\gen_icon.exe   tools\gen_icon.cs   && tools\gen_icon.exe     # 重画全部密度
csc /out:tools\check_icon.exe tools\check_icon.cs && tools\check_icon.exe   # 复核配色/透明/安全区
```

图标是**画的**（`System.Drawing` 几何绘制），不是裁切或复用他人素材；
`check_icon` 会断言关键区域的颜色（天空/新月/播放三角/文字行）与
"前景美术必须落在 72/108 安全区内、圆形版四角透明"。

**与 krkrsdl3 的命名差异**（可与 krkrsdl3 APK 共存、互不干扰）：

| 面 | krkrsdl3 | openartemis |
|---|---|---|
| applicationId / 包 | `org.tvp.krkrsdl3` | `org.openartemis` |
| 启动器 Activity | `MainActivity` | `LauncherActivity` |
| 引擎 Activity | `KRKRActivity` | `GameActivity` |
| 原生库 | `libkrkrsdl3.so` | `libopenartemis.so` |
| SharedPreferences | `krkrsdl` | `openartemis` |
| 默认启动文件 | `data.xp3` | `root.pfs`（或含 `system.ini` 的解包目录） |

### 2.4 argv 约定（`GameActivity` → `main()`）

启动器组装（`LauncherActivity.buildLaunchArgs`）：

```
<数据源绝对路径> --platform android [--renderer gles]
```

- 数据源：目录里存在 `root.pfs` → 传该文件；否则传目录本身（解包项目）。
  引擎侧挂载见 `src/app/main.cpp`（`PhysFileSystem`：包 + 同目录 sidecar 覆盖）。
- 每游戏存档隔离（勾选时）：不走 argv，由 `GameActivity` 在 `super.onCreate`
  之前 `SDLActivity.nativeSetenv("OA_SAVE_ROOT", <dir>)` —— 引擎的存档根策略
  就是读这个环境变量（`host_save_root`）。
- 渲染线：全局设置面板"渲染器"单选，缺省 **硬件渲染**（传 `--renderer gles`，原生
  GLES 后端）；选"软件渲染"则不传 `--renderer`，由引擎走自己的缺省线 `sdl`
  （SDL3 SDL_Render）。单人配置可覆盖全局。
- 平台段：面板"平台"单选，缺省 `android`，可切 `windows`，决定 `system.ini` 读
  `[android]` 还是 `[windows]`（同时参与脚本侧 os 判定）。非法值回落到 `android`。

### 2.4.1 触屏手势（`src/app/main.cpp`，Android/iOS/wasm 共用）

SDL 的触摸→鼠标合成（`SDL_TOUCH_MOUSE_EVENTS`）**默认关闭**，所以移动端必须在宿主
把手指事件翻成指针事件，否则引擎收不到任何点击。手势模型对齐 krkrsdl3 参照实现
（`cpp/environ/sdl3/sdl3_app.cpp`）：

| 手势 | 映射 |
|---|---|
| 单指轻点 | 左键 按下+抬起（无移动时） |
| 单指拖动 | 鼠标移动（超过 12px 阈值才算拖动，拖动后不再触发点击） |
| 双指轻点 | **右键** 按下+抬起（Artemis 把右键接在 EXIT / UI 返回链上） |
| 三指及以上 | **不映射**（参照实现在此打开自己的原生菜单，本引擎没有该菜单） |

合成事件走 `SDL_PushEvent` 进 SDL 事件队列，因此和真实指针走完全相同的路径
（窗口→舞台坐标换算、左/右键 vk 边沿都不需要第二套代码）。

两条让点击"灵敏"、且不与双指冲突的关键处理（都是踩过的坑）：

- **手指按下时一个事件都不发**：手指落下还不构成手势——第二根手指随时可能跟上来
  把它变成右键。按下就推 motion（更别说推按下）正是让双指与单击**打架**的原因：
  引擎看到指针移到按钮上、在还没判定完手势时就派发了一次左键点击。
  krkrsdl3 参照实现同样在 finger-down 什么都不发，一切等 finger-up 判定。
- **抬起时先补 motion，再推按键**：引擎的点击命中判定用的是它本帧持有的指针位置
  （`runtime` 的 `mouse_x_/mouse_y_`），而那个位置**只由 MOTION 事件写入**。
  轻点若不先推 motion，就会按"上一次的位置"判定（首次点击是 0,0）——表现就是
  **按钮会高亮但点击没反应**。桌面鼠标天然"先移动再点击"，这里保持一致。
- **按下与抬起分两帧**：`SDL_AppEvent` 一次把队列里的事件全消费掉，再跑本帧的
  `SDL_AppIterate`。若同一个 tick 里既推 down 又推 up，引擎只会看到一个单帧边沿、
  从没有"按住的帧"。因此按下立即推、抬起延到下一帧（`touch_advance_frame`）。

判定顺序与参照一致：以**最后一根手指抬起**为准（先看剩余手指数、再删除），
单指→左键、双指→右键，拖动过的手指不算轻点。

诊断：`OA_TOUCH_DIAG=1` 会把每次轻点打印成
`[touch] one-finger/two-finger tap win=(x,y) stage=(x,y)`，
用来区分"触点坐标不对"和"命中判定没跟上"。

### 2.5 运行注意

- 需要 MANAGE_EXTERNAL_STORAGE / 存储权限才能读取任意游戏目录（manifest 已声明；
  Android 11+ 需在系统设置里授予"所有文件访问"）。
- 存档写入应用私有目录（无需权限）；`scopedsavedir` 打开时写到
  `Android/data/<pkg>/files/save/<游戏名>`。
- 日志：`adb logcat | findstr openartemis`（引擎 stdout 也走 logcat）。

---

## 3. WebAssembly（浏览器）

### 3.1 构建

```bat
call C:\path\to\emsdk\emsdk_env.bat  :: 需要 emcc 在 PATH(vcpkg wasm triplet 也要)
script\build-wasm.bat
```

手工等价：`cmake --preset wasm && cmake --build build\wasm --config Release`。

产物：`build/wasm/src/app/index.html` / `index.js` / `index.wasm`（≈4 MB）/
`index.worker.js`。wasm 目标**不含 ffmpeg**（`vcpkg.json` 里
`"platform": "!wasm32"`），视频走 Ogg/Theora。

**多线程**：浏览器构建链接真 pthreads（引擎的解码/音频 worker 是
`std::thread`）。因此：

- 依赖必须用带线程的 triplet 重建（preset 已指向
  `cmake/triplets/wasm32-emscripten-threads.cmake`）；
- 顶层 CMakeLists 对 EMSCRIPTEN 全局加 `-pthread`（模块里每个对象都要带
  atomics/bulk-memory 特性，混链会被 `wasm-ld` 拒绝）；
- 页面必须由带 COOP/COEP 的服务器提供（`web/serve.py`），否则
  `SharedArrayBuffer` 不可用 —— `web/index.html` 打开时会检查并给出提示。

### 3.2 页面壳（改编自 krkrsdl3 的 emscripten 壳）

`web/index.html` 由 `--shell-file` 链入，提供：

- 加载进度/错误浮层、页面内日志面板（快捷键 <code>`</code>）；
- **游戏数据入口**：`?pfs=root.pfs`（fetch 到 MEMFS 后启动）、
  `?preload=1`（配 `filelist.json` 的目录式项目，逐文件下载进 `/data` 再启动）、
  `?dir=/data`（已预载/已选择的目录）、`?args=…`（追加/替换 argv，可与
  `?preload` 组合，例如 `?preload=1&args=--frames,180`），或交互式选择
  （拖拽 / 选 `.pfs` 文件 / 选整个游戏目录）；
- 存档：`/save`（IDBFS）由引擎侧挂载并持久化到 IndexedDB，页面每 5 s 触发一次
  `FS.syncfs`，标签页隐藏/关闭时也同步；
- 音频解锁（浏览器要求用户手势后才能出声）；
- SharedArrayBuffer 检查（多线程构建的前置条件，缺失时给出可操作提示）。

`-sINVOKE_RUN=0` + `Module.callMain(argv)`：浏览器没有 argv，页面先把数据写进
MEMFS 再启动引擎。

本地预览：

```bash
python web/serve.py <含 index.* 与 root.pfs 的目录> 8080
# 浏览器: http://localhost:8080/index.html?pfs=root.pfs
```

无头验证（CI/本机冒烟，读页面日志面板）：

```bash
chrome --headless=new --remote-debugging-port=9222 \
       "http://localhost:8080/index.html?preload=1&args=--frames,180" &
node tools/cdp_dump.js 9222      # 打印日志面板内容（引擎启动/渲染/音频/退出）
```

---

## 4. 验证状态（诚实记录）

| 面 | 状态 |
|---|---|
| 桌面构建 + ctest 69 | ✅ 通过（24 过 / 4 平台性失败 / 41 真包自跳，与基线逐项一致） |
| Android 原生库构建（arm64-v8a, NDK r28 + vcpkg(含 ffmpeg)） | ✅ `libopenartemis.so` 链接成功（strip 后 ≈21 MB；未 strip ≈130 MB），导出 `SDL_main` |
| Android APK 打包（Gradle 8.10 + AGP 8.7.3 + Java 壳） | ✅ `assembleDebug` 15.1 MB（debug 签名，可直接安装）+ `assembleRelease` 12.0 MB（未签名，`script\build-android.bat release`） |
| Android 设备实跑 | ⛔ 未做（本机无设备/模拟器；`adb devices` 为空）。原生入口/argv/存档根在代码面成立，实机需另行验证 |
| wasm 构建（Emscripten 4.0.23 + pthreads） | ✅ `index.html` / `index.js` / `index.wasm`(≈4 MB) / `index.worker.js` |
| wasm 浏览器实跑 | ✅ headless Chrome 端到端跑通：预载目录式项目 → 挂载 → 打开窗口/渲染器（stage 1280x720）→ **打开音频设备(44100 Hz)** → 180 帧 tick/渲染 → 正常退出（`--frames 180`）。观测方式：`--remote-debugging-port` + `tools/cdp_dump.js`（读页面日志面板） |
| 真实游戏资源 | ⛔ 本机无游戏包，浏览器验证用的是合成最小项目（`system.ini` + `system/first.iet`） |

### 4.1 移植期发现并修掉的问题（都是"平台壳/工具链"面，非引擎逻辑）

1. **`PHYSFS_init` 在无 `/proc` 的目标上必然失败**（wasm，Android 也可能）：
   PhysicsFS 的 POSIX 层先探 `/proc/self/exe`，拿不到再要求 `argv0 != NULL`
   （`PHYSFS_ERR_ARGV0_IS_NULL`），而引擎传的是 `nullptr` ⇒ 返回 0 且
   "no error"，项目根本挂不上。修法（`src/core/fs/physfs_fs.cpp`）：在
   `__EMSCRIPTEN__`/`__ANDROID__` 上传一个含目录分隔符的 argv0 提示
   （`"/openartemis"`，只有 `PHYSFS_getBaseDir` 会读它，引擎不用）。
2. **`$HOME` 缺失**：PhysicsFS 的用户目录还要 `getenv("HOME")` 或
   `getpwuid`，浏览器/Android 应用两者都没有 —— wasm 的 `platform_wasm.cpp`
   把 `HOME` 指向 `/save`（IDBFS 挂载点），Android 的 `platform_android.cpp`
   指向应用私有目录。
3. **Emscripten 默认关闭 C++ 异常**：宿主用 try/catch 包住了项目打开/启动/
   解码路径，libc++ 也会 throw（`std::filesystem` 等），未启用异常时一次
   throw 直接终止模块（现象：打印版本横幅后立刻退出）。修法：
   `-fexceptions -sDISABLE_EXCEPTION_CATCHING=0`。
4. **线程**：引擎的解码/音频 worker 是真 `std::thread`。单线程 wasm 建不出
   线程（音频永远起不来），因此浏览器构建走真 pthreads：全局 `-pthread`
   （顶层 CMakeLists）+ 依赖用 `cmake/triplets/wasm32-emscripten-threads.cmake`
   重编（vcpkg 默认 triplet 不带 `-pthread`，混链会被 `wasm-ld` 以
   `--shared-memory is disallowed ... atomics/bulk-memory` 拒绝）+ 页面
   COOP/COEP（`web/serve.py` 已发，`web/index.html` 启动时检查并给出提示）。
5. **`--shell-file` 不是构建依赖**：改 `web/index.html` 不会触发重链
   （emcc 会把旧壳固化进 `index.html`）。修法：`LINK_DEPENDS`。
