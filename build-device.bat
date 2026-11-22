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
rem Two options on purpose: GRAPHLINT_WITH_CUDA adds the header-only reader and
rem cudart; GRAPHLINT_BUILD_DEVICE_TESTS adds nvcc, the demo and the GPU suite.
rem GRAPHLINT_BUILD_BENCH adds nvcc, tools/bench.cu, tools/weights.cu and cuda-timer
rem (FetchContent). GRAPHLINT_BUILD_EXAMPLES adds nvcc and examples/three_lines.cu.
rem
rem Usage:  build-device.bat <configure|build|demo|bench|example|test|clean> [build-dir]
rem    demo        run demo_graph, which rewrites graphs/*.json in the source tree
rem    bench       configure with the benchmark on, build bench + weights, write
rem                docs/bench/bench.json, measured.json and weights.json
rem    example     configure with the examples on, build and run example_three_lines
rem ---------------------------------------------------------------------------
setlocal
if "%~1"=="" goto usage

set "NVCC_PREPEND_FLAGS=--allow-unsupported-compiler"
set "BUILD=%~2"
if "%BUILD%"=="" set "BUILD=build-dev"
rem A relative build dir resolves against the repo root, not the caller's cwd.
if not "%BUILD:~1,1%"==":" set "BUILD=%~dp0%BUILD%"

set "CUDA_HOME=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.6"
set "CUDA_NVCC=%CUDA_HOME%\bin\nvcc.exe"
set "BENCH_FLAG="
set "EXAMPLE_FLAG="

call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if /i "%~1"=="clean" goto clean
if /i "%~1"=="configure" goto configure
if /i "%~1"=="build" goto build
if /i "%~1"=="demo" goto demo
if /i "%~1"=="bench" goto bench
if /i "%~1"=="example" goto example
if /i "%~1"=="test" goto test
goto usage

:configure
call :configure_cmake
exit /b %errorlevel%

:bench
set "BENCH_FLAG=-DGRAPHLINT_BUILD_BENCH=ON"
rem 12.1 comes first on PATH here; the numbers must come from the toolkit the binary
rem was compiled against, the same reason CMAKE_CUDA_COMPILER is pinned.
set "PATH=%CUDA_HOME%\bin;%PATH%"
call :configure_cmake
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" --target bench weights
if errorlevel 1 exit /b 1
rem From the repo root: the default --out path is relative to the caller's cwd.
cd /d "%~dp0"
"%BUILD%\bench.exe"
if errorlevel 1 exit /b 1
rem The producer of docs/bench/measured.json and docs/bench/weights.json.
"%BUILD%\weights.exe"
exit /b %errorlevel%

:example
set "EXAMPLE_FLAG=-DGRAPHLINT_BUILD_EXAMPLES=ON"
call :configure_cmake
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" --target example_three_lines
if errorlevel 1 exit /b 1
"%BUILD%\example_three_lines.exe"
exit /b %errorlevel%

:configure_cmake
cmake -S "%~dp0." -B "%BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_CUDA_COMPILER=%CUDA_NVCC%" ^
    -DCMAKE_CUDA_ARCHITECTURES=75 ^
    -DGRAPHLINT_BUILD_TESTS=ON ^
    -DGRAPHLINT_WITH_CUDA=ON ^
    -DGRAPHLINT_BUILD_DEVICE_TESTS=ON ^
    %BENCH_FLAG% ^
    %EXAMPLE_FLAG%
exit /b %errorlevel%

:build
cmake --build "%BUILD%"
exit /b %errorlevel%

:demo
"%BUILD%\demo_graph.exe"
exit /b %errorlevel%

:test
ctest --test-dir "%BUILD%" --output-on-failure --timeout 900
exit /b %errorlevel%

:clean
if exist "%BUILD%" rmdir /s /q "%BUILD%"
exit /b 0

:usage
echo Usage: %~nx0 ^<configure^|build^|demo^|bench^|example^|test^|clean^> [build-dir]
exit /b 2
