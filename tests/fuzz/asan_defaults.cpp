// ASan / UBSan 的默认选项：不设环境变量也能得到想要的行为。
//
// 这两个函数必须 **导出**：ASan 运行时是用
//   GetProcAddress(GetModuleHandle(NULL), "__asan_default_options")
// 在可执行文件自己的导出表里找它的。llvm-mingw 链的是动态 ASan 运行时，不导出
// 就完全等于没写 —— 现象是 libFuzzer 枚举语料时被 libc++ 容器注解误报成
// container-overflow。
//
// detect_container_overflow=0：那个误报在 libFuzzer 的 FuzzerIOWindows.cpp 里，
// 跟被测代码无关。detect_leaks=0：引擎有进程级缓存、宿主不刻意回收，泄漏报告
// 只会淹没真问题。stdout/stderr 不动：ASan 报告必须能到 stderr。
extern "C" {

#if defined(_WIN32)
#define OA_SAN_EXPORT __declspec(dllexport)
#else
#define OA_SAN_EXPORT __attribute__((visibility("default")))
#endif

OA_SAN_EXPORT const char* __asan_default_options() {
    return "detect_leaks=0:abort_on_error=1:detect_container_overflow=0";
}

OA_SAN_EXPORT const char* __ubsan_default_options() {
    return "print_stacktrace=1";
}
}
