cmake_minimum_required(VERSION 3.16)
project(zxing_datamatrix_scanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OpenCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

find_package(ZXing 2.3 CONFIG QUIET)
if(NOT TARGET ZXing::ZXing)
    include(FetchContent)
    set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
    set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
    FetchContent_Declare(
        zxing-cpp
        GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
        GIT_TAG v2.3.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(zxing-cpp)
endif()

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
    ZXing::ZXing
    PkgConfig::GLFW
    OpenGL::GL
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(scanner PRIVATE -Wall -Wextra -Wpedantic)
endif()
