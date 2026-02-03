#!/usr/bin/env bash
set -e

echo "▶ ZXing Scanner installer (macOS)"

# 1. Homebrew
if ! command -v brew &>/dev/null; then
  echo "❌ Homebrew not found. Installing..."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

brew update

# 2. Dependencies
echo "📦 Installing dependencies..."
brew install \
  opencv \
  zxing-cpp \
  glfw \
  pkg-config

# 3. Build
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

echo "✅ Done!"
echo "▶ Run with: ./scanner"