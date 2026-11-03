#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p build

echo "Building terminator-shell..."

g++ -std=c++17 -Wall -Wextra \
    -Iinclude \
    $(find src/shell -name '*.cpp' -print) \
    -o build/terminator-shell \
    -pthread -lrt

echo "Building Terminator GUI..."

g++ -std=c++17 -Wall -Wextra \
    -Iinclude \
    $(find src -name '*.cpp' ! -path 'src/shell/main.cpp' -print) \
    -o build/terminator \
    -lX11 -lutil -pthread -lrt

echo "Build successful. Launching Terminator..."
exec ./build/terminator "$@"