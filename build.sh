#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p build

echo "Building Terminator..."

g++ -std=c++17 -Wall -Wextra \
    -Iinclude \
    $(find src -name '*.cpp' -print) \
    -o build/terminator \
    -lX11 -lutil

echo "Build successful. Launching Terminator..."
exec ./build/terminator
