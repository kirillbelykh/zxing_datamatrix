#!/bin/bash
set -e

BUILD_DIR=build

if [ ! -d "$BUILD_DIR" ]; then
  mkdir "$BUILD_DIR"
fi

cd "$BUILD_DIR"

cmake ..
make -j$(sysctl -n hw.ncpu)

echo ""
echo "✅ Сборка завершена"
echo "🚀 Запуск приложения..."
echo ""

./scanner_qt
