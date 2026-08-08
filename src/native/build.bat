@echo off
setlocal enabledelayedexpansion
echo ============================================
echo  cameraSimpleDetect — Build Script
echo  Zero-dependency C++ / Java build
echo ============================================
echo.

:: ────────────────────────────────────────────
:: Check args
:: ────────────────────────────────────────────
if /i "%~1"=="java" goto :BUILD_JAVA
if /i "%~1"=="cpp"   goto :BUILD_CPP
if /i "%~1"=="all"   goto :BUILD_ALL

echo Usage: build.bat [cpp^|java^|all]
echo.
echo   cpp  — Build native C++ executable (needs MSVC Build Tools)
echo   java — Compile Java .class file     (needs JDK 8+)
echo   all  — Build both
echo.
set /p CHOICE="Choose build target (cpp/java/all): "
if /i "!CHOICE!"=="java" goto :BUILD_JAVA
if /i "!CHOICE!"=="all"   goto :BUILD_ALL
goto :BUILD_CPP

:: ────────────────────────────────────────────
:: C++ Build
:: ────────────────────────────────────────────
:BUILD_CPP
echo [1/2] Locating MSVC compiler...

:: Try: cl is already on PATH (Developer Command Prompt)
where cl 2>nul >nul
if %ERRORLEVEL% EQU 0 (
    echo         Found cl.exe on PATH
    goto :CPP_COMPILE
)

:: Try VS 2022
set "VCDIR="
for %%A in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%A\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCDIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%A\VC\Auxiliary\Build\vcvars64.bat"
        goto :CPP_FOUND
    )
)

:: Try VS 2019
for %%A in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%A\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%A\VC\Auxiliary\Build\vcvars64.bat"
        goto :CPP_FOUND
    )
)

echo [ERROR] MSVC Build Tools not found.
echo.
echo Install options:
echo   1. winget install Microsoft.VisualStudio.2022.BuildTools
echo   2. Download from https://visualstudio.microsoft.com/downloads/
echo      Select "Build Tools for Visual Studio 2022"
echo      Install the "Desktop development with C++" workload
echo.
exit /b 1

:CPP_FOUND
echo         Found: !VCDIR!
call "!VCDIR!" x64 >nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to initialize MSVC environment
    exit /b 1
)

:CPP_COMPILE
echo [2/2] Compiling camera_detect.cpp ...
cl /nologo /O2 /EHsc /MT /std:c++17 /W3 ^
   camera_detect.cpp ^
   /Fe:camera_detect.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ============================================
    echo   SUCCESS: camera_detect.exe built!
    echo.
    echo   Run: camera_detect.exe [--port PORT] [--interval MS]
    echo     e.g. camera_detect.exe --port 8787 --interval 2000
    echo ============================================
) else (
    echo [ERROR] Compilation failed.
    exit /b 1
)

if /i "%~1"=="all" goto :BUILD_JAVA
goto :EOF

:: ────────────────────────────────────────────
:: Java Build
:: ────────────────────────────────────────────
:BUILD_JAVA
echo [1/2] Locating JDK...

where javac 2>nul >nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] javac not found.
    echo.
    echo Install options:
    echo   1. winget install EclipseAdoptium.Temurin.21.JDK
    echo   2. Download from https://adoptium.net/
    echo.
    exit /b 1
)

for /f "tokens=*" %%i in ('javac -version 2^>^&1') do echo         %%i

echo [2/2] Compiling CameraDetect.java ...
javac CameraDetect.java

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ============================================
    echo   SUCCESS: CameraDetect.class built!
    echo.
    echo   Run: java CameraDetect [port] [interval]
    echo     e.g. java CameraDetect 8787 2000
    echo ============================================
) else (
    echo [ERROR] Compilation failed.
    exit /b 1
)
goto :EOF

:: ────────────────────────────────────────────
:: Build All
:: ────────────────────────────────────────────
:BUILD_ALL
call :BUILD_CPP
echo.
call :BUILD_JAVA
goto :EOF
