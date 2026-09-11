@echo off
rem ---------------------------------------------------------------------------
rem openartemis - WebAssembly build (Emscripten)
rem
rem   script\build-wasm.bat
rem
rem Output: build\wasm\src\app\index.html / index.js / index.wasm
rem Local preview: copy next to web\serve.py, serve, then open
rem   http://localhost:8080/index.html?pfs=root.pfs
rem Requires: EMSDK (emsdk with upstream/emscripten installed), VCPKG_ROOT.
rem NOTE: keep this file ASCII-only. cmd.exe parses .bat files in the OEM code
rem page, so UTF-8 text in rem/echo lines makes it emit spurious
rem "'<fragment>' is not recognized" / "syntax is incorrect" errors.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0.."

if "%EMSDK%"=="" (
    echo [warn] EMSDK is not set; vcpkg's wasm32-emscripten triplet needs emcc.
    echo        Run emsdk_env ^(e.g. C:\path\to\emsdk\emsdk_env.bat^) in this shell first.
)

echo [1/2] CMake configure (wasm)...
cmake --preset wasm || goto :fail

echo [2/2] Building index.html / index.js / index.wasm ...
cmake --build build/wasm --config Release -j 8 || goto :fail

echo.
echo Done. Web artifacts:
dir /b build\wasm\src\app\index.*
echo.
echo Preview:
echo   copy build\wasm\src\app\index.* to a folder, put your root.pfs next to them,
echo   then:  python web\serve.py . 8080     --^>  http://localhost:8080/index.html?pfs=root.pfs
exit /b 0

:fail
echo.
echo BUILD FAILED (step above)
exit /b 1
