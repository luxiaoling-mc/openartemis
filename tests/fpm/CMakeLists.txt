# Real-game regression tests: every target here boots or reads a REAL Artemis
# game archive (fpm/root.pfs, or NekoMiko/root.pfs for the U6b nekomiko_p0
# driver) pointed at by OA_TEST_FPM_PFS / OA_TEST_NEKOMIKO_PFS, so each test
# carries
#   ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77
# (or the NekoMiko equivalent) and self-skips (exit 77) when the archive is
# not configured.
# Generic (asset-free) engine tests stay in tests/ (../CMakeLists.txt).
# See docs/TESTING.md.
find_package(Freetype CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)

# Resource tiering (2026-09): heavy real-game tests are serialized by default
# (memory-safe on modest machines). Machines with ample RAM can build with
# -DOA_TEST_SERIALIZE_HEAVY=OFF and run ctest -jN for speed — the fpm tests
# then run in parallel (resource tiering: see docs/ARCHITECTURE.md §7).
option(OA_TEST_SERIALIZE_HEAVY "Serialize heavy real-game tests (memory-safe default)" ON)
if(OA_TEST_SERIALIZE_HEAVY)
  set(_OA_RS RUN_SERIAL TRUE)
else()
  set(_OA_RS "")
endif()



# research/72: NekoMiko UI save->load->walk driver — post-load page turns must
# keep replacing the story text (user: text dead after 读档; research/72).
add_executable(nekomiko_txtload_test nekomiko_txtload_test.cpp)
target_link_libraries(nekomiko_txtload_test PRIVATE libopenartemis)
add_test(NAME nekomiko_txtload COMMAND nekomiko_txtload_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(nekomiko_txtload PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(nekomiko_txtload PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# U6b P0 (research/41): NekoMiko boot/title/story driver. Uses its own
# OA_TEST_NEKOMIKO_PFS archive variable (auto-detected at the top level).
add_executable(nekomiko_p0_test nekomiko_p0_test.cpp)
target_link_libraries(nekomiko_p0_test PRIVATE libopenartemis)
add_test(NAME nekomiko_p0 COMMAND nekomiko_p0_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(nekomiko_p0 PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(nekomiko_p0 PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/86: real rr movie-audio acceptance — engine-level pump over
# movie/d2z4ydi8wv.mp4 plus a story journey to the opening movie
# (movie/d1e86u7ftq.mp4) asserting live container audio through EOF.
# OA_TEST_RR_PFS points at the rr root.pfs FILE (its own real parent
# directory supplies the loose movie/ files). Skips (77) when unset.
add_executable(rr_movie_audio_test rr_movie_audio_test.cpp)
target_link_libraries(rr_movie_audio_test PRIVATE libopenartemis)
add_test(NAME rr_movie_audio COMMAND rr_movie_audio_test)
set_tests_properties(rr_movie_audio PROPERTIES
  ENVIRONMENT "OA_TEST_RR_PFS=${OA_TEST_RR_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/87: real HCT (灵感满溢的甜蜜创想凸) movie-audio acceptance — the
# stutter-report title. Engine-level pump over the OP movie_bai/o4p6jwsag.mp4
# (init.movie.op, HEVC 29.97 fps + AAC 44.1 kHz stereo) plus a boot journey
# through the movie_bai/logo.mp4 silent-track park. OA_TEST_HCT_PFS points at
# the game's root.pfs FILE (its real parent directory supplies the loose
# movie_bai/ files). Skips (77) when unset.
add_executable(tm_movie_audio_test tm_movie_audio_test.cpp)
target_link_libraries(tm_movie_audio_test PRIVATE libopenartemis)
add_test(NAME tm_movie_audio COMMAND tm_movie_audio_test)
set_tests_properties(tm_movie_audio PROPERTIES
  ENVIRONMENT "OA_TEST_HCT_PFS=${OA_TEST_HCT_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/88: the wall-clock video driver must not credit idle time into a
# later channel's clocks (real-machine story-head movie started ~16 s in).
# Real-time (driver + decode pool) two-play geometry; skips 77 without the
# archive. Registered after tm_movie_audio, same HCT asset convention.
add_executable(tm_movie_gap_test tm_movie_gap_test.cpp)
target_link_libraries(tm_movie_gap_test PRIVATE libopenartemis)
add_test(NAME tm_movie_gap COMMAND tm_movie_gap_test)
set_tests_properties(tm_movie_gap PROPERTIES
  ENVIRONMENT "OA_TEST_HCT_PFS=${OA_TEST_HCT_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/54: real WMV decode over the NekoMiko boot asset
# movie/logo_emote.wmv (ASF/WMV3) through the optional FFmpeg backend
# (FfmpegSource). Skips (77) when the archive is unset or the engine was
# built without FFmpeg (OA_HAVE_FFMPEG undefined).
add_executable(wmv_decode_test wmv_decode_test.cpp)
target_link_libraries(wmv_decode_test PRIVATE libopenartemis)
add_test(NAME wmv_decode COMMAND wmv_decode_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(wmv_decode PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(wmv_decode PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# P1 (research/42): E-mote PSB parse parity + static pose raster against the
# real tay_0.psb in the NekoMiko archive.
add_executable(emote_static_test emote_static_test.cpp)
target_link_libraries(emote_static_test PRIVATE libopenartemis)
add_test(NAME emote_static COMMAND emote_static_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(emote_static PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(emote_static PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# P1 (research/42): real-chain e:createEmoteLayer -> static layer binding
# through the game Lua VM (NekoMiko archive).
add_executable(emote_chain_test emote_chain_test.cpp)
target_link_libraries(emote_chain_test PRIVATE libopenartemis)
add_test(NAME emote_chain COMMAND emote_chain_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(emote_chain PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(emote_chain PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# P4U (research/46): default text face resolution across real projects
# (NekoMiko ships its own font/ — engine default is an FPM asset).
add_executable(font_face_test font_face_test.cpp)
target_link_libraries(font_face_test PRIVATE libopenartemis Freetype::Freetype)
add_test(NAME font_face COMMAND font_face_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(font_face PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(font_face PROPERTIES
  ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS};OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}"
  SKIP_RETURN_CODE 77 ${_OA_RS})

# P2 (research/43): EmotePlayer timeline/variable playback state machine.
add_executable(emote_playback_test emote_playback_test.cpp)
target_link_libraries(emote_playback_test PRIVATE libopenartemis)
add_test(NAME emote_playback COMMAND emote_playback_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(emote_playback PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(emote_playback PROPERTIES ENVIRONMENT "OA_TEST_NEKOMIKO_PFS=${OA_TEST_NEKOMIKO_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# S3 (research/122): timeline slot model regression — the S3 wrap rule
# (wrap iff loopEnd>0) and the ordered foreground play list, driven straight
# through EmotePlayer on the two new-generation archives whose gesture
# timelines exposed the P0 freeze (甜蜜女友3 / きら☆かの). Both arms run when
# their archive variable is set; the test self-skips (77) when neither is.
add_executable(emote_timeline_test emote_timeline_test.cpp)
target_link_libraries(emote_timeline_test PRIVATE libopenartemis)
add_test(NAME emote_timeline COMMAND emote_timeline_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(emote_timeline PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(emote_timeline PROPERTIES
  ENVIRONMENT "OA_TEST_TG3_PFS=${OA_TEST_TG3_PFS};OA_TEST_SLNY_PFS=${OA_TEST_SLNY_PFS}"
  SKIP_RETURN_CODE 77 ${_OA_RS})

# E9 diagnostic: dumps slant/UD axis keyframes + idle-entry head/body geometry
# (report-only executable; not registered as a ctest).
add_executable(emote_axis_dump emote_axis_dump.cpp)
target_link_libraries(emote_axis_dump PRIVATE libopenartemis)

# Headless emote performance probe (report-only, not a ctest): walks a real
# game archive to an E-mote scene and measures tick/player-level costs.
add_executable(emote_perf_probe emote_perf_probe.cpp)
target_link_libraries(emote_perf_probe PRIVATE libopenartemis SDL3::SDL3 PNG::PNG Freetype::Freetype)

# research/91 diagnostic: btjy (HENPRI) backlog speaker-face geometry probe
# (report-only; not a ctest). Walks the story, opens the backlog and dumps
# row face-subtree geometry vs natural sizes (backlog-face-size report).
add_executable(bt91_backlog_face_probe bt91_backlog_face_probe.cpp)
target_link_libraries(bt91_backlog_face_probe PRIVATE libopenartemis)

# research/88 diagnostic: HCT story journey probe (report-only; not a ctest).
add_executable(hct_journey_probe hct_journey_probe.cpp)
target_link_libraries(hct_journey_probe PRIVATE libopenartemis)

# research/88 diagnostic: HCT movie real-device audio probe (report-only).
add_executable(hct_av_probe hct_av_probe.cpp)
target_link_libraries(hct_av_probe PRIVATE libopenartemis)

# research/118 diagnostic: audio latency distribution probe (report-only; not
# a ctest). Real-time dummy/real device, app-like tick cadence with a "video
# section" of long frames; prints the before/after P50/P95/max of the standing
# audible delay (the queued stream level). Env: OA_LAT_RUNS, OA_LAT_ARM,
# OA_LAT_SEED, OA_LAT_LOAD, SDL_AUDIODRIVER.
add_executable(audio_latency_dist_probe audio_latency_dist_probe.cpp)
target_link_libraries(audio_latency_dist_probe PRIVATE libopenartemis)

# research/125 diagnostic: signal-face capture driver (report-only; not a
# ctest). Drives ONE real BGM asset through the real decode+mix chain with a
# virtual tick clock and writes the produced samples to disk via
# OA_AUDIO_REC=<prefix> for offline analysis (resampling error, fade-in
# staircases, dropouts). Env: OA_AQ_ASSET / OA_AQ_TICK_MS / OA_AQ_SECONDS /
# OA_AQ_FADE_MS / OA_AQ_GAIN / OA_AQ_LOOP / OA_AQ_DEVICE.
add_executable(audio_quality_probe audio_quality_probe.cpp)
target_link_libraries(audio_quality_probe PRIVATE libopenartemis)

# research/100 diagnostic: snll sakura `_m` pair evidence probe (report-only;
# not a ctest). Env: OA_TEST_SNLL_PFS -> snll root.pfs FILE (the loose
# movie/ dir of its parent supplies the ogv pair).
add_executable(sakura_mask_probe sakura_mask_probe.cpp)
target_link_libraries(sakura_mask_probe PRIVATE libopenartemis)

# research/101 diagnostic: snll prologue recap-montage probe (report-only;
# not a ctest). Env: OA_TEST_SNLL_PFS -> snll root.pfs; optional
# OA_SN101_SAVESRC -> warm save root from a windowed run that already picked
# a language (else the first-boot language page stalls the journey).
add_executable(sn101_montage_probe sn101_montage_probe.cpp)
target_link_libraries(sn101_montage_probe PRIVATE libopenartemis)

# research/90 diagnostic: Select-Oblige (天选庶民的真命之选) mw-face/backlog
# offscreen-group scene probe (report-only; not a ctest).
add_executable(so90_face_probe so90_face_probe.cpp)
target_link_libraries(so90_face_probe PRIVATE libopenartemis)


add_executable(folder_start_test folder_start_test.cpp)
target_link_libraries(folder_start_test PRIVATE libopenartemis)
add_test(NAME folder_start_test COMMAND folder_start_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(folder_start_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(folder_start_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(runtime_frames_test runtime_frames_test.cpp)
target_link_libraries(runtime_frames_test PRIVATE libopenartemis)
add_test(NAME runtime_frames_test COMMAND runtime_frames_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(runtime_frames_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(runtime_frames_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(boot_fpm_test boot_fpm_test.cpp)
target_link_libraries(boot_fpm_test PRIVATE libopenartemis)
add_test(NAME boot_fpm_test COMMAND boot_fpm_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(boot_fpm_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(boot_fpm_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# Optional test against the real game archive (validates tool parity);
# skipped (exit 77) unless OA_TEST_FPM_PFS points at fpm/root.pfs.
add_executable(pfs_real_test pfs_real_test.cpp)
target_link_libraries(pfs_real_test PRIVATE libopenartemis)
add_test(NAME pfs_fpm_real COMMAND pfs_real_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(pfs_fpm_real PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(pfs_fpm_real PROPERTIES
  ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}"
  SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(title_drain_test title_drain_test.cpp)
target_link_libraries(title_drain_test PRIVATE libopenartemis)
add_test(NAME title_drain_test COMMAND title_drain_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(title_drain_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(title_drain_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(p3b_body_probe p3b_body_probe.cpp)
target_link_libraries(p3b_body_probe PRIVATE libopenartemis)
add_test(NAME text_p3b_body COMMAND p3b_body_probe)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(text_p3b_body PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(text_p3b_body PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(text_layout_probe text_layout_probe.cpp)
# FontSystem 真实字体度量断言需要 FreeType（R3）。
target_link_libraries(text_layout_probe PRIVATE libopenartemis Freetype::Freetype)
add_test(NAME text_layout_probe COMMAND text_layout_probe)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(text_layout_probe PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(text_layout_probe PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(ux_journey_test ux_journey_test.cpp)
target_link_libraries(ux_journey_test PRIVATE libopenartemis)
add_test(NAME ux_journey_test COMMAND ux_journey_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(ux_journey_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(ux_journey_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(ux_flow_controls_test ux_flow_controls_test.cpp)
target_link_libraries(ux_flow_controls_test PRIVATE libopenartemis)
add_test(NAME ux_flow_controls_test COMMAND ux_flow_controls_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(ux_flow_controls_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(ux_flow_controls_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/83: real-fpm automode voice-sync probe — enables automode via the
# real Lua path (automode_start) in the story body and watches page releases
# against playing voice channels (diagnostic; env-gated like p3b_body_probe).
add_executable(automode_probe automode_probe.cpp)
target_link_libraries(automode_probe PRIVATE libopenartemis)
add_test(NAME automode_probe COMMAND automode_probe)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(automode_probe PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(automode_probe PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(qload_dock_test qload_dock_test.cpp)
target_link_libraries(qload_dock_test PRIVATE libopenartemis)
add_test(NAME qload_dock_test COMMAND qload_dock_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(qload_dock_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(qload_dock_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(save_load_ui_test save_load_ui_test.cpp)
target_link_libraries(save_load_ui_test PRIVATE libopenartemis)
add_test(NAME save_load_ui_test COMMAND save_load_ui_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(save_load_ui_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(save_load_ui_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# L2 M11: real-fpm save->load->walk->backlog regression (dump log.stack across
# every boundary; skip without assets like the other real-fpm drivers).
add_executable(backlog_saveload_test backlog_saveload_test.cpp)
target_link_libraries(backlog_saveload_test PRIVATE libopenartemis lua51)
add_test(NAME backlog_saveload COMMAND backlog_saveload_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(backlog_saveload PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(backlog_saveload PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(char_visual_probe char_visual_probe.cpp)
target_link_libraries(char_visual_probe PRIVATE libopenartemis)
add_test(NAME char_fpm_visual COMMAND char_visual_probe)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(char_fpm_visual PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(char_fpm_visual PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(p1_chain_skip_test p1_chain_skip_test.cpp)
target_link_libraries(p1_chain_skip_test PRIVATE libopenartemis)
add_test(NAME p1_chain_skip COMMAND p1_chain_skip_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(p1_chain_skip PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(p1_chain_skip PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(p3_select_test p3_select_test.cpp)
target_link_libraries(p3_select_test PRIVATE libopenartemis)
add_test(NAME p3_select COMMAND p3_select_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(p3_select PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(p3_select PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/56: FPM "次の選択肢に進む" (dock bt_nsel -> adv_selnext ->
# goNextSelect -> e:debugSkip): engine exskip fast-forward must cross
# chapters and park at the next select (共通-12). Red pre-fix (debugSkip was
# a no-op), green after the runtime exskip protocol (research/56).
add_executable(nsel_jump_test nsel_jump_test.cpp)
target_link_libraries(nsel_jump_test PRIVATE libopenartemis)
add_test(NAME nsel_jump COMMAND nsel_jump_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(nsel_jump PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(nsel_jump PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(u13_wheel_test u13_wheel_test.cpp)
target_link_libraries(u13_wheel_test PRIVATE libopenartemis)
add_test(NAME u13_wheel COMMAND u13_wheel_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(u13_wheel PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(u13_wheel PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r10b_backlog_rows_test r10b_backlog_rows_test.cpp)
target_link_libraries(r10b_backlog_rows_test PRIVATE libopenartemis)
add_test(NAME r10b_backlog_rows_test COMMAND r10b_backlog_rows_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r10b_backlog_rows_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r10b_backlog_rows_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r38p2_dock_hover_test r38p2_dock_hover_test.cpp)
target_link_libraries(r38p2_dock_hover_test PRIVATE libopenartemis)
add_test(NAME r38p2_dock_hover_test COMMAND r38p2_dock_hover_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r38p2_dock_hover_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r38p2_dock_hover_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r10e_preview_recover_test r10e_preview_recover_test.cpp)
target_link_libraries(r10e_preview_recover_test PRIVATE libopenartemis)
add_test(NAME r10e_preview_recover_test COMMAND r10e_preview_recover_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r10e_preview_recover_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r10e_preview_recover_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r10d_reset_title_test r10d_reset_title_test.cpp)
target_link_libraries(r10d_reset_title_test PRIVATE libopenartemis)
add_test(NAME r10d_reset_title_test COMMAND r10d_reset_title_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r10d_reset_title_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r10d_reset_title_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r10_var_barrier_test r10_var_barrier_test.cpp)
target_link_libraries(r10_var_barrier_test PRIVATE libopenartemis)
add_test(NAME r10_var_barrier_test COMMAND r10_var_barrier_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r10_var_barrier_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r10_var_barrier_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

add_executable(r38p1_trans_flip_test r38p1_trans_flip_test.cpp)
target_link_libraries(r38p1_trans_flip_test PRIVATE libopenartemis)
add_test(NAME r38p1_trans_flip_test COMMAND r38p1_trans_flip_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r38p1_trans_flip_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r38p1_trans_flip_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})
# R38 P1: drives the real RenderEngine::process_event host loop headless, so
# font.cpp's FreeType calls must resolve (same as save_thumb_readback_test).
target_link_libraries(r38p1_trans_flip_test PRIVATE Freetype::Freetype)

add_executable(r38p3_tablet_toggle_test r38p3_tablet_toggle_test.cpp)
target_link_libraries(r38p3_tablet_toggle_test PRIVATE libopenartemis)
add_test(NAME r38p3_tablet_toggle_test COMMAND r38p3_tablet_toggle_test)
if(OA_TEST_SERIALIZE_HEAVY)
  set_tests_properties(r38p3_tablet_toggle_test PROPERTIES RUN_SERIAL TRUE)
endif()
set_tests_properties(r38p3_tablet_toggle_test PROPERTIES ENVIRONMENT "OA_TEST_FPM_PFS=${OA_TEST_FPM_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# research/118: engine-level movie-audio bound + deterministic EOF flush (tick
# clock with a real device attached: the producer deliberately outruns the
# device, the worst case for the bound). In-process negative control: the same
# geometry with the bound disabled must carry seconds of standing level.
add_executable(movie_audio_bound_test movie_audio_bound_test.cpp)
target_link_libraries(movie_audio_bound_test PRIVATE libopenartemis)
add_test(NAME movie_audio_bound COMMAND movie_audio_bound_test)
set_tests_properties(movie_audio_bound PROPERTIES ENVIRONMENT "OA_TEST_HCT_PFS=${OA_TEST_HCT_PFS}" SKIP_RETURN_CODE 77 ${_OA_RS})

# Headless video performance probe (report-only, not a ctest).
add_executable(video_perf_probe video_perf_probe.cpp)
target_link_libraries(video_perf_probe PRIVATE libopenartemis SDL3::SDL3 PNG::PNG Freetype::Freetype)

# Memory-churn probe (report-only, not a ctest).
add_executable(mem_churn_probe mem_churn_probe.cpp)
target_link_libraries(mem_churn_probe PRIVATE libopenartemis SDL3::SDL3 PNG::PNG Freetype::Freetype)
if(WIN32)
  target_link_libraries(mem_churn_probe PRIVATE psapi)
endif()

# E-mote stencil/mask structure survey (report-only).
add_executable(emote_mask_dump emote_mask_dump.cpp)
target_link_libraries(emote_mask_dump PRIVATE libopenartemis)
