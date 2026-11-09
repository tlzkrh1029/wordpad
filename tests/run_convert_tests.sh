#!/bin/sh
# Builds and runs the converter unit tests natively (no Windows needed).
# Usage: tests/run_convert_tests.sh [output-dir]
set -e
cd "$(dirname "$0")/.."
mkdir -p build-native
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter ${SANITIZE--fsanitize=address,undefined} \
    -fno-omit-frame-pointer -Isrc tests/convert_test.cpp src/convert/*.cpp -o build-native/convert_test
./build-native/convert_test "$@"
