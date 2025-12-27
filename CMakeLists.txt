cmake_minimum_required(VERSION 3.16)
project(ZXingQtScanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# OpenCV
find_package(OpenCV REQUIRED)

# ZXing (Homebrew)
find_library(ZXING_LIB
    NAMES ZXing
    PATHS /opt/homebrew/lib /usr/local/lib
    REQUIRED
)

include_directories(
    /opt/homebrew/include
    /usr/local/include
)

qt_standard_project_setup()

add_executable(scanner_qt
    gui/main_gui.cpp
    gui/main_window.cpp
    core/scanner_core.cpp
)

target_include_directories(scanner_qt PRIVATE
    core
    gui
)

target_link_libraries(scanner_qt
    Qt6::Widgets
    ${OpenCV_LIBS}
    ${ZXING_LIB}
)

# rpath чтобы dylib находился при запуске
set_target_properties(scanner_qt PROPERTIES
    BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib"
)