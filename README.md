# openartemis — artemisEngine

C++20 + SDL3 的 **artemisEngine（Artemis 系视觉小说引擎）**：直接运行
Artemis 系 PFS 游戏资源包（`root.pfs`，可带分卷）。本仓库不含任何游戏资产；
真包回归测试需要哪些游戏包由各测试文件头部注释与 `docs/TESTING.md` 说明
（`OA_TEST_*_PFS` 环境变量指定路径）。

开发基线 = **真实运行验证与画面/流程表现**：场景树/文本/媒体/存档/UI 事件/
输入/转场/动画等已实现面逐项按实际表现校准，行为正确性由真实运行验证自证。

- 文档：
  - `docs/ARCHITECTURE.md` — **整体架构**（分层/数据流/模块职责/平台/构建/测试）
  - `docs/TESTING.md` — `OA_*` 环境变量唯一权威表 + 测试布局与验收流
  - `docs/PLATFORMS.md` — **Android APK / WebAssembly 构建与打包**（含壳层说明）
- 引擎：`src/core`（无窗口依赖）+ `src/app`（SDL3 宿主）
- 工具：`tools/`（C++ CLI: opfs / psb / asb / oasave + 脚本: pfs.py / mkfixture.py /
  cdp_dump.js；清单见 `tools/README.md`）
- 平台壳：`android/`（Gradle + Java 启动器界面）+ `web/`（Emscripten 页面壳）
- 调查工具：`tools/pfs.py`（Python 只读 PFS 查看器，开发期与 C++ 工具互验）

## 构建与运行（CMake + vcpkg）

```bash
export VCPKG_ROOT=/path/to/vcpkg   # vcpkg 所在目录
cmake --preset default
cmake --build --preset default
# 干净用户版（无测试/autodrive/探针代码）
./build/default/src/app/openartemis /path/to/game/root.pfs   # 带窗运行（Esc 退出）
./build/default/src/app/openartemis --headless --frames 900 /path/to/game/root.pfs
OA_SAVE_ROOT=/path ./build/default/src/app/openartemis /path/to/game/root.pfs  # 覆盖存档根（默认=游戏数据所在目录）
# 测试超集版（autodrive 回归流 OA_AUTODRIVE=exit|title|help|conf|r10save|r10t|r10blog|qld|d38 等）
OA_SAVE_ROOT=/path ./build/default/src/app/openartemis_test /path/to/game/root.pfs
cd build/default && OA_TEST_FPM_PFS=/path/to/game/root.pfs ctest   # 全量测试（tests/ + tests/fpm/；真包测试需按各测试注释准备游戏包）
# ASAN 诊断构建：cmake --preset asan && cmake --build --preset asan
```

- 两个程序共享同一实现（src/app/main.cpp，测试版经 src/app/main_test.cpp 以
  OA_TEST_BUILD=1 编译）：`openartemis`=干净用户版，`openartemis_test`=含全部
  autodrive/探针/诊断 env 的回归版；除测试设施外行为一致。
- 环境变量总表、测试布局与快速验证：**`docs/TESTING.md`**（唯一权威 OA_* 清单）。

依赖（全部 vcpkg manifest）：SDL3、freetype、libpng、libjpeg-turbo、libvorbis、
libtheora、physfs（+ 可选 ffmpeg：桌面/Android 有，wasm 自动剔除）；Lua 5.1.5 为
vendored 源码（`third_party/lua-5.1.5`）。不引入 stb 系库。

## 其它平台（Android / WebAssembly）

```bash
# Android APK（arm64-v8a；需要 VCPKG_ROOT / ANDROID_NDK_HOME(r28) / ANDROID_HOME / JDK17）
script\build-android.bat            # 原生库 + Gradle 打包 -> android\app\build\outputs\apk\debug\*.apk

# 浏览器（Emscripten；需要 EMSDK：先跑 emsdk_env）
script\build-wasm.bat               # -> build\wasm\src\app\index.html|index.js|index.wasm
python web\serve.py <目录> 8080      # 浏览器打开 http://localhost:8080/index.html?pfs=root.pfs
```

两者共用同一份 `src/app/main.cpp` 与 `src/core`；差异集中在 `src/app/platform/`
（`platform_android.cpp` = 应用私有存档根 + 生命周期闩锁；`platform_wasm.cpp` =
IDBFS `/save` 挂载 + 页面持久化/可见性钩子）。Android 启动器界面与网页壳分别改编
自 krkrsdl3 的 Java 界面 / emscripten 页面壳，细节见 `docs/PLATFORMS.md`。

## 许可

本项目采用 **MIT 或 GPL-3.0-or-later 二选一** 双许可
（`SPDX-License-Identifier: MIT OR GPL-3.0-or-later`），Copyright (c) 2026 deepseek：

- 选用 **MIT**：见 `LICENSE` 的 ALTERNATIVE A；
- 选用 **GNU GPL v3 或更高版本**：见 `LICENSE` 的 ALTERNATIVE B。

代码不含任何游戏资产；`third_party/`（如 Lua 5.1.5）与 vcpkg 依赖仍分别遵循
各自的原始许可。
