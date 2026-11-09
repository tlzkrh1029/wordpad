#!/bin/sh
# Builds the converters with sanitizers and feeds them damaged documents.
# Usage: tests/run_convert_fuzz.sh [iterations] [seed]
# The sample documents written by the unit tests are the starting point of every mutation.
set -e
cd "$(dirname "$0")/.."
mkdir -p build-native/fuzz
FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc"
${CXX:-g++} $FLAGS -Wall -Wextra -Wno-unused-parameter tests/convert_test.cpp src/convert/*.cpp -o build-native/convert_test
${CXX:-g++} $FLAGS tests/convert_fuzz.cpp src/convert/*.cpp -o build-native/convert_fuzz
./build-native/convert_test build-native/fuzz > /dev/null
cd build-native/fuzz
ASAN_OPTIONS=detect_leaks=0:max_allocation_size_mb=3000 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ../convert_fuzz "${1:-3000}" "${2:-1}" sample.docx sample.odt sample.rtf
