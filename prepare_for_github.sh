#!/bin/bash

set -e

echo "🚀 Preparing project for GitHub..."

# -----------------------------
# Create directories
# -----------------------------
mkdir -p scripts
mkdir -p assets/fonts

# -----------------------------
# Move platform scripts
# -----------------------------
[ -f build.sh ] && mv build.sh scripts/build_macos.sh
[ -f install_macos.sh ] && mv install_macos.sh scripts/install_macos.sh
[ -f install_windows.ps1 ] && mv install_windows.ps1 scripts/install_windows.ps1

# -----------------------------
# Remove build artifacts
# -----------------------------
echo "🧹 Removing build artifacts..."

rm -rf build
rm -rf CMakeFiles
rm -f CMakeCache.txt
rm -f cmake_install.cmake
rm -f Makefile

find . -name "*.o" -delete
find . -name "*.a" -delete
find . -name "*.dylib" -delete
find . -name "*.so" -delete
find . -name "*.exe" -delete

# ZXing build junk
rm -rf third_party/zxing-cpp/build

# -----------------------------
# Create .gitignore
# -----------------------------
if [ ! -f .gitignore ]; then
cat << 'EOF' > .gitignore
# Build
/build/
/CMakeFiles/
CMakeCache.txt
cmake_install.cmake
Makefile

# Binaries
*.exe
*.dll
*.so
*.dylib
*.a
*.lib
*.o

# IDE
.vscode/
.idea/
*.user
*.suo
*.vcxproj*
*.sln

# OS
.DS_Store
Thumbs.db

# Logs
logs/
EOF
fi

# -----------------------------
# README template
# -----------------------------
if [ ! -f README.md ]; then
cat << 'EOF' > README.md
# Scanner

Cross-platform C++ scanner based on OpenCV, ZXing, ImGui and GLFW.

## Platforms
- macOS
- Windows (Visual Studio + vcpkg)

## Build (Windows)

Requirements:
- Windows 10+
- Visual Studio 2022
- vcpkg

Steps:
1. git clone <repo>
2. vcpkg install opencv zxing-cpp glfw3
3. cmake -B build -S .
4. cmake --build build --config Release
EOF
fi

echo "✅ Project is ready for GitHub."
echo "👉 Next steps:"
echo "   git init"
echo "   git status"
echo "   git add ."
echo "   git commit -m \"Prepare project for GitHub\""