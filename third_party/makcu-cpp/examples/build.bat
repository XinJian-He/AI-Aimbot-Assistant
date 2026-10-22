@echo off
setlocal enabledelayedexpansion

REM MAKCU C++ Library Examples Build Script (Windows)
REM This script shows how to build applications using the MAKCU library

echo === MAKCU C++ Library Examples Build ===
echo.

REM Check if parent library has been built
if not exist "..\build\makcu-cppConfig.cmake" (
    echo Error: Parent library not found!
    echo.
    echo Please build the main library first:
    echo   cd ..
    echo   .\build.bat
    echo.
    echo Then run this script again.
    exit /b 1
)
echo [SUCCESS] Found parent library build

REM Check for CMake
cmake --version >nul 2>&1
if errorlevel 1 (
    echo Error: CMake is not installed or not in PATH
    echo Download from: https://cmake.org/download/
    exit /b 1
)
echo [SUCCESS] CMake found

REM Check for Visual Studio and set up environment
where cl >nul 2>&1
if errorlevel 1 (
    echo Visual Studio compiler not in PATH, attempting to locate Visual Studio...
    
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
            echo Setting up Visual Studio environment...
            call "!vsinstall!\VC\Auxiliary\Build\vcvarsall.bat" x64
            if errorlevel 1 (
                echo Error: Failed to set up Visual Studio environment
                exit /b 1
            )
        ) else (
            echo Error: Visual Studio with C++ tools not found
            echo Please install Visual Studio with C++ development tools
            exit /b 1
        )
    ) else (
        echo Error: Visual Studio 2017 or later required
        exit /b 1
    )
)
echo [SUCCESS] Visual Studio compiler found

REM Build with CMake using Visual Studio
echo Building examples with CMake and Visual Studio...
if not exist build mkdir build
cd build

cmake .. -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo Error: CMake configuration failed
    cd ..
    exit /b 1
)

cmake --build . --config Release --parallel
if errorlevel 1 (
    echo Error: CMake build failed
    cd ..
    exit /b 1
)

REM Check if executables were built successfully
set "basic_found="
set "demo_found="
set "c_api_found="
set "baud_test_found="

if exist "bin\Release\basic_usage.exe" (
    echo [SUCCESS] basic_usage.exe built successfully
    set "basic_found=1"
)
if exist "bin\Release\demo.exe" (
    echo [SUCCESS] demo.exe built successfully
    set "demo_found=1"
)
if exist "bin\Release\c_api_test.exe" (
    echo [SUCCESS] c_api_test.exe built successfully
    set "c_api_found=1"
)
if exist "bin\Release\baud_rate_test.exe" (
    echo [SUCCESS] baud_rate_test.exe built successfully
    set "baud_test_found=1"
)

if not defined basic_found if not defined demo_found if not defined c_api_found if not defined baud_test_found (
    echo Error: No executables found
    cd ..
    exit /b 1
)

REM Copy DLL to output directory for runtime
echo.
echo Copying DLL to output directory...
set "dll_copied="

REM First try to copy from parent build directory (go up two levels from examples/build/)
if exist "..\..\build\bin\Release\makcu-cpp.dll" (
    copy "..\..\build\bin\Release\makcu-cpp.dll" "bin\Release\" >nul 2>&1
    if not errorlevel 1 (
        echo [SUCCESS] DLL copied from parent build directory
        set "dll_copied=1"
    )
)

REM If not found in parent build, try to find it in PATH
if not defined dll_copied (
    echo DLL not found in parent build, checking system PATH...
    where makcu-cpp.dll >nul 2>&1
    if not errorlevel 1 (
        REM Found in PATH, copy it
        for /f "delims=" %%i in ('where makcu-cpp.dll') do (
            copy "%%i" "bin\Release\" >nul 2>&1
            if not errorlevel 1 (
                echo [SUCCESS] DLL copied from system PATH: %%i
                set "dll_copied=1"
                goto :dll_done
            )
        )
    )
)

:dll_done
if not defined dll_copied (
    echo Warning: makcu-cpp.dll not found in parent build or system PATH
    echo You may need to:
    echo 1. Build the parent library: cd .. ^&^& .\build.bat
    echo 2. Or install the library system-wide
    echo 3. Or manually copy makcu-cpp.dll to bin\Release\
)

echo.
echo === Build completed successfully! ===
echo.
if defined dll_copied (
    echo To run the examples (DLL available in output directory^)
) else (
    echo To run the examples (Note - DLL may need to be in PATH or manually copied^)
)
echo   build\bin\Release\basic_usage.exe    # Simple usage example
echo   build\bin\Release\demo.exe           # Full demo with all features  
echo   build\bin\Release\c_api_test.exe     # C API test
echo   build\bin\Release\baud_rate_test.exe # Baud rate speed test
echo.
echo Alternatively, run from the build\bin\Release directory
echo   cd build\bin\Release
echo   basic_usage.exe
echo   demo.exe
echo   c_api_test.exe
echo   baud_rate_test.exe
echo.
echo To use this in your own project
echo 1. Install the MAKCU library
echo 2. Copy the CMakeLists.txt from this examples directory
echo 3. Use find_package(makcu-cpp REQUIRED) and target_link_libraries(your_app PRIVATE makcu::makcu-cpp)