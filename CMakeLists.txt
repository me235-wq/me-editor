cmake_minimum_required(VERSION 3.21)
project(VideoEditor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
find_package(Qt6 QUIET COMPONENTS Multimedia)   # optional: live audio preview
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)

# ---- Engine (no UI dependencies): project model, commands, decoding, export ----
add_library(ve_core STATIC
  src/core/Project.cpp
  src/core/Commands.cpp
  src/core/Effects.cpp
  src/core/TextRender.cpp
  src/media/Decoder.cpp
  src/media/AudioMixer.cpp
  src/media/Overview.cpp
  src/media/Proxy.cpp
  src/media/Compositor.cpp
  src/media/FrameServer.cpp
  src/media/Exporter.cpp)
target_include_directories(ve_core PUBLIC src)
target_link_libraries(ve_core PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL PkgConfig::FFMPEG)

# ---- Application ----
add_executable(videoeditor WIN32
  src/main.cpp
  src/ui/MainWindow.cpp
  src/ui/TimelineWidget.cpp
  src/ui/PreviewWidget.cpp
  src/ui/AudioPlayer.cpp)
target_link_libraries(videoeditor PRIVATE ve_core Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent)
if(Qt6Multimedia_FOUND)
  target_compile_definitions(videoeditor PRIVATE VE_HAS_AUDIO)
  target_link_libraries(videoeditor PRIVATE Qt6::Multimedia)
  message(STATUS "Audio preview: enabled (Qt Multimedia found)")
else()
  message(STATUS "Audio preview: DISABLED (install qt6-multimedia-dev to hear audio while editing)")
endif()
install(TARGETS videoeditor RUNTIME DESTINATION bin)

# ---- Headless self-test (model, undo/redo, save/load, decode, export) ----
enable_testing()
add_executable(ve_selftest tests/selftest.cpp)
target_link_libraries(ve_selftest PRIVATE ve_core)
add_test(NAME selftest COMMAND ve_selftest)
set_tests_properties(selftest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")   # GPU section is skipped here; use xvfb-run / a desktop session to include it

# ---- Optional UI smoke test: drives the real window (folder import, track buttons, drag-box select, keyframes).
#      Needs a display or xvfb:  cmake -DVE_BUILD_UI_TESTS=ON ...  then  xvfb-run -a ./build/ve_uismoke
option(VE_BUILD_UI_TESTS "Build the UI smoke test" OFF)
if(VE_BUILD_UI_TESTS)
  add_executable(ve_uismoke tests/smoke.cpp src/ui/MainWindow.cpp src/ui/TimelineWidget.cpp src/ui/PreviewWidget.cpp src/ui/AudioPlayer.cpp)
  target_link_libraries(ve_uismoke PRIVATE ve_core Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent)
  if(Qt6Multimedia_FOUND)
    target_compile_definitions(ve_uismoke PRIVATE VE_HAS_AUDIO)
    target_link_libraries(ve_uismoke PRIVATE Qt6::Multimedia)
  endif()
endif()
