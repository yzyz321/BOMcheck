cmake_minimum_required(VERSION 3.20)
project(BOMCheck VERSION 4.2.0 LANGUAGES C CXX)

# 版本号只在这里出现；同时注入 gen/bs_version_gen.h 供 C 代码与 .rc 使用
set(BS_VERSION "${PROJECT_VERSION}")
set(BS_EXE     "BOMCheck_V${BS_VERSION}")

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "BOMCheck 仅支持 Windows（Win10 1809+ / 11）")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── 生成头与清单 ────────────────────────────────────────────────
configure_file(src/res/bs_version_gen.h.in  "${CMAKE_BINARY_DIR}/gen/bs_version_gen.h"  @ONLY)
configure_file(src/res/app.manifest.in      "${CMAKE_BINARY_DIR}/gen/app.manifest"      @ONLY)

# MinGW：windres 需要自己找 windows.h，且要能 include 到生成目录
if(MINGW)
  get_filename_component(_mingw_bin  "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_mingw_root "${_mingw_bin}" DIRECTORY)
  set(_mingw_inc "${_mingw_root}/x86_64-w64-mingw32/include")
  set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I\"${_mingw_inc}\" -I\"${CMAKE_BINARY_DIR}/gen\"")
endif()

# ── core：纯 C99，不 include <windows.h>，可直接单测 ──────────────
add_library(bscore STATIC
  src/core/bs_version.c
  src/core/bs_file.c
  src/core/bs_inflate.c
  src/core/bs_deflate.c
  src/core/bs_zip.c
  src/core/bs_xml.c
  src/core/bs_xlsx.c
  src/core/bs_pn.c
  src/core/bs_arena.c
  src/core/bs_sheet.c
  src/core/bs_pair.c
  src/core/bs_zipw.c
  src/core/bs_write.c
  src/core/bs_mark.c
)
target_include_directories(bscore PUBLIC
  src/core
  "${CMAKE_BINARY_DIR}/gen"
)
target_compile_definitions(bscore PUBLIC BS_VERSION="${BS_VERSION}")

# ── ui：C++17 + Win32 ──────────────────────────────────────────
add_executable(${BS_EXE} WIN32
  src/ui/main.cpp
  src/ui/app.cpp
  src/ui/bench.cpp
  src/ui/card.cpp
  src/ui/table.cpp
  src/ui/table_src.cpp
  src/ui/drop.cpp
  src/ui/layout.cpp
  src/ui/layout_doc.cpp
  src/ui/paint.cpp
  src/ui/dpi.cpp
  src/ui/selfcheck.cpp
  src/ui/utf8.cpp
  src/ui/xlsx_dump.cpp
  src/res/app.rc
)
target_include_directories(${BS_EXE} PRIVATE src/ui "${CMAKE_BINARY_DIR}/gen")
target_link_libraries(${BS_EXE} PRIVATE bscore
  comctl32 comdlg32 shell32 ole32 uuid msimg32 uxtheme version)

if(MINGW)
  # 必须配 -DWINVER=0x0A00，否则 GetDpiForWindow / GetThreadDpiAwarenessContext 声明不出来
  target_compile_definitions(${BS_EXE} PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  target_compile_options(${BS_EXE} PRIVATE
    -municode -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

  set(_spec "${CMAKE_SOURCE_DIR}/cmake/mingw-no-default-manifest.spec")
  target_link_options(${BS_EXE} PRIVATE
    -municode
    -static -static-libgcc -static-libstdc++
    -Wl,--gc-sections -Wl,-s
    "-specs=${_spec}")
endif()

# ── 自检目标：不弹窗地报告 DPI / 版本，供脚本验证 ────────────────
enable_testing()
add_test(NAME version COMMAND ${BS_EXE} --version)
add_test(NAME dpi     COMMAND ${BS_EXE} --probe-dpi)
add_test(NAME self    COMMAND ${BS_EXE} --self-check)

# ── core 单测：不链接 Win32 上层，只跑纯函数 / 读取链 ─────────────
add_executable(bstest tests/test_core.c)
target_link_libraries(bstest PRIVATE bscore)
add_test(NAME core COMMAND bstest)
