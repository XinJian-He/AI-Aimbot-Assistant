#include <makcu.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <string_view>

using namespace makcu;

int main() {
    std::cout << "MAKCU Connection and Baud Rate Test\n";
    std::cout << "===================================\n\n";

    try {
        // Find MAKCU device
        auto devices = Device::findDevices();
        if (devices.empty()) {
            std::cout << "Error: No MAKCU device found!\n";
            return 1;
        }

        std::string port = devices[0].port;
        std::cout << "Found MAKCU on port: " << port << "\n";

        // Create device instance
        Device device;
        
        // Connect (automatically switches to 4M baud rate internally)
        std::cout << "Connecting (will automatically switch to 4M baud rate)...\n";
        if (!device.connect(port)) {
            std::cout << "Error: Failed to connect to MAKCU!\n";
            return 1;
        }
        
        std::cout << "Connected successfully at 4M baud rate!\n";
        
        auto is_valid_version = [](std::string_view version) {
            return version.find("km.MAKCU") != std::string_view::npos;
        };

        // Test communication at 4M baud rate
        std::cout << "Testing communication at 4M baud...\n";
        auto version = device.getVersion();
        std::cout << "Version: " << version << "\n\n";
        if (!is_valid_version(version)) {
            std::cout << "Error: invalid version response at 4M baud.\n";
            return 1;
        }
        
        // Test manual baud rate change to a different speed
        std::cout << "Testing manual baud rate change to 2M...\n";
        if (device.setBaudRate(2000000)) {
            std::cout << "Baud rate changed to 2M successfully!\n";
            
            // Test communication at new rate
            std::cout << "Testing communication at 2M baud...\n";
            version = device.getVersion();
            std::cout << "Version: " << version << "\n";
            if (!is_valid_version(version)) {
                std::cout << "Error: invalid version response at 2M baud.\n";
                return 1;
            }
            
            // Change back to 4M
            std::cout << "\nChanging back to 4M baud rate...\n";
            if (device.setBaudRate(4000000)) {
                std::cout << "Successfully changed back to 4M baud rate!\n";
                version = device.getVersion();
                std::cout << "Version: " << version << "\n";
                if (!is_valid_version(version)) {
                    std::cout << "Error: invalid version response after returning to 4M baud.\n";
                    return 1;
                }
            } else {
                std::cout << "Error: failed to change back to 4M baud rate.\n";
                return 1;
            }
        } else {
            std::cout << "Error: failed to change baud rate to 2M!\n";
            return 1;
        }
        
        std::cout << "\nNote: All baud rate changes are temporary and will reset to 115200\n";
        std::cout << "when the MAKCU device is power cycled. The next connection will\n";
        std::cout << "automatically switch back to 4M baud rate.\n";
        
        // Disconnect
        device.disconnect();
        std::cout << "\nDisconnected from MAKCU.\n";
        
    } catch (const MakcuException& e) {
        std::cerr << "MAKCU Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
