// 把 libFuzzer 需要的 sanitizer 接口从 ASan 运行时 DLL 转发出来。
//
// llvm-mingw 的 clang 链的是**动态** ASan 运行时（libclang_rt.asan_dynamic-
// x86_64.dll），而 libFuzzer 是用
//   GetProcAddress(GetModuleHandle(NULL), "__sanitizer_...")
// 在**可执行文件自己的导出表**里找这些符号。导入符号不会被再导出，所以不补这层
// 跳板的话 libFuzzer 只会打印
//   WARNING: Failed to find function "__sanitizer_..."
// 并且永远不写 crash-* 样本 —— 夜里砸出崩溃也拿不到复现文件。
#include <windows.h>

extern "C" {
typedef void (*oa_death_callback)(void);

static HMODULE asan_runtime() {
    static HMODULE module = GetModuleHandleA("libclang_rt.asan_dynamic-x86_64.dll");
    return module;
}

__declspec(dllexport) int __sanitizer_acquire_crash_state(void) {
    typedef int (*fn_t)(void);
    static fn_t fn = reinterpret_cast<fn_t>(
        GetProcAddress(asan_runtime(), "__sanitizer_acquire_crash_state"));
    return fn ? fn() : 0;
}

__declspec(dllexport) void __sanitizer_print_stack_trace(void) {
    typedef void (*fn_t)(void);
    static fn_t fn = reinterpret_cast<fn_t>(
        GetProcAddress(asan_runtime(), "__sanitizer_print_stack_trace"));
    if (fn) fn();
}

__declspec(dllexport) void __sanitizer_set_death_callback(oa_death_callback cb) {
    typedef void (*fn_t)(oa_death_callback);
    static fn_t fn = reinterpret_cast<fn_t>(
        GetProcAddress(asan_runtime(), "__sanitizer_set_death_callback"));
    if (fn) fn(cb);
}
}
