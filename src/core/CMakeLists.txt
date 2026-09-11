# libopenartemis: engine library (no windowing dependency yet)
add_library(libopenartemis STATIC
  version.cpp
  util/sha1.cpp
  util/charset.cpp
  fs/project.cpp
  fs/physfs_fs.cpp
  util/binary_stream.cpp
  runtime/runtime.cpp        # 内核 + 控制域 + 对外薄转发
  runtime/runtime_media.cpp
  runtime/runtime_save.cpp
  # Lua 域：整个 Lua 宿主面收成一个 TU 并放进
  # runtime 家族——桥对象 + 各域注册段（tag/sys/input/surface/
  # emote/filter/pluto），原 src/core/lua/ 的 8 个 .cpp 与 lua_api.h 全部
  # 消失；公开契约只留瘦头 runtime/runtime_lua.h（"小接口，大文件"）。
  runtime/runtime_lua.cpp
  # The whole oa::runtime module in ONE translation unit: values
  # + expressions, the .iet/.ast text parser, the .asb binary decoder and the
  # interpreter. Six script/*.cpp TUs merged here; public face is the single
  # thin header runtime/runtime_iet.h ("small interface, big file").
  runtime/runtime_iet.cpp
  render/layer.cpp
  # render/anim.cpp + render/transition.cpp 并入 render/layer.cpp（同一个渲染域
  # TU，"小接口，大文件"）：the anim (easing/tween/[anime])
  # and transition ([trans] machine) semantics; declarations are the internal
  # header render/render_internal.h (public faces: renderer.h/layer.h/runtime.h).
  # Placement is link-closure sensitive: layer.cpp.o carries no font/backend
  # deps, renderer.cpp.o does (see the banner at the top of layer.cpp).
  render/backend_gles.cpp
  render/backend_sdl.cpp
  render/font.cpp
  render/renderer.cpp
  render/text.cpp
  emote/psb_reader.cpp
  emote/emote_file.cpp
  emote/emote_render.cpp
  emote/emote_player.cpp
  # Vendored BC7/BPTC block decoder (richgel999/bc7enc, MIT/public domain;
  # provenance + license pointers in emote/bc7decomp.h / .cpp). Only the
  # E-mote atlas path uses it, so it lives beside its single consumer
  # instead of third_party/.
  emote/bc7decomp.cpp
  # media domain, one big TU per domain ("small interface, big
  # file"): audio.cpp = audio.cpp + vorbis.cpp + players.cpp (state machine,
  # vorbis decode source, playback host); video.cpp = video.cpp +
  # theora.cpp + the ffmpeg_source.cpp section (its `#if OA_HAVE_FFMPEG`
  # gate is INSIDE the file now, so the conditional target_sources() that used
  # to add media/ffmpeg_source.cpp is gone). decode_pool.cpp (shared worker
  # host) and image.cpp (still-image codec) stay their own single-purpose TUs.
  # Public faces: media/audio.h, media/video.h, media/decode_pool.h,
  # media/image.h; the backends/seam live in media/media_internal.h.
  media/audio.cpp
  media/decode_pool.cpp
  media/image.cpp
  media/video.cpp
)
# headers are included as <core/...>, <pfs/...> from the src root
target_include_directories(libopenartemis PUBLIC
  ${PROJECT_SOURCE_DIR}/src
)
find_package(Vorbis CONFIG REQUIRED)
find_package(unofficial-theora CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)
# JPEG decode: libjpeg-turbo. Android/iOS-shipped Artemis
# archives (thyt) store story bgs (image/bg) and extras (image/ez) as real
# JPEG while ev/fg/cg/rule stay PNG; media::decode_image dispatches on magic
# and RenderEngine probes the .jpg extension for extension-less layer files.
find_package(JPEG REQUIRED)
# PhysicsFS (physfs_fs.cpp): single project-source VFS. The Artemis PFS pack
# support is a custom PHYSFS_Archiver built into the core library.
find_package(PhysFS CONFIG REQUIRED)
target_link_libraries(libopenartemis PUBLIC
    SDL3::SDL3
    lua51
    Vorbis::vorbisfile
    unofficial::theora::theora
    PNG::PNG
    JPEG::JPEG
    $<IF:$<TARGET_EXISTS:PhysFS::PhysFS>,PhysFS::PhysFS,PhysFS::PhysFS-static>)

