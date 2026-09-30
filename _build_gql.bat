@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0"
%CMAKE% -S . -B build_v0211t -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_EXTENSIONS="gql" ^
  -DBUILD_EXTENSION_TESTS=TRUE -DBUILD_TESTS=TRUE ^
  -DBUILD_SHELL=OFF -DBUILD_NODEJS=OFF -DBUILD_PYTHON=OFF -DBUILD_JAVA=OFF -DBUILD_BENCHMARK=OFF ^
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="C:/Users/chian/Documents/trae_projects/ladybug-0.19.0/build_algo/_deps/googletest-src" ^
  > build_gql_configure.log 2>&1
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
echo CONFIGURE_OK
%CMAKE% --build build_v0211t --config Release --target e2e_test lbug_gql_extension -- -m > build_gql_build.log 2>&1
echo BUILD_EXIT=%errorlevel%
