#!/usr/bin/env bash
# Host-runnable test harness for lib/TDspMPE.
# Run from this directory: `bash run_tests.sh`
# On Windows: `run_tests.bat`
#
# Requires: g++ (any recent version with C++17 support).

set -e

cd "$(dirname "$0")"

CXX=${CXX:-g++}
CXXFLAGS="${CXXFLAGS:--std=c++17 -Wall -Wextra -O1}"

OUT="./test_voice_allocator"
SRC="test_voice_allocator.cpp ../src/MpeVaSink.cpp"
INC="-I../src -I./mocks -I../../TDspMidi/src"

echo "Compiling..."
$CXX $CXXFLAGS $INC -o $OUT $SRC

echo "Running..."
$OUT
