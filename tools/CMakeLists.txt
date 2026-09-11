# CLI tools (desktop hosts only — the top-level CMakeLists skips tools/ for the
# Android and Emscripten targets).
#
#   opfs    project archive (PFS) survey/extract — C++ twin of tools/pfs.py
#   psb     E-mote PSB container tool: info/tree/json/motions/sources/icons/
#           extract (atlas PNGs)/render (static pose PNG)
#   asb     Artemis script binary (ASB) tool: info/labels/decode/extract
#   oasave  save-file inspector (numbered SaveData + saveg/system domain maps)
#   fontedge 文字描边环断裂度量（现象 J+，research/126）：同一字形的
#            legacy 4 对角副本 vs 连续描边（FT_Stroker）逐项给数字
#            （缺口数/环宽 min-max-σ/越界裁切），绘制路径共用
#            oa::render::raster_edge_alpha。
#
# Each tool links the engine library and reuses its readers (oa::fs,
# oa::emote, oa::emote, oa::runtime, oa::runtime) — no format logic is
# duplicated in the tools.
add_executable(opfs opfs.cpp)
add_executable(psb psb.cpp)
add_executable(asb asb.cpp)
add_executable(oasave oasave.cpp)
add_executable(fontedge fontedge.cpp)

# fontedge 直接调 FreeType（描边/栅格化在 libopenartemis 内，但该静态库不
# 传递链接 Freetype —— 与 src/app 同样显式链）。
find_package(Freetype CONFIG REQUIRED)
target_link_libraries(fontedge PRIVATE libopenartemis Freetype::Freetype)

foreach(oa_tool opfs psb asb oasave)
  target_link_libraries(${oa_tool} PRIVATE libopenartemis)
  if(MSVC)
    target_compile_options(${oa_tool} PRIVATE /W4)
  else()
    target_compile_options(${oa_tool} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