# ---------------------------------------------------------------------------
# Optional FFmpeg video backend: decodes ASF/WMV3 (and other
# libavformat containers) through the vcpkg "ffmpeg" port — pinned 6.1.1 in
# vcpkg.json, features trimmed to avcodec+avformat (libavutil always); no
# swscale/swresample/avfilter/avdevice. The backend is strictly optional:
# without FFmpeg the engine keeps its Ogg/Theora-only decode surface and
# undecodable files take the documented logical immediate-finish fallback
# (media/video.h) — builds never break, only the WMV playback surface
# disappears. The backend section lives inside
# media/video.cpp behind its own `#if defined(OA_HAVE_FFMPEG)` gate, so this
# block only defines the macro and wires the libraries.
# ---------------------------------------------------------------------------
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND)
  target_compile_definitions(libopenartemis PUBLIC OA_HAVE_FFMPEG=1)
  target_include_directories(libopenartemis SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(libopenartemis PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(libopenartemis PRIVATE ${FFMPEG_LIBRARIES})
  message(STATUS "openartemis: FFmpeg video backend ENABLED (${FFMPEG_LIBRARIES})")
else()
  message(STATUS "openartemis: FFmpeg video backend DISABLED (not installed) — Ogg/Theora only")
endif()

if(MSVC)
  target_compile_options(libopenartemis PRIVATE /W4)
else()
  target_compile_options(libopenartemis PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Vendored Lua 5.1.5 (third_party/lua-5.1.5) as a static lib.
# ---------------------------------------------------------------------------
set(LUA51_SRC_DIR ${PROJECT_SOURCE_DIR}/third_party/lua-5.1.5/src)
add_library(lua51 STATIC
  ${LUA51_SRC_DIR}/lapi.c
  ${LUA51_SRC_DIR}/lauxlib.c
  ${LUA51_SRC_DIR}/lbaselib.c
  ${LUA51_SRC_DIR}/lcode.c
  ${LUA51_SRC_DIR}/ldblib.c
  ${LUA51_SRC_DIR}/ldebug.c
  ${LUA51_SRC_DIR}/ldo.c
  ${LUA51_SRC_DIR}/ldump.c
  ${LUA51_SRC_DIR}/lfunc.c
  ${LUA51_SRC_DIR}/lgc.c
  ${LUA51_SRC_DIR}/linit.c
  ${LUA51_SRC_DIR}/liolib.c
  ${LUA51_SRC_DIR}/llex.c
  ${LUA51_SRC_DIR}/lmathlib.c
  ${LUA51_SRC_DIR}/lmem.c
  ${LUA51_SRC_DIR}/loadlib.c
  ${LUA51_SRC_DIR}/lobject.c
  ${LUA51_SRC_DIR}/lopcodes.c
  ${LUA51_SRC_DIR}/loslib.c
  ${LUA51_SRC_DIR}/lparser.c
  ${LUA51_SRC_DIR}/lstate.c
  ${LUA51_SRC_DIR}/lstring.c
  ${LUA51_SRC_DIR}/lstrlib.c
  ${LUA51_SRC_DIR}/ltable.c
  ${LUA51_SRC_DIR}/ltablib.c
  ${LUA51_SRC_DIR}/ltm.c
  ${LUA51_SRC_DIR}/lundump.c
  ${LUA51_SRC_DIR}/lvm.c
  ${LUA51_SRC_DIR}/lzio.c
  ${LUA51_SRC_DIR}/print.c
)
target_include_directories(lua51 PUBLIC ${LUA51_SRC_DIR})
# Lua compiled as C (it is C code); silence third-party warnings.
target_compile_options(lua51 PRIVATE -w)
if(NOT MSVC)
  target_compile_options(lua51 PRIVATE -fPIC)
endif()
