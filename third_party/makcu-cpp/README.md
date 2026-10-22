# MAKCU C++ Library

High-performance C++ library for MAKCU mouse controllers. Sub-millisecond response times, cross-platform support with C ABI for multi-language integration.

## Prerequisites

- C++23 compiler (for C++ API)
- C99 compiler (for C API only)
- CMake 3.20+
- MAKCU Device (VID:PID = 1A86:55D3)
- Linux: `libudev-dev`, `pkg-config`

## Building

```bash
git clone https://github.com/k4hvh/makcu-cpp
cd makcu-cpp
mkdir build && cd build
cmake ..
make -j$(nproc)  # Linux
# OR cmake --build . --config Release  # Windows
sudo make install  # Install system-wide
```

## Integration

### C++ API

Add to your CMakeLists.txt:

```cmake
find_package(makcu-cpp REQUIRED)
target_link_libraries(your_app PRIVATE makcu::makcu-cpp)
```

Basic C++ usage:

```cpp
#include <makcu.h>

makcu::Device device;
device.connect();
device.mouseMove(100, 0);
device.click(makcu::MouseButton::LEFT);
```

### C API (for other languages)

The library includes a complete C ABI for easy integration with Python, Rust, Go, C#, and other languages:

```c
#include <makcu_c.h>

makcu_device_t* device = makcu_device_create();
makcu_connect(device, "");
makcu_mouse_move(device, 100, 0);
makcu_mouse_click(device, MAKCU_MOUSE_LEFT);
makcu_device_destroy(device);
```

See `examples/` for complete integration examples.

## Examples

```bash
cd examples && ./build.sh  # Build examples
./build/bin/demo           # Run demo
```

## Performance

- Mouse Movement: ~0.04ms (40μs)
- Button Click: ~0.04ms (40μs)
- 28x faster than Python implementation

## Troubleshooting

**Linux permissions:**

```bash
sudo usermod -a -G dialout $USER
```

**Windows:** Check Device Manager for COM port with VID:PID = 1A86:55D3

## License

GNU GPLv3

## Acknowledgements

- [Makcu Discord Server](https://discord.gg/frvh3P4Qeg) community
- [Makcu Python Library](https://github.com/SleepyTotem/makcu-py-lib) by [SleepyTotem](https://github.com/SleepyTotem)
- [Joonal Salmi](https://github.com/josal52) for his fix to a long-standing bug
