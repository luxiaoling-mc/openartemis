@echo off
rem ---------------------------------------------------------------------------
rem openartemis - Android APK build (arm64-v8a)
rem
rem   script\build-android.bat [debug^|release]
rem
rem Single-stage: Gradle drives CMake/NDK through externalNativeBuild to build
rem libopenartemis.so and package the APK (see android/app/build.gradle).
rem AGP keeps the unstripped native symbol tables, so Android Studio / lldb can
rem attach for native debugging.
rem
rem Requires: VCPKG_ROOT, ANDROID_NDK_HOME (NDK r28), ANDROID_HOME (SDK), JDK 17.
rem NOTE: keep this file ASCII-only. cmd.exe parses .bat files in the OEM code
rem page, so UTF-8 CJK text in echo lines corrupts the following lines.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0.."

set VARIANT=%1
if "%VARIANT%"=="" set VARIANT=debug

echo [1/2] Gradle assemble%VARIANT% (drives the CMake native build)...
cd android
if /i "%VARIANT%"=="release" (
    call gradlew.bat assembleRelease --console=plain || goto :fail
) else (
    call gradlew.bat assembleDebug --console=plain || goto :fail
)
cd ..

echo.
echo Done. APK:
dir /b /s build\android-gradle\app\outputs\apk\*.apk
echo.
echo   debug   = signed with the debug keystore, ready for: adb install
echo   release = unsigned (minify+shrink); sign with:
echo     zipalign -p 4 in.apk aligned.apk ^&^& apksigner sign --ks %USERPROFILE%\.android\debug.keystore aligned.apk
echo.
echo Native debugging: open the android\ directory in Android Studio, pick the
echo app configuration with LLDB. Symbol tables live under
echo   build\android-gradle\app\intermediates\cxx\Debug\*\obj\arm64-v8a\
exit /b 0

:fail
echo.
echo BUILD FAILED (step above)
exit /b 1
