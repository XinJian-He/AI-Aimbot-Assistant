// Basic usage example for MAKCU C++ Library
#include <makcu.h>
#include <iostream>

int main() {
    try {
        // Find and connect to device
        auto devices = makcu::Device::findDevices();
        if (devices.empty()) {
            std::cout << "No MAKCU devices found.\n";
            return 1;
        }

        makcu::Device device;
        if (!device.connect(devices[0].port)) {
            std::cout << "Failed to connect to device.\n";
            return 1;
        }

        std::cout << "Connected to device: " << devices[0].port << "\n";
        std::cout << "Firmware version: " << device.getVersion() << "\n";

        // Basic mouse operations
        if (!device.mouseMove(100, 0)) {
            std::cout << "mouseMove(100,0) failed\n";
            return 1;
        }
        if (!device.mouseMove(-100, 0)) {
            std::cout << "mouseMove(-100,0) failed\n";
            return 1;
        }
        if (!device.click(makcu::MouseButton::LEFT)) {
            std::cout << "click(LEFT) failed\n";
            return 1;
        }
        if (!device.mouseWheel(3)) {
            std::cout << "mouseWheel(3) failed\n";
            return 1;
        }
        if (!device.mouseWheel(-3)) {
            std::cout << "mouseWheel(-3) failed\n";
            return 1;
        }

        device.disconnect();
        std::cout << "Demo completed successfully!\n";
    }
    catch (const makcu::MakcuException& e) {
        std::cerr << "MAKCU Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
