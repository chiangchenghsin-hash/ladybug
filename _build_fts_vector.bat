@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
echo [1/2] configure BUILD_EXTENSIONS=algo;timeseries;fts;vector ...
%CMAKE% -S . -B build_v0211 -DBUILD_EXTENSIONS=algo;timeseries;fts;vector
if errorlevel 1 exit /b 1
echo [2/2] build algo/fts/timeseries/vector extensions + shell ...
%CMAKE% --build build_v0211 --config Release --target lbug_algo_extension lbug_fts_extension lbug_timeseries_extension lbug_vector_extension lbug_shell
if errorlevel 1 exit /b 1
echo BUILD_OK
