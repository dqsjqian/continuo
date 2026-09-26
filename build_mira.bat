@echo off
rem TEMPORARY local helper: build the Mira repo (ex-continuo) under MSVC.
call "D:\worksoft\VS2026\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\Coding\continuo
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug || exit /b 1
"C:\Program Files\CMake\bin\cmake.exe" --build build --parallel 8 || exit /b 1
"C:\Program Files\CMake\bin\ctest.exe" --test-dir build --output-on-failure --no-tests=error
