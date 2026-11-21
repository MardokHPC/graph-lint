@echo off
rem ---------------------------------------------------------------------------
rem Host-only build: no CUDA toolkit, no GPU, no nvcc.
rem
rem Toolchain: Visual Studio 2022 BuildTools + Ninja. This is the configuration
rem the host-only CI job runs on ubuntu-22.04 / windows-2022, and it is the one
rem that proves the core is testable without a GPU.
rem
rem Usage:  build-host.bat <configure|build|test|clean|artifacts> [build-dir]
rem    configure   configure build-host (or the given directory)
rem    build       build it
rem    test        ctest --output-on-failure
rem    clean       delete the build directory (a from-scratch build + test)
rem    artifacts   rewrite graphs/*.dot and graphs/*.mmd from graphs/*.json
rem ---------------------------------------------------------------------------
setlocal
if "%~1"=="" goto usage

set "BUILD=%~2"
if "%BUILD%"=="" set "BUILD=build-host"
rem A relative build dir resolves against the repo root, not the caller's cwd.
if not "%BUILD:~1,1%"==":" set "BUILD=%~dp0%BUILD%"

call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if /i "%~1"=="clean" goto clean
if /i "%~1"=="configure" goto configure
if /i "%~1"=="build" goto build
if /i "%~1"=="test" goto test
if /i "%~1"=="artifacts" goto artifacts
goto usage

:configure
cmake -S "%~dp0." -B "%BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DGRAPHLINT_BUILD_TESTS=ON ^
    -DGRAPHLINT_WITH_CUDA=OFF
exit /b %errorlevel%

:build
cmake --build "%BUILD%"
exit /b %errorlevel%

:test
ctest --test-dir "%BUILD%" --output-on-failure --timeout 600
exit /b %errorlevel%

:clean
if exist "%BUILD%" rmdir /s /q "%BUILD%"
exit /b 0

:artifacts
rem From the repo root, so the artifacts land in graphs/ next to their JSON.
cd /d "%~dp0"
"%BUILD%\emit_artifacts.exe" --out graphs
exit /b %errorlevel%

:usage
echo Usage: %~nx0 ^<configure^|build^|test^|clean^|artifacts^> [build-dir]
exit /b 2
