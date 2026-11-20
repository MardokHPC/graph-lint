@echo off
REM Rebuild and rerun the M0 spike on this machine (RTX 2060 SUPER, sm_75).
REM Run it as a single token: cmd /c <repo>\experiments\m0_graph\run.bat
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo vcvars64.bat failed
  exit /b 1
)
set "CUDA_BIN=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin"
set "PATH=%CUDA_BIN%;%PATH%"
pushd "%~dp0"
"%CUDA_BIN%\nvcc.exe" -std=c++17 -O2 -arch=sm_75 -o shape.exe shape.cu
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
shape.exe
popd
