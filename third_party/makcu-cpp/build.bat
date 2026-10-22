@echo off
setlocal enabledelayedexpansion

REM Build script for makcu-cpp on Windows

echo === MAKCU C++ Windows Build Script ===
echo.

REM Check for required dependencies
echo Checking dependencies...
echo.

REM Check for CMake
cmake --version >nul 2>&1
if errorlevel 1 (
    echo Error: CMake is not installed or not in PATH
    echo Download from: https://cmake.org/download/
    exit /b 1
)
echo [SUCCESS] CMake found

REM Check for Visual Studio or Build Tools and set up environment
where cl >nul 2>&1
if errorlevel 1 (
    echo MSVC compiler not in PATH, attempting to locate Visual Studio...
    
    REM Try to find vswhere.exe (comes with VS 2017+)
    set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!vswhere!" (
        set "vswhere=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    
    if exist "!vswhere!" (
        REM Find the latest Visual Studio installation
        for /f "usebackq tokens=*" %%i in (`"!vswhere!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            set "vsinstall=%%i"
        )
        
        if defined vsinstall (
            echo Found Visual Studio at: !vsinstall!
            REM Set up the environment using vcvarsall.bat
            if exist "!vsinstall!\VC\Auxiliary\Build\vcvarsall.bat" (
                echo Setting up Visual Studio environment...
                call "!vsinstall!\VC\Auxiliary\Build\vcvarsall.bat" x64
                if errorlevel 1 (
                    echo Error: Failed to set up Visual Studio environment
                    exit /b 1
                )
            ) else (
                echo Error: vcvarsall.bat not found in Visual Studio installation
                exit /b 1
            )
        ) else (
            echo Error: Visual Studio with C++ tools not found
            echo Please install Visual Studio with C++ development tools
            exit /b 1
        )
    ) else (
        echo Error: vswhere.exe not found - Visual Studio 2017 or later required
        echo Please install Visual Studio 2017 or later with C++ development tools
        exit /b 1
    )
    
    REM Verify compiler is now available
    where cl >nul 2>&1
    if errorlevel 1 (
        echo Error: MSVC compiler still not found after environment setup
        exit /b 1
    )
)
echo [SUCCESS] MSVC compiler found

echo [SUCCESS] All dependencies found!
echo.

REM Create build directory
echo Creating build directory...
if not exist build mkdir build
cd build

REM Run CMake configuration
echo Configuring project...
cmake .. -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo Error: CMake configuration failed
    exit /b 1
)

REM Build the project
echo Building project...
cmake --build . --config Release --parallel
if errorlevel 1 (
    echo Error: Build failed
    exit /b 1
)

echo.
echo === Build completed successfully! ===
echo Library location: %CD%\lib\Release\

REM Test if libraries were created
set "shared_found="
set "static_found="

if exist "bin\Release\makcu-cpp.dll" (
    echo [SUCCESS] Shared library built successfully
    set "shared_found=1"
)
if exist "lib\Release\makcu-cpp.lib" (
    echo [SUCCESS] Static library built successfully  
    set "static_found=1"
)

if not defined shared_found if not defined static_found (
    echo [ERROR] No libraries found
    exit /b 1
)

echo.
echo To build and run examples:
echo cmake --build . --target install
echo cd ..\examples ^&^& mkdir build ^&^& cd build
echo cmake .. ^&^& cmake --build . --config Release
echo bin\Release\basic_usage.exe  # C++ basic usage
echo bin\Release\demo.exe         # C++ demo
echo bin\Release\c_api_test.exe   # C API test
echo bin\Release\baud_rate_test.exe # Baud rate speed test