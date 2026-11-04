# 模糊测试（OA_ENABLE_FUZZ，见 docs/TESTING.md）。
#
# 编译器是 Clang 时全部编成 libFuzzer（-fsanitize=fuzzer，覆盖率引导 + 自动
# 最小化）；其它编译器退化成 fuzz_loop.h 里的暴力变异循环 —— 那种模式下配
# ASan 一样能砸出内存错误。
function(oa_add_fuzz target source)
  add_executable(${target} ${source} asan_defaults.cpp)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target} PRIVATE libopenartemis ${ARGN})
  if(OA_FUZZ_LIBFUZZER_OK)
    target_compile_definitions(${target} PRIVATE OA_FUZZ_LIBFUZZER=1)
    # libFuzzer 的 main 在运行时库里；Windows 上还要补一层 sanitizer 导出跳板，
    # 否则它找不到 __sanitizer_* 就不会写 crash-* 样本。
    target_sources(${target} PRIVATE sanitizer_exports.cpp)
    target_link_libraries(${target} PRIVATE "${OA_FUZZ_LIBFUZZER_RUNTIME}")
  endif()
endfunction()

# 解析面：一个源文件，编译期宏选输入面（psb / iet / image / save）。
foreach(_t psb iet image save)
  string(TOUPPER "${_t}" _T)
  oa_add_fuzz(oa_fuzz_${_t} oa_fuzz.cpp)
  target_compile_definitions(oa_fuzz_${_t} PRIVATE OA_FUZZ_${_T}=1)
endforeach()

# 核心两块的输入面不是文件而是"命令/剧本"：运行时跑真解释器，渲染跑真栅格化。
oa_add_fuzz(oa_fuzz_runtime oa_fuzz_runtime.cpp)
oa_add_fuzz(oa_fuzz_render oa_fuzz_render.cpp SDL3::SDL3)

# 暴力驱动：真包 + 随机输入，可选隐藏窗口渲染。
add_executable(oa_monkey oa_monkey.cpp)
target_link_libraries(oa_monkey PRIVATE libopenartemis SDL3::SDL3)

# 并发压力：高频起停视频通道，专打驱动线程与主线程的交错（--threads 多实例）。
add_executable(oa_stress oa_stress.cpp)
target_link_libraries(oa_stress PRIVATE libopenartemis)
