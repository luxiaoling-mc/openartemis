# Unit tests (plain assert executables registered with CTest).
#
# Layout: this directory keeps GENERIC engine tests (self-contained fixtures;
# no real game assets required). Tests that need a real Artemis game archive
# (root.pfs / an extracted project tree) live in tests/fpm/ and are wired in
# by add_subdirectory(fpm) at the bottom — see docs/TESTING.md.
find_package(Vorbis CONFIG REQUIRED)

add_executable(ini_project_test ini_project_test.cpp)
target_link_libraries(ini_project_test PRIVATE libopenartemis)
add_test(NAME ini_project_test COMMAND ini_project_test)
set_tests_properties(ini_project_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(dir_fs_test dir_fs_test.cpp)
target_link_libraries(dir_fs_test PRIVATE libopenartemis)
add_test(NAME dir_fs_test COMMAND dir_fs_test)

add_executable(decode_pool_test decode_pool_test.cpp)
target_link_libraries(decode_pool_test PRIVATE libopenartemis)
add_test(NAME decode_pool_test COMMAND decode_pool_test)

add_executable(lua_smoke_test lua_smoke_test.cpp)
target_link_libraries(lua_smoke_test PRIVATE libopenartemis lua51)
add_test(NAME lua_smoke_test COMMAND lua_smoke_test)

add_executable(interpreter_test interpreter_test.cpp)
target_link_libraries(interpreter_test PRIVATE libopenartemis)
add_test(NAME interpreter_test COMMAND interpreter_test)

add_executable(variable_test variable_test.cpp)
target_link_libraries(variable_test PRIVATE libopenartemis)
add_test(NAME variable_test COMMAND variable_test)

add_executable(asb_test asb_test.cpp)
target_link_libraries(asb_test PRIVATE libopenartemis)
add_test(NAME asb_test COMMAND asb_test)
set_tests_properties(asb_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(compositor_test compositor_test.cpp)
target_link_libraries(compositor_test PRIVATE libopenartemis)
add_test(NAME compositor_test COMMAND compositor_test)

# layer-model S1/S2 (research/105): 分类权威 layer_kind.h kind_of 断言组 +
# 统一 invalidate 簿记集合/写入口标记面(纯新增;不改任何现有行为)。
add_executable(layer_kind_test layer_kind_test.cpp)
target_link_libraries(layer_kind_test PRIVATE libopenartemis)
add_test(NAME layer_kind_test COMMAND layer_kind_test)

# layer-model S3 (research/106): ContentRole 家族投影/面方法/quad 矩阵断言
# (测试内参考实现对照;纯新增;不改任何现有行为)。
add_executable(content_role_test content_role_test.cpp)
target_link_libraries(content_role_test PRIVATE libopenartemis)
add_test(NAME content_role_test COMMAND content_role_test)

add_executable(layer_semantics_test layer_semantics_test.cpp)
target_link_libraries(layer_semantics_test PRIVATE libopenartemis)
add_test(NAME layer_semantics_test COMMAND layer_semantics_test)

add_executable(transform_test transform_test.cpp)
target_link_libraries(transform_test PRIVATE libopenartemis)
add_test(NAME transform_test COMMAND transform_test)

add_executable(event_tags_test event_tags_test.cpp)
target_link_libraries(event_tags_test PRIVATE libopenartemis)
add_test(NAME event_tags_test COMMAND event_tags_test)

add_executable(lua_bridge_test lua_bridge_test.cpp)
target_link_libraries(lua_bridge_test PRIVATE libopenartemis)
add_test(NAME lua_bridge_test COMMAND lua_bridge_test)

add_executable(script_test script_test.cpp)
target_link_libraries(script_test PRIVATE libopenartemis)
add_test(NAME script_test COMMAND script_test)
# script_test reads OA_TEST_FPM_PFS for an extra real-archive arm (system/first.iet
# decode: 28 instructions / 5 labels). The variable was never propagated to the
# ctest entry, so that arm silently never ran under ctest (research/117).
set_tests_properties(script_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(pfs_test pfs_test.cpp)
target_link_libraries(pfs_test PRIVATE libopenartemis)
add_test(NAME pfs_test COMMAND pfs_test)

add_executable(transition_test transition_test.cpp)
target_link_libraries(transition_test PRIVATE libopenartemis)
add_test(NAME transition_test COMMAND transition_test)
set_tests_properties(transition_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(input_dispatch_test input_dispatch_test.cpp)
target_link_libraries(input_dispatch_test PRIVATE libopenartemis lua51)
add_test(NAME input_dispatch_test COMMAND input_dispatch_test)
set_tests_properties(input_dispatch_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(render2_test render2_test.cpp)
target_link_libraries(render2_test PRIVATE libopenartemis)
add_test(NAME render2_test COMMAND render2_test)
set_tests_properties(render2_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(text_domain_test text_domain_test.cpp)
target_link_libraries(text_domain_test PRIVATE libopenartemis)
add_test(NAME text_domain_test COMMAND text_domain_test)
set_tests_properties(text_domain_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(text_reveal_test text_reveal_test.cpp)
target_link_libraries(text_reveal_test PRIVATE libopenartemis)
add_test(NAME text_reveal_test COMMAND text_reveal_test)
set_tests_properties(text_reveal_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(media_state_test media_state_test.cpp)
target_link_libraries(media_state_test PRIVATE libopenartemis)
add_test(NAME media_state_test COMMAND media_state_test)

add_executable(media_runtime_test media_runtime_test.cpp)
target_link_libraries(media_runtime_test PRIVATE libopenartemis lua51 Vorbis::vorbisenc)
add_test(NAME media_runtime_test COMMAND media_runtime_test)
set_tests_properties(media_runtime_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(video_decode_test video_decode_test.cpp)
target_link_libraries(video_decode_test PRIVATE libopenartemis)
add_test(NAME video_decode_test COMMAND video_decode_test)
# This generic-location test drives a REAL fpm asset (movie/logo.ogv), so it
# self-skips with 77 when OA_TEST_FPM_PFS is empty. Declare the skip contract:
# without SKIP_RETURN_CODE the no-asset configuration reported it as a hard
# FAILURE instead of a skip, poisoning the "green" signal (research/117).
set_tests_properties(video_decode_test PROPERTIES
  ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77)

# research/118: the audio output latency bound (standing device-stream queue =
# standing audible delay). Asset-free: pushes through the engine's own mix /
# movie paths onto a real-time dummy device, asserts the bounded push, and
# runs the pre-fix behaviour (bound disabled) as an in-process negative
# control. SDL_AUDIODRIVER overrides the backend for a real-machine run.
add_executable(audio_latency_bound_test audio_latency_bound_test.cpp)
target_link_libraries(audio_latency_bound_test PRIVATE libopenartemis)
add_test(NAME audio_latency_bound COMMAND audio_latency_bound_test)
set_tests_properties(audio_latency_bound PROPERTIES SKIP_RETURN_CODE 77)

# research/100: `_m` mask-partner engine tests (asset-free synthetic theora
# pair; tests/theora_fixture.h encodes with libtheoraenc at test time).
find_package(unofficial-theora CONFIG REQUIRED)
add_executable(video_mask_test video_mask_test.cpp)
target_link_libraries(video_mask_test PRIVATE libopenartemis unofficial::theora::theoraenc)
add_test(NAME video_mask_test COMMAND video_mask_test)

add_executable(save_domain_test save_domain_test.cpp)
target_link_libraries(save_domain_test PRIVATE libopenartemis lua51 Vorbis::vorbisenc)
add_test(NAME save_domain_test COMMAND save_domain_test)
set_tests_properties(save_domain_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

# U15: save-area image read-back (slot thumbnails) routes through the
# runtime SaveStore, so plain runs (default ./save) can display [savess]
# PNGs without OA_SAVE_ROOT. RenderEngine pulls font.cpp's FreeType calls.
find_package(Freetype CONFIG REQUIRED)
add_executable(save_thumb_readback_test save_thumb_readback_test.cpp)
target_link_libraries(save_thumb_readback_test PRIVATE libopenartemis Freetype::Freetype)
add_test(NAME save_thumb_readback COMMAND save_thumb_readback_test)
set_tests_properties(save_thumb_readback PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(p1c2_control_test p1c2_control_test.cpp)
target_link_libraries(p1c2_control_test PRIVATE libopenartemis lua51)
add_test(NAME p1c2_control_test COMMAND p1c2_control_test)
set_tests_properties(p1c2_control_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

add_executable(backlog_var_bridge_test backlog_var_bridge_test.cpp)
target_link_libraries(backlog_var_bridge_test PRIVATE libopenartemis)
add_test(NAME backlog_var_bridge_test COMMAND backlog_var_bridge_test)

add_executable(misc_b_test misc_b_test.cpp)
target_link_libraries(misc_b_test PRIVATE libopenartemis lua51 Vorbis::vorbisenc)
add_test(NAME misc_b_test COMMAND misc_b_test)
set_tests_properties(misc_b_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}")

# research/113: 读档加固的回归网（同版本内的边界/畸形存档语料 + [C] 桶读档清场
# 断言 + 失败原子性 + 事件泵确定性）。跨版本兼容按用户裁定不做，仅留 1 条记录性
# 用例（S5）作为未来预留。
add_executable(save_compat_test save_compat_test.cpp)
target_link_libraries(save_compat_test PRIVATE libopenartemis lua51)
add_test(NAME save_compat_test COMMAND save_compat_test)

# Real-game-asset tests (need OA_TEST_FPM_PFS; see docs/TESTING.md).
add_subdirectory(fpm)
