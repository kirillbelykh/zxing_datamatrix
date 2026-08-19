cmake_minimum_required(VERSION 3.16)
project(zxing_datamatrix_scanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OpenCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(ZXing REQUIRED IMPORTED_TARGET zxing)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp")
    message(FATAL_ERROR "ImGui is missing. Run: git submodule update --init --recursive")
endif()

add_executable(scanner
    main.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(scanner PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)

target_link_libraries(scanner PRIVATE
    PkgConfig::OpenCV
    PkgConfig::ZXing
    PkgConfig::GLFW
    OpenGL::GL
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(scanner PRIVATE -Wall -Wextra -Wpedantic)
endif()
