# Integration Examples

Quick integration guide for adding MAKCU to existing projects. Supports both C++ and C APIs for multi-language integration.

## CMake Integration

**1. Install MAKCU library:**

```bash
cd makcu-cpp && mkdir build && cd build
cmake .. && make && sudo make install
```

**2. Add to your existing CMakeLists.txt:**

```cmake
find_package(makcu-cpp REQUIRED)
target_link_libraries(your_existing_target PRIVATE makcu::makcu-cpp)
```

**3. Use in your code:**

### C++ API

```cpp
#include <makcu.h>

makcu::Device device;
device.connect();
device.mouseMove(100, 0);
```

### C API

```c
#include <makcu_c.h>

makcu_device_t* device = makcu_device_create();
makcu_connect(device, "");
makcu_mouse_move(device, 100, 0);
makcu_device_destroy(device);
```

## Manual Compilation

### C++ Programs

**Linux:**

```bash
g++ -std=c++23 -I/usr/local/include/makcu your_app.cpp -lmakcu-cpp -lpthread -ludev
```

**Windows:** Add include/lib directories and link `makcu-cpp.lib` + `setupapi.lib`

### C Programs

**Linux:**

```bash
gcc -std=c99 -I/usr/local/include your_app.c -lmakcu-cpp -lstdc++ -lpthread -ludev
```

**Windows:**

```bash
gcc -I"C:\Program Files\makcu-cpp\include" your_app.c -L"C:\Program Files\makcu-cpp\lib" -lmakcu-cpp -lstdc++
```

## Bundle with App

Copy library files to your project and link directly:

```cmake
target_link_libraries(your_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmakcu-cpp.so)
```

## Common Issues

- **"makcu.h not found"**: Run `sudo make install`
- **"cannot find -lmakcu-cpp"**: Run `sudo ldconfig`
- **DLL not found (Windows)**: Copy `makcu-cpp.dll` next to exe

## Examples

```bash
mkdir build && cd build
cmake .. && make        # Build all examples
./bin/demo              # C++ demo application
./bin/basic_usage       # C++ basic usage example
./bin/c_api_test        # C API comprehensive test
```

### Language Integration Examples

The C API enables easy integration with other languages:

**Python (ctypes):**

```python
from ctypes import *
lib = CDLL("libmakcu-cpp.so")
device = lib.makcu_device_create()
lib.makcu_connect(device, b"")
lib.makcu_mouse_move(device, 100, 0)
```

**Rust:**

```rust
use std::ffi::CString;
extern "C" {
    fn makcu_device_create() -> *mut std::ffi::c_void;
    fn makcu_connect(device: *mut std::ffi::c_void, port: *const i8) -> i32;
}
```

**Go:**

```go
/*
#cgo LDFLAGS: -lmakcu-cpp -lstdc++
#include <makcu_c.h>
*/
import "C"
device := C.makcu_device_create()
C.makcu_connect(device, C.CString(""))
```
