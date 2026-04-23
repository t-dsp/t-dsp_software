@echo off
REM Host-runnable test harness for lib/TDspMPE (Windows).
REM Run from this directory: run_tests.bat
REM
REM Requires: g++ on PATH (MinGW / MSYS / WSL g++.exe).

setlocal

if "%CXX%"=="" set CXX=g++
set CXXFLAGS=-std=c++17 -Wall -Wextra -O1

set OUT=test_voice_allocator.exe
set SRC=test_voice_allocator.cpp ..\src\MpeVaSink.cpp
set INC=-I..\src -I.\mocks -I..\..\TDspMidi\src

echo Compiling...
%CXX% %CXXFLAGS% %INC% -o %OUT% %SRC% || exit /b 1

echo Running...
%OUT%
