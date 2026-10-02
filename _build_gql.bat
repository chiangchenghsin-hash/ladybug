@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0"

REM Force real relinks: MSBuild incremental up-to-date check (especially with
REM -m / multi-target) can skip a link and still print the "proj -> out" line,
REM leaving a stale exe that silently masquerades as a fresh build. Delete the
REM outputs first so a green line below always means a real link happened.
if exist build_v0211t\src\Release\e2e_test.exe del /f /q build_v0211t\src\Release\e2e_test.exe
if exist extension\gql\build\libgql.lbug_extension del /f /q extension\gql\build\libgql.lbug_extension

%CMAKE% -S . -B build_v0211t -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_EXTENSIONS="gql" ^
  -DBUILD_EXTENSION_TESTS=TRUE -DBUILD_TESTS=TRUE ^
  -DBUILD_SHELL=OFF -DBUILD_NODEJS=OFF -DBUILD_PYTHON=OFF -DBUILD_JAVA=OFF -DBUILD_BENCHMARK=OFF ^
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="C:/Users/chian/Documents/trae_projects/ladybug-0.19.0/build_algo/_deps/googletest-src" ^
  > build_gql_configure.log 2>&1
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
echo CONFIGURE_OK

REM Sequential single-target builds (no -m): parallel multi-target is what
REM raced the up-to-date check and skipped the e2e_test link. e2e_test is
REM built LAST so its link is the final word on lbug.lib (the second target's
REM dependency build may re-archive lbug.lib and bump its mtime after the
REM extension DLL linked -- content-identical, mtime-only; that is why the
REM timestamp gate below covers the exe only, and the DLL is existence-only).
%CMAKE% --build build_v0211t --config Release --target lbug_gql_extension > build_gql_build.log 2>&1
if errorlevel 1 ( echo BUILD_FAILED_lbug_gql_extension & exit /b 1 )
%CMAKE% --build build_v0211t --config Release --target e2e_test >> build_gql_build.log 2>&1
if errorlevel 1 ( echo BUILD_FAILED_e2e_test & exit /b 1 )
echo BUILD_EXIT=0

REM Artifact gate: both outputs must exist; e2e_test.exe must not be older
REM than lbug.lib (stale-exe masquerade guard).
powershell -NoProfile -Command "$lib='build_v0211t\src\Release\lbug.lib'; $exe='build_v0211t\src\Release\e2e_test.exe'; $ext='extension\gql\build\libgql.lbug_extension'; foreach ($p in @($exe,$ext)) { if (!(Test-Path $p)) { Write-Host ('ARTIFACT_MISSING ' + $p); exit 1 } }; foreach ($p in @($exe,$ext)) { Write-Host ($p + '  ' + (Get-Item $p).LastWriteTime) }; $l=(Get-Item $lib).LastWriteTime; Write-Host ($lib + '  ' + $l); if ((Get-Item $exe).LastWriteTime -lt $l) { Write-Host ('STALE_EXE ' + $exe + ' older than lbug.lib'); exit 1 }; Write-Host 'ARTIFACTS_OK'"
if errorlevel 1 ( echo ARTIFACT_GATE_FAILED & exit /b 1 )
echo ARTIFACT_GATE_OK
