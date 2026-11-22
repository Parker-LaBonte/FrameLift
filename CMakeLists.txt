cmake_minimum_required(VERSION 3.20)
project(FrameLift VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_executable(FrameLift WIN32
  src/main.cpp
  src/capture.cpp
  src/audio.cpp
  src/gpumon.cpp
  src/framegen.cpp
  src/settings.cpp
  src/bench.cpp
  src/nvflow.cpp
  src/nvprofile.cpp
  src/clips.cpp
  res/framelift.rc
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(FrameLift PRIVATE src third_party/nvof ${CMAKE_BINARY_DIR}/generated ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(FrameLift PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
)
configure_file(res/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h)
target_link_libraries(FrameLift PRIVATE
  d3d11 dxgi d3dcompiler mfplat mf mfreadwrite mfuuid ole32 oleaut32 uuid
  pdh cfgmgr32 avrt winmm imm32 dwmapi propsys windowscodecs shell32)

if(MSVC)
  target_compile_options(FrameLift PRIVATE /W3 /utf-8 /permissive-)
  set_property(TARGET FrameLift PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  # MinGW: single self-contained exe
  target_link_options(FrameLift PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()
