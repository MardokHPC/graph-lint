@echo off
rem ---------------------------------------------------------------------------
rem Device build: CUDA 12.6 (nvcc) + Visual Studio 2022 BuildTools (MSVC 14.44).
rem
rem Two toolchains on one machine and no PATH surgery: vcvars64 from the
rem BuildTools installation is called here, and CMAKE_CUDA_COMPILER is pinned to
rem the 12.6 toolkit so the build cannot silently pick up the 12.1 first on PATH.
rem
rem NVCC_PREPEND_FLAGS=--allow-unsupported-compiler: CUDA 12.6 does not know
rem MSVC 14.44 and refuses to compile the host code without it. Safe here because
rem the host code is also compiled by /W4 /WX through -Xcompiler (CMakeLists.txt).
rem
rem Today -DGRAPHLINT_WITH_CUDA=ON stops the configure with "CUDA source lands in
rem M2": expected, not a broken script.
rem
rem Usage:  build-device.bat <configure|build|test|clean> [build-dir]
rem ---------------------------------------------------------------------------
setlocal
if "%~1"=="" goto usage

set "NVCC_PREPEND_FLAGS=--allow-unsupported-compiler"
set "BUILD=%~2"
if "%BUILD%"=="" set "BUILD=build-dev"
rem A relative build dir resolves against the repo root, not the caller's cwd.
if not "%BUILD:~1,1%"==":" set "BUILD=%~dp0%BUILD%"

set "CUDA_NVCC=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin\nvcc.exe"

call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if /i "%~1"=="clean" goto clean
if /i "%~1"=="configure" goto configure
if /i "%~1"=="build" goto build
if /i "%~1"=="test" goto test
goto usage

:configure
cmake -S "%~dp0." -B "%BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_CUDA_COMPILER=%CUDA_NVCC%" ^
    -DCMAKE_CUDA_ARCHITECTURES=75 ^
    -DGRAPHLINT_BUILD_TESTS=ON ^
    -DGRAPHLINT_WITH_CUDA=ON
exit /b %errorlevel%

:build
cmake --build "%BUILD%"
exit /b %errorlevel%

:test
ctest --test-dir "%BUILD%" --output-on-failure --timeout 900
exit /b %errorlevel%

:clean
if exist "%BUILD%" rmdir /s /q "%BUILD%"
exit /b 0

:usage
echo Usage: %~nx0 ^<configure^|build^|test^|clean^> [build-dir]
exit /b 2
