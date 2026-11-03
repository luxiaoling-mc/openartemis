# SDL layer (window/events/platform) + entry points. The engine host lives
# in libopenartemis (src/core/runtime/runtime.cpp).
if(EMSCRIPTEN)
  set(OA_HOST_PLATFORM platform/platform_wasm.cpp)
elseif(ANDROID)
  set(OA_HOST_PLATFORM platform/platform_android.cpp)
else()
  set(OA_HOST_PLATFORM platform/platform_desktop.cpp)
endif()
if(ANDROID)
  # Android loads the native side as one shared library (SDLActivity).
  add_library(openartemis SHARED main.cpp ${OA_HOST_PLATFORM})
  set_target_properties(openartemis PROPERTIES OUTPUT_NAME "openartemis")
else()
  add_executable(openartemis main.cpp ${OA_HOST_PLATFORM})
endif()
target_include_directories(openartemis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openartemis PRIVATE libopenartemis SDL3::SDL3)

if(EMSCRIPTEN)
  # Browser shell (web/index.html): IDBFS save mount, big MEMFS archives,
  # pthreads + exception catching for the engine host.
  set_target_properties(openartemis PROPERTIES
    SUFFIX ".html"
    OUTPUT_NAME "index")
  target_link_options(openartemis PRIVATE
    -lidbfs.js
    -sFORCE_FILESYSTEM=1
    -sALLOW_MEMORY_GROWTH=1
    -sNO_EXIT_RUNTIME=1
    -sINVOKE_RUN=0
    -sSTACK_SIZE=4MB
    -sEXPORTED_RUNTIME_METHODS=FS,callMain,ccall,cwrap,UTF8ToString
    --shell-file "${CMAKE_SOURCE_DIR}/web/index.html")
  set_property(TARGET openartemis APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/index.html")
  target_compile_options(openartemis PRIVATE -fexceptions)
  target_link_options(openartemis PRIVATE -fexceptions -sDISABLE_EXCEPTION_CATCHING=0)
  target_link_options(openartemis PRIVATE -sPTHREAD_POOL_SIZE=6)
elseif(ANDROID)
  target_link_libraries(openartemis PRIVATE EGL GLESv3)
  target_link_options(openartemis PRIVATE $<$<CONFIG:Release>:-s>)
  if(OA_ANDROID_JNI_DIR)
    set_target_properties(openartemis PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${OA_ANDROID_JNI_DIR}/${ANDROID_ABI}")
  endif()
endif()

if(MSVC)
  target_compile_options(openartemis PRIVATE /W4)
else()
  target_compile_options(openartemis PRIVATE -Wall -Wextra -Wpedantic)
endif()
