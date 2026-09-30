@echo off
REM hyperalgo-core standalone test build (MSVC)
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0"
if not exist build_test\CMakeCache.txt (
    %CMAKE% -S . -B build_test -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Release -DHYPERALGO_BUILD_TESTS=TRUE > build_test_configure.log 2>&1
    if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
)
%CMAKE% --build build_test --config Release > build_test_build.log 2>&1
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )
echo BUILD_OK
build_test\Release\hyperalgo_tests.exe
echo TEST_EXIT=%errorlevel%
