#!/bin/bash

# MAKCU C++ Library Examples Build Script
# This script shows how to build applications using the MAKCU library

set -e

echo "=== MAKCU C++ Library Examples Build ==="
echo

# Build with CMake (recommended approach)
echo "Building examples with CMake..."
mkdir -p build && cd build
cmake ..
make -j$(nproc)

if [ -f "bin/basic_usage" ] && [ -f "bin/demo" ]; then
    echo "✓ Build successful!"
    echo
    echo "To run the examples:"
    echo "  ./build/bin/basic_usage    # Simple usage example"
    echo "  ./build/bin/demo           # Full demo with all features"
    echo "  ./build/bin/c_api_test     # C API test"
    echo "  ./build/bin/baud_rate_test # Baud rate speed test"
    echo
    echo "To use this in your own project:"
    echo "1. Install the MAKCU library: cd .. && mkdir build && cd build && cmake .. && make && sudo make install"
    echo "2. Copy the CMakeLists.txt from this examples directory"
    echo "3. Use: find_package(makcu-cpp REQUIRED) and target_link_libraries(your_app PRIVATE makcu::makcu-cpp)"
else
    echo "✗ Build failed"
    echo "Make sure the MAKCU library is installed or available via CMAKE_PREFIX_PATH"
    exit 1
fi