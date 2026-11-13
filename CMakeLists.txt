cmake_minimum_required(VERSION 3.17)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

project(scc-synth VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(SCC_COPY_AFTER_BUILD "Copy .clap next to build and into LocalAppData CLAP folder" ON)

add_subdirectory(libs/clap EXCLUDE_FROM_ALL)
add_subdirectory(libs/clap-helpers EXCLUDE_FROM_ALL)

# readerwriterqueue is header-only; provide an interface target if missing
if(NOT TARGET readerwriterqueue)
  add_library(readerwriterqueue INTERFACE)
  target_include_directories(readerwriterqueue INTERFACE libs/readerwriterqueue)
endif()

set(IMGUI_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/imgui)
add_subdirectory(libs/clap-imgui-support EXCLUDE_FROM_ALL)

# emu2212 chip emulator (copied)
add_library(emu2212 STATIC third_party/emu2212/emu2212.c)
target_include_directories(emu2212 PUBLIC third_party/emu2212)

add_library(emu2149 STATIC third_party/emu2149/emu2149.c)
target_include_directories(emu2149 PUBLIC third_party/emu2149)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO)

# VGM register player (vendored from VGM-editor_Cursor; local seek/arm APIs)
set(VGMPSG_AY_CLOCK_HZ 1789772)
set(VGMPSG_OUTPUT_RATE 44100)
set(VGMPSG_VGM_WAIT_RATE 44100)
add_library(vgmpsg STATIC
  vendor/vgm-player/src/vgm_cmd.c
  vendor/vgm-player/src/vgm_file.c
  vendor/vgm-player/src/vgm_player.c
  vendor/vgm-player/src/vgm_period_smooth.c
  vendor/vgm-player/src/vgm_scc.c
)
target_include_directories(vgmpsg PUBLIC
  vendor/vgm-player/include
  vendor/vgm-player/src
  third_party/miniz
)
target_compile_definitions(vgmpsg PUBLIC
  VGMPSG_AY_CLOCK_HZ=${VGMPSG_AY_CLOCK_HZ}
  VGMPSG_OUTPUT_RATE=${VGMPSG_OUTPUT_RATE}
  VGMPSG_VGM_WAIT_RATE=${VGMPSG_VGM_WAIT_RATE}
)
target_link_libraries(vgmpsg PUBLIC emu2149 emu2212 miniz)

# SCC chip synth core (copied from SCC-synth)
add_library(scc_chip STATIC
  vendor/scc-synth/src/scc_synth.c
  vendor/scc-synth/src/vgm_scc.c
)
target_include_directories(scc_chip PUBLIC
  vendor/scc-synth/include
  vendor/scc-synth/src
  third_party/emu2212
)
target_link_libraries(scc_chip PUBLIC emu2212)

add_library(${PROJECT_NAME} MODULE
  src/plugin/plugin_entry.cpp
  src/plugin/scc_plugin.cpp
  src/gui/scc_editor.cpp
  src/dsp/adsr.cpp
  src/dsp/modulation.cpp
  src/dsp/poly_voice.cpp
  src/dsp/morph_bank.cpp
  src/dsp/morph_player.cpp
  src/dsp/engine.cpp
  src/patch/patch_format.cpp
  src/patch/patch_io_win.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  src/dsp
  src/patch
  src/plugin
  src/gui
  third_party
  libs/readerwriterqueue
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  clap
  clap-helpers
  readerwriterqueue
  imgui-clap-support
  scc_chip
  d3d12.lib
  dxgi.lib
  d3dcompiler.lib
  dxguid.lib
  comdlg32.lib
)

target_compile_definitions(${PROJECT_NAME} PRIVATE IS_WIN=1 _CRT_SECURE_NO_WARNINGS)
set_target_properties(${PROJECT_NAME} PROPERTIES
  SUFFIX ".clap"
  PREFIX ""
  OUTPUT_NAME "scc-synth"
)

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W3 /EHsc)
  target_compile_options(scc_chip PRIVATE /W3)
endif()

# SCC VGM — separate CLAP (one VGM channel per instance)
add_library(scc-vgm MODULE
  src/vgm_plugin/plugin_entry_vgm.cpp
  src/vgm_plugin/scc_vgm_plugin.cpp
  src/vgm_plugin/scc_vgm_editor.cpp
  src/vgm_plugin/vgm_file_dialog.cpp
  src/dsp/vgm_scc_engine.cpp
)
target_include_directories(scc-vgm PRIVATE
  src
  src/dsp
  src/vgm_plugin
  third_party
  libs/readerwriterqueue
  vendor/vgm-player/include
  vendor/vgm-player/src
)
set(SNIFFER_PNG "${CMAKE_CURRENT_SOURCE_DIR}/src/vgm_plugin/sniffer.png")
set(RUBIKON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/src/vgm_plugin/rubikonlab-logo.png")
string(REPLACE "\\" "/" SNIFFER_PNG_SLASH "${SNIFFER_PNG}")
string(REPLACE "\\" "/" RUBIKON_PNG_SLASH "${RUBIKON_PNG}")
configure_file(src/vgm_plugin/sniffer.rc.in ${CMAKE_CURRENT_BINARY_DIR}/sniffer.rc @ONLY)

target_sources(scc-vgm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sniffer.rc)
target_link_libraries(scc-vgm PRIVATE
  clap
  clap-helpers
  readerwriterqueue
  imgui-clap-support
  vgmpsg
  d3d12.lib
  dxgi.lib
  d3dcompiler.lib
  dxguid.lib
  comdlg32.lib
  windowscodecs.lib
  ole32.lib
)
target_compile_definitions(scc-vgm PRIVATE IS_WIN=1 _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(scc-vgm PROPERTIES
  SUFFIX ".clap"
  PREFIX ""
  OUTPUT_NAME "scc-vgm"
)
if(MSVC)
  target_compile_options(scc-vgm PRIVATE /W3 /EHsc)
  target_compile_options(vgmpsg PRIVATE /W3)
endif()

if(SCC_COPY_AFTER_BUILD)
  set(_clap_dst "$ENV{LOCALAPPDATA}/Programs/Common/CLAP")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/scc-synth/voices/scc-voices.json"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/scc-voices.json"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/install-clap.ps1"
            -ClapPath "$<TARGET_FILE:${PROJECT_NAME}>"
            -VoicesPath "${CMAKE_SOURCE_DIR}/vendor/scc-synth/voices/scc-voices.json"
            -DestDir "${_clap_dst}"
  )
  add_custom_command(TARGET scc-vgm POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/install-scc-vgm.ps1"
            -ClapPath "$<TARGET_FILE:scc-vgm>"
            -DestDir "${_clap_dst}"
  )
endif()
