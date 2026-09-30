@echo off
set PATH=C:\emsdk\upstream\emscripten;C:\emsdk\node\24.19.0_64bit\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0"
if not exist build\wasm\CMakeCache.txt (
    cmake -S . -B build\wasm -G Ninja ^
      -DCMAKE_TOOLCHAIN_FILE=C:/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake ^
      -DEMSCRIPTEN_FORCE_COMPILERS=ON ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DBUILD_WASM=TRUE ^
      -DSINGLE_THREADED=TRUE ^
      -DBUILD_TESTS=FALSE -DBUILD_EXTENSION_TESTS=FALSE -DBUILD_EXTENSIONS=OFF ^
      -DBUILD_SHELL=FALSE -DBUILD_NODEJS=OFF -DBUILD_PYTHON=OFF -DBUILD_JAVA=OFF ^
      -DBUILD_BENCHMARK=OFF -DBUILD_WAL_DUMP=OFF ^
      > build_wasm_configure.log 2>&1
    if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
)
echo CONFIGURE_OK
ninja -C build\wasm > build_wasm_ninja.log 2>&1
echo NINJA_EXIT=%errorlevel%
