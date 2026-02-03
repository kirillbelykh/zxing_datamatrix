#!/bin/bash
set -e

echo "🔨 Building scanner..."

g++ main.cpp \
 imgui/imgui.cpp \
 imgui/imgui_draw.cpp \
 imgui/imgui_tables.cpp \
 imgui/imgui_widgets.cpp \
 imgui/backends/imgui_impl_glfw.cpp \
 imgui/backends/imgui_impl_opengl3.cpp \
 -Iimgui \
 -Iimgui/backends \
 -std=c++17 -O2 -Wall \
 $(pkg-config --cflags --libs opencv4 zxing glfw3) \
 -framework OpenGL \
 -o scanner

echo "✅ Build complete"