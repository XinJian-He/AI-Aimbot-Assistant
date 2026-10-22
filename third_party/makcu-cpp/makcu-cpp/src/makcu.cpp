#include "../include/makcu.h"
#include "../include/serialport.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace makcu {

    namespace {
        bool equalsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs) {
            if (lhs.size() != rhs.size()) {
                return false;
            }

            for (size_t i = 0; i < lhs.size(); ++i) {
                if (std::toupper(static_cast<unsigned char>(lhs[i])) !=
                    std::toupper(static_cast<unsigned char>(rhs[i]))) {
                    return false;
                }
            }

            return true;
        }

        std::optional<uint8_t> parseUint8Decimal(std::string_view valueText) {
            int parsedValue = 0;
            const char* begin = valueText.data();
            const char* end = begin + valueText.size();
            const auto [parseEnd, parseErr] = std::from_chars(begin, end, parsedValue);
            if (parseErr != std::errc{} || parseEnd != end) {
                return std::nullopt;
            }

            if (parsedValue < 0 || parsedValue > (std::numeric_limits<uint8_t>::max)()) {
                return std::nullopt;
            }

            return static_cast<uint8_t>(parsedValue);
        }

        std::string escapeSingleQuotedCommandString(std::string_view value) {
            constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

            std::string escaped;
            escaped.reserve(value.size());

            for (const unsigned char ch : value) {
                switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\'':
                    escaped += "\\'";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if (std::iscntrl(ch)) {
                        escaped += "\\x";
                        escaped += HEX_DIGITS[(ch >> 4) & 0x0F];
                        escaped += HEX_DIGITS[ch & 0x0F];
                    } else {
                        escaped.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }

            return escaped;
        }
    } // namespace

    // Constants
    constexpr uint16_t MAKCU_VID = 0x1A86;
    constexpr uint16_t MAKCU_PID = 0x55D3;
    constexpr const char* TARGET_DESC = "USB-Enhanced-SERIAL CH343";
    constexpr const char* DEFAULT_NAME = "USB-SERIAL CH340";
    constexpr uint32_t INITIAL_BAUD_RATE = 115200;
    constexpr uint32_t HIGH_SPEED_BAUD_RATE = 4000000;

    // Static member definitions for PerformanceProfiler
    std::atomic<bool> PerformanceProfiler::s_enabled{ false };
    std::mutex PerformanceProfiler::s_mutex;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> PerformanceProfiler::s_stats;

    // Command cache for maximum performance - using constexpr arrays for zero-overhead lookups
    struct CommandCache {
        static constexpr size_t BUTTON_COUNT = 5; // LEFT, RIGHT, MIDDLE, SIDE1, SIDE2
        static constexpr size_t LOCK_TARGET_COUNT = 7; // X, Y, LEFT, RIGHT, MIDDLE, SIDE1, SIDE2

        // Pre-computed command strings indexed by MouseButton enum value
        std::array<std::string, BUTTON_COUNT> press_commands;
        std::array<std::string, BUTTON_COUNT> release_commands;

        // Lock/unlock/query commands indexed by LockTarget enum value
        std::array<std::string, LOCK_TARGET_COUNT> lock_commands;
        std::array<std::string, LOCK_TARGET_COUNT> unlock_commands;
        std::array<std::string, LOCK_TARGET_COUNT> query_commands;

        CommandCache() {
            // Pre-compute all button commands indexed by MouseButton
            press_commands[std::to_underlying(MouseButton::LEFT)] = "km.left(1)";
            press_commands[std::to_underlying(MouseButton::RIGHT)] = "km.right(1)";
            press_commands[std::to_underlying(MouseButton::MIDDLE)] = "km.middle(1)";
            press_commands[std::to_underlying(MouseButton::SIDE1)] = "km.ms1(1)";
            press_commands[std::to_underlying(MouseButton::SIDE2)] = "km.ms2(1)";

            release_commands[std::to_underlying(MouseButton::LEFT)] = "km.left(0)";
            release_commands[std::to_underlying(MouseButton::RIGHT)] = "km.right(0)";
            release_commands[std::to_underlying(MouseButton::MIDDLE)] = "km.middle(0)";
            release_commands[std::to_underlying(MouseButton::SIDE1)] = "km.ms1(0)";
            release_commands[std::to_underlying(MouseButton::SIDE2)] = "km.ms2(0)";

            // Pre-compute lock commands indexed by LockTarget
            lock_commands[0] = "km.lock_mx(1)"; // X
            lock_commands[1] = "km.lock_my(1)"; // Y
            lock_commands[2] = "km.lock_ml(1)"; // LEFT
            lock_commands[3] = "km.lock_mr(1)"; // RIGHT
            lock_commands[4] = "km.lock_mm(1)"; // MIDDLE
            lock_commands[5] = "km.lock_ms1(1)"; // SIDE1
            lock_commands[6] = "km.lock_ms2(1)"; // SIDE2

            unlock_commands[0] = "km.lock_mx(0)";
            unlock_commands[1] = "km.lock_my(0)";
            unlock_commands[2] = "km.lock_ml(0)";
            unlock_commands[3] = "km.lock_mr(0)";
            unlock_commands[4] = "km.lock_mm(0)";
            unlock_commands[5] = "km.lock_ms1(0)";
            unlock_commands[6] = "km.lock_ms2(0)";

            query_commands[0] = "km.lock_mx()";
            query_commands[1] = "km.lock_my()";
            query_commands[2] = "km.lock_ml()";
            query_commands[3] = "km.lock_mr()";
            query_commands[4] = "km.lock_mm()";
            query_commands[5] = "km.lock_ms1()";
            query_commands[6] = "km.lock_ms2()";
        }

        // Safe button command lookup — returns nullptr if out of range
        const std::string* getPressCommand(MouseButton button) const {
            auto idx = std::to_underlying(button);
            return idx < BUTTON_COUNT ? &press_commands[idx] : nullptr;
        }

        const std::string* getReleaseCommand(MouseButton button) const {
            auto idx = std::to_underlying(button);
            return idx < BUTTON_COUNT ? &release_commands[idx] : nullptr;
        }
    };

    // High-performance PIMPL implementation
    class Device::Impl {
    public:
        std::unique_ptr<SerialPort> serialPort;
        DeviceInfo deviceInfo;
        std::atomic<ConnectionStatus> atomicStatus{ConnectionStatus::DISCONNECTED};
        std::atomic<bool> connected;
        std::atomic<bool> highPerformanceMode;
        mutable std::mutex mutex;
        static std::string lastError;

        // Command cache for ultra-fast lookups
        CommandCache commandCache;

        // State caching with bitwise operations (like Python v2.0)
        std::atomic<uint16_t> lockStateCache{ 0 };  // 16 bits for different lock states
        std::atomic<bool> lockStateCacheValid{ false };

        // Button state tracking
        std::atomic<uint8_t> currentButtonMask{ 0 };
        std::atomic<bool> buttonMonitoringEnabled{ false };

        // Callbacks
        Device::MouseButtonCallback mouseButtonCallback;
        Device::ConnectionCallback connectionCallback;
        mutable std::mutex callbackMutex;

        // Pre-allocated string buffers for different command types
        mutable std::string moveCommandBuffer;
        mutable std::string smoothCommandBuffer;
        mutable std::string bezierCommandBuffer;
        mutable std::string wheelCommandBuffer;
        mutable std::string generalCommandBuffer;
        mutable std::mutex commandBufferMutex;

        // Connection monitoring
        std::jthread monitoringThread;
        std::condition_variable monitoringCondition;
        std::mutex monitoringMutex;

        enum class LockTarget : uint8_t {
            X = 0,
            Y = 1,
            LEFT = 2,
            RIGHT = 3,
            MIDDLE = 4,
            SIDE1 = 5,
            SIDE2 = 6
        };
        
        // Safe thread cleanup
        void cleanupMonitoringThread() {
            if (!monitoringThread.joinable()) {
                return;
            }

            monitoringThread.request_stop();

            // Wake up the monitoring thread immediately.
            monitoringCondition.notify_all();

            if (std::this_thread::get_id() != monitoringThread.get_id()) {
                monitoringThread.join();
            }
            // If on the monitoring thread, skip join — the jthread destructor
            // will join after the monitoring loop exits via the stop token.
        }

        Impl() : serialPort(std::make_unique<SerialPort>())
            , connected(false)
            , highPerformanceMode(false) {
          
            deviceInfo.isConnected = false;

            // Pre-allocate command buffers to avoid frequent allocations
            moveCommandBuffer.reserve(128);
            smoothCommandBuffer.reserve(128);
            bezierCommandBuffer.reserve(192);
            wheelCommandBuffer.reserve(64);
            generalCommandBuffer.reserve(256);

            // Set up button callback for serial port
            serialPort->setButtonCallback([this](uint8_t button, bool pressed) {
                handleButtonEvent(button, pressed);
                });
        }

        ~Impl() = default;

        // Private static method for the core baud rate change protocol
        static bool performBaudRateChange(SerialPort* serialPort, uint32_t baudRate) {
            if (!serialPort->isOpen()) {
                return false;
            }

            // Create MAKCU baud rate change command
            // Protocol: 0xDE 0xAD [size_u16] 0xA5 [baud_u32]
            std::vector<uint8_t> baudChangeCommand = {
                0xDE, 0xAD,                                    // Standard header
                0x05, 0x00,                                    // Size (5 bytes: command + 4-byte baud rate)
                0xA5,                                          // Baud rate change command
                static_cast<uint8_t>(baudRate & 0xFF),         // Baud rate bytes (little-endian)
                static_cast<uint8_t>((baudRate >> 8) & 0xFF),
                static_cast<uint8_t>((baudRate >> 16) & 0xFF),
                static_cast<uint8_t>((baudRate >> 24) & 0xFF)
            };

            // Send the baud rate change command
            if (!serialPort->write(baudChangeCommand)) {   
                setLastError("Baud rate change serial port write failed: " + serialPort->getLastError());
                return false;
            }

            if (!serialPort->flush()) {
                setLastError("Baud rate change serial port flush failed: " + serialPort->getLastError());
                return false;
            }

            // Close and reopen at new baud rate
            std::string portName = serialPort->getPortName();
            serialPort->close();

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (!serialPort->open(portName, baudRate)) {
                setLastError("Baud rate change serial port open failed: " + serialPort->getLastError());
                return false;
            }

            return true;
        }

        static void setLastError(const std::string& error) {
            lastError = error;
        }

        bool initializeDevice() {
            if (!serialPort->isOpen()) {
                setLastError("Initialize device serial port open failed: " + serialPort->getLastError());
                return false;
            }

            // Small delay for device to be ready
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Enable button monitoring - fire and forget for performance
            return serialPort->sendCommand("km.buttons(1)");
        }

        void handleButtonEvent(uint8_t button, bool pressed) {
            // Update button mask atomically using fetch_or/fetch_and
            // to avoid lost updates from concurrent button events
            const uint8_t bit = static_cast<uint8_t>(1u << button);
            if (pressed) {
                currentButtonMask.fetch_or(bit, std::memory_order_acq_rel);
            }
            else {
                currentButtonMask.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_acq_rel);
            }

            // Call user callback if set
            if (button >= 5) {
                return;
            }

            Device::MouseButtonCallback callbackCopy;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                callbackCopy = mouseButtonCallback;
            }

            if (!callbackCopy) {
                return;
            }

            const MouseButton mouseBtn = static_cast<MouseButton>(button);
            try {
                callbackCopy(mouseBtn, pressed);
            }
            catch (...) {
                // Ignore callback exceptions.
            }
        }

        void notifyConnectionChange(bool isConnected) {
            Device::ConnectionCallback callbackCopy;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                callbackCopy = connectionCallback;
            }

            if (!callbackCopy) {
                return;
            }

            try {
                callbackCopy(isConnected);
            }
            catch (...) {
                // Ignore callback exceptions.
            }
        }

        void connectionMonitoringLoop(std::stop_token stopToken) {
            int pollInterval = 150;
            const int maxPollInterval = 500;
            const int pollIncrement = 50;
            
            while (!stopToken.stop_requested()) {
                // Double-check connection state with acquire semantics to ensure we see all updates
                bool currentlyConnected = connected.load(std::memory_order_acquire);
                if (!currentlyConnected) {
                    break;
                }
                
                // Check actual connection status using platform-specific methods
                // Use a local variable to avoid multiple calls during state updates
                bool actuallyConnected = serialPort->isActuallyConnected();
                
                if (!actuallyConnected) {
                    // Device disconnected - use compare_exchange to prevent race conditions
                    // Only update if we're still marked as connected
                    bool expectedConnected = true;
                    if (connected.compare_exchange_strong(expectedConnected, false, std::memory_order_acq_rel)) {
                        // We successfully changed from connected to disconnected
                        // Now update all other state atomically
                        atomicStatus.store(ConnectionStatus::DISCONNECTED, std::memory_order_release);
                        currentButtonMask.store(0, std::memory_order_release);
                        lockStateCacheValid.store(false, std::memory_order_release);
                        buttonMonitoringEnabled.store(false, std::memory_order_release);
                        
                        // Trigger callback after all state is updated
                        notifyConnectionChange(false);
                    }
                    
                    // Exit the loop regardless of who updated the state
                    break;
                }
                
                // Use condition variable for interruptible sleep with exponential backoff
                std::unique_lock<std::mutex> lock(monitoringMutex);
                if (monitoringCondition.wait_for(lock, std::chrono::milliseconds(pollInterval),
                    [&stopToken] { return stopToken.stop_requested(); })) {
                    // Condition was signaled (stop requested)
                    break;
                }
                
                // Exponential backoff to reduce CPU usage
                pollInterval = std::min<int>(maxPollInterval, pollInterval + pollIncrement);
            }
        }

        // High-performance command execution
        bool executeCommand(const std::string& command) {
            if (!connected.load(std::memory_order_acquire)) {
                return false;
            }

            auto start = std::chrono::high_resolution_clock::now();

            bool result = serialPort->sendCommand(command);

            // Performance profiling
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            makcu::PerformanceProfiler::logCommandTiming(command, duration);

            return result;
        }


        // Optimized move command with buffer reuse and bounds checking
        bool executeMoveCommand(int32_t x, int32_t y) {
            // Validate coordinate ranges to prevent buffer overflow
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;
            
            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD) {
                #ifdef DEBUG
                std::cerr << "Move coordinates out of range: (" << x << "," << y << ")" << std::endl;
                #endif
                return false;
            }
            
            std::lock_guard<std::mutex> lock(commandBufferMutex);
            moveCommandBuffer.clear();

            moveCommandBuffer = "km.move(";
            moveCommandBuffer += std::to_string(x);
            moveCommandBuffer += ",";
            moveCommandBuffer += std::to_string(y);
            moveCommandBuffer += ")";

            // Additional length check
            if (moveCommandBuffer.length() > 512) {
                return false;
            }

            return executeCommand(moveCommandBuffer);
        }

        // Optimized smooth move command with buffer reuse
        bool executeSmoothMoveCommand(int32_t x, int32_t y, uint32_t segments) {
            // Validate inputs
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;
            
            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD) {
                return false;
            }
            if (segments > 1000) { // Reasonable limit
                return false;
            }
            
            std::lock_guard<std::mutex> lock(commandBufferMutex);
            smoothCommandBuffer.clear();

            smoothCommandBuffer = "km.move(";
            smoothCommandBuffer += std::to_string(x);
            smoothCommandBuffer += ",";
            smoothCommandBuffer += std::to_string(y);
            smoothCommandBuffer += ",";
            smoothCommandBuffer += std::to_string(segments);
            smoothCommandBuffer += ")";

            return executeCommand(smoothCommandBuffer);
        }

        // Optimized bezier move command with buffer reuse
        bool executeBezierMoveCommand(int32_t x, int32_t y, uint32_t segments, int32_t ctrl_x, int32_t ctrl_y) {
            // Validate inputs
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;
            
            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD ||
                ctrl_x < MIN_COORD || ctrl_x > MAX_COORD || ctrl_y < MIN_COORD || ctrl_y > MAX_COORD) {
                return false;
            }
            if (segments > 1000) { // Reasonable limit
                return false;
            }
            
            std::lock_guard<std::mutex> lock(commandBufferMutex);
            bezierCommandBuffer.clear();

            bezierCommandBuffer = "km.move(";
            bezierCommandBuffer += std::to_string(x);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(y);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(segments);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(ctrl_x);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(ctrl_y);
            bezierCommandBuffer += ")";

            return executeCommand(bezierCommandBuffer);
        }

        // Optimized wheel command with buffer reuse
        bool executeWheelCommand(int32_t delta) {
            // Validate wheel delta range
            if (delta < -32768 || delta > 32767) {
                return false;
            }
            
            std::lock_guard<std::mutex> lock(commandBufferMutex);
            wheelCommandBuffer.clear();

            wheelCommandBuffer = "km.wheel(";
            wheelCommandBuffer += std::to_string(delta);
            wheelCommandBuffer += ")";

            return executeCommand(wheelCommandBuffer);
        }

        static constexpr uint16_t lockBit(LockTarget target) {
            return static_cast<uint16_t>(1u << std::to_underlying(target));
        }

        // Cache-based lock state management using atomic RMW
        void updateLockStateCache(LockTarget target, bool locked) {
            const uint16_t bit = lockBit(target);
            if (locked) {
                lockStateCache.fetch_or(bit, std::memory_order_acq_rel);
            }
            else {
                lockStateCache.fetch_and(static_cast<uint16_t>(~bit), std::memory_order_acq_rel);
            }
            lockStateCacheValid.store(true, std::memory_order_release);
        }

        bool getLockStateFromCache(LockTarget target) const {
            if (!lockStateCacheValid.load(std::memory_order_acquire)) {
                return false; // Cache invalid
            }

            return (lockStateCache.load(std::memory_order_acquire) & lockBit(target)) != 0;
        }
    };

    // Device implementation
    Device::Device()
        : m_impl(std::make_unique<Impl>())
        , m_lifetimeToken(std::make_shared<std::atomic<bool>>(true)) {}
    std::string Device::Impl::lastError = "";

    Device::~Device() {
        if (m_lifetimeToken) {
            m_lifetimeToken->store(false, std::memory_order_release);
        }
        disconnect();
    }

    std::vector<DeviceInfo> Device::findDevices() {
        std::vector<DeviceInfo> devices;
        auto ports = SerialPort::findMakcuPorts();

        for (const auto& port : ports) {
            DeviceInfo info;
            info.port = port;
            info.description = TARGET_DESC;
            info.vid = MAKCU_VID;
            info.pid = MAKCU_PID;
            info.isConnected = false;
            devices.push_back(info);
        }

        return devices;
    }

    std::string Device::getLastError()
    {
        return m_impl->lastError;
    }

    std::string Device::findFirstDevice() {
        auto devices = findDevices();
        return devices.empty() ? "" : devices[0].port;
    }

    bool Device::connect(const std::string& port, bool highSpeed) {
        std::unique_lock<std::mutex> lock(m_impl->mutex);

        if (m_impl->connected.load()) {
            return true;
        }

        std::string targetPort = port.empty() ? findFirstDevice() : port;
        if (targetPort.empty()) {
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->setLastError("Invalid device port!");
            return false;
        }

        m_impl->atomicStatus.store(ConnectionStatus::CONNECTING, std::memory_order_release);

        // Open at initial baud rate
        if (!m_impl->serialPort->open(targetPort, INITIAL_BAUD_RATE)) {
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->setLastError("Serial port open failed: " + m_impl->serialPort->getLastError());
            return false;
        }

        // Switch to high-speed mode if requested
        if (highSpeed && !Impl::performBaudRateChange(m_impl->serialPort.get(), HIGH_SPEED_BAUD_RATE))
        {
            m_impl->serialPort->close();
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // Validate connection after baud rate switch
        if (!m_impl->serialPort->isOpen() || !m_impl->serialPort->isActuallyConnected()) {
            m_impl->setLastError("Serial port not opened or connected!");
            m_impl->serialPort->close();
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // Initialize device
        if (!m_impl->initializeDevice()) {
            m_impl->serialPort->close();
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // Final validation that device is responsive
        try {
            // Test device responsiveness with a simple command
            auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true, 
                std::chrono::milliseconds(100));
            
            // Wait for response with timeout
            if (future.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout) {
                m_impl->setLastError("Device connection response timeout!");
                m_impl->serialPort->close();
                m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
                m_impl->deviceInfo.isConnected = false;
                return false;
            }
            
            // Get the result to ensure no exception
            future.get();
        }
        catch (...) {
            // Device not responding properly
            m_impl->setLastError("Device not responding properly!");
            m_impl->serialPort->close();
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // Update device info first
        m_impl->deviceInfo.port = targetPort;
        m_impl->deviceInfo.description = TARGET_DESC;
        m_impl->deviceInfo.vid = MAKCU_VID;
        m_impl->deviceInfo.pid = MAKCU_PID;
        m_impl->deviceInfo.isConnected = true;

        // Atomically update all connection state before starting monitoring thread
        m_impl->atomicStatus.store(ConnectionStatus::CONNECTED, std::memory_order_release);
        m_impl->buttonMonitoringEnabled.store(true, std::memory_order_release);
        
        // Use acquire-release semantics to ensure all state is visible before connected flag is set
        std::atomic_thread_fence(std::memory_order_release);
        m_impl->connected.store(true, std::memory_order_release);
        
        // Start connection monitoring thread AFTER all state is established
        // This prevents the monitoring thread from seeing inconsistent state
        try {
            m_impl->monitoringThread = std::jthread([impl = m_impl.get()](std::stop_token stopToken) {
                impl->connectionMonitoringLoop(stopToken);
            });
        } catch (const std::system_error&) {
            // Thread creation failed - cleanup and return error
            m_impl->setLastError("Monitoring thread creation failure!");
            m_impl->connected.store(false, std::memory_order_release);
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            m_impl->serialPort->close();
            return false;
        }

        lock.unlock();
        m_impl->notifyConnectionChange(true);

        return true;
    }

    std::future<bool> Device::connectAsync(const std::string& port) {
        // OPTIMIZED: Use immediate return for already connected state
        if (m_impl->connected.load(std::memory_order_acquire)) {
            // Create ready future more efficiently
            std::packaged_task<bool()> task([]() { return true; });
            auto future = task.get_future();
            task();
            return future;
        }
        
        // For actual connection, this is inherently I/O bound so thread is acceptable
        return std::async(std::launch::async, [this, port]() {
            return connect(port);
        });
    }

    std::expected<void, ConnectionStatus> Device::connectExpected(const std::string& port) {
        if (connect(port)) {
            return {};
        }

        return std::unexpected(getStatus());
    }

    void Device::disconnect() {
        bool shouldNotify = false;
        {
            std::unique_lock<std::mutex> lock(m_impl->mutex);

            // Always clean up monitoring thread first, regardless of connection state.
            m_impl->cleanupMonitoringThread();

            // Use compare_exchange to prevent race conditions with monitoring thread.
            bool expectedConnected = true;
            shouldNotify = m_impl->connected.compare_exchange_strong(
                expectedConnected, false, std::memory_order_acq_rel);

            // Ensure all disconnected state is consistent even if another thread
            // already marked the connection as down.
            m_impl->atomicStatus.store(ConnectionStatus::DISCONNECTED, std::memory_order_release);

            // Always close the serial port if it is still open.
            if (m_impl->serialPort->isOpen()) {
                m_impl->serialPort->close();
            }

            // Update remaining state after serial port is closed.
            m_impl->deviceInfo.isConnected = false;
            m_impl->currentButtonMask.store(0, std::memory_order_release);
            m_impl->lockStateCacheValid.store(false, std::memory_order_release);
            m_impl->buttonMonitoringEnabled.store(false, std::memory_order_release);
        }

        if (shouldNotify) {
            m_impl->notifyConnectionChange(false);
        }
    }


    bool Device::isConnected() const noexcept {
        return m_impl->connected.load(std::memory_order_acquire);
    }

    ConnectionStatus Device::getStatus() const noexcept {
        return m_impl->atomicStatus.load(std::memory_order_acquire);
    }

    DeviceInfo Device::getDeviceInfo() const {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        DeviceInfo info = m_impl->deviceInfo;
        info.isConnected = m_impl->connected.load(std::memory_order_acquire);
        return info;
    }

    std::string Device::getVersion() const {
        if (!m_impl->connected.load()) {
            return "";
        }

        // Retry with escalating timeouts to tolerate temporary instability
        // immediately after a baud-rate transition.
        constexpr std::array<std::chrono::milliseconds, 3> timeouts = {
            std::chrono::milliseconds(75),
            std::chrono::milliseconds(150),
            std::chrono::milliseconds(300)
        };

        for (size_t attempt = 0; attempt < timeouts.size(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 10 : 20));

            auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true, timeouts[attempt]);
            try {
                std::string version = future.get();
                if (!version.empty()) {
                    return version;
                }
            }
            catch (...) {
                // Continue retry loop.
            }

            if (!m_impl->connected.load(std::memory_order_acquire)) {
                return "";
            }
        }

        return "";
    }

    std::expected<std::string, ConnectionStatus> Device::getVersionExpected() const {
        if (!m_impl->connected.load(std::memory_order_acquire)) {
            return std::unexpected(ConnectionStatus::DISCONNECTED);
        }

        const std::string version = getVersion();
        if (version.empty()) {
            return std::unexpected(getStatus());
        }

        return version;
    }


    // High-performance mouse control methods
    bool Device::mouseDown(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        const auto* cmd = m_impl->commandCache.getPressCommand(button);
        return cmd ? m_impl->executeCommand(*cmd) : false;
    }

    bool Device::mouseUp(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        const auto* cmd = m_impl->commandCache.getReleaseCommand(button);
        return cmd ? m_impl->executeCommand(*cmd) : false;
    }

    bool Device::click(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // For maximum performance, batch press+release
        const auto* pressCmd = m_impl->commandCache.getPressCommand(button);
        const auto* releaseCmd = m_impl->commandCache.getReleaseCommand(button);

        if (pressCmd && releaseCmd) {
            bool result1 = m_impl->executeCommand(*pressCmd);
            bool result2 = m_impl->executeCommand(*releaseCmd);
            return result1 && result2;
        }
        return false;
    }




    bool Device::mouseButtonState(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // Use cached button state for performance
        uint8_t mask = m_impl->currentButtonMask.load();
        return (mask & (1u << std::to_underlying(button))) != 0;
    }


    // High-performance movement methods
    bool Device::mouseMove(int32_t x, int32_t y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeMoveCommand(x, y);
    }

    bool Device::mouseMoveSmooth(int32_t x, int32_t y, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeSmoothMoveCommand(x, y, segments);
    }

    bool Device::mouseMoveBezier(int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeBezierMoveCommand(x, y, segments, ctrl_x, ctrl_y);
    }

    // High-performance drag operations
    bool Device::mouseDrag(MouseButton button, int32_t x, int32_t y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        const auto* pressCmd = m_impl->commandCache.getPressCommand(button);
        const auto* releaseCmd = m_impl->commandCache.getReleaseCommand(button);
        if (!pressCmd || !releaseCmd) return false;

        // Execute drag sequence: press -> move -> release
        bool result1 = m_impl->executeCommand(*pressCmd);
        bool result2 = m_impl->executeMoveCommand(x, y);
        bool result3 = m_impl->executeCommand(*releaseCmd);

        return result1 && result2 && result3;
    }

    bool Device::mouseDragSmooth(MouseButton button, int32_t x, int32_t y, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        const auto* pressCmd = m_impl->commandCache.getPressCommand(button);
        const auto* releaseCmd = m_impl->commandCache.getReleaseCommand(button);
        if (!pressCmd || !releaseCmd) return false;

        // Execute smooth drag sequence: press -> smooth move -> release
        bool result1 = m_impl->executeCommand(*pressCmd);
        bool result2 = m_impl->executeSmoothMoveCommand(x, y, segments);
        bool result3 = m_impl->executeCommand(*releaseCmd);

        return result1 && result2 && result3;
    }

    bool Device::mouseDragBezier(MouseButton button, int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        const auto* pressCmd = m_impl->commandCache.getPressCommand(button);
        const auto* releaseCmd = m_impl->commandCache.getReleaseCommand(button);
        if (!pressCmd || !releaseCmd) return false;

        // Execute bezier drag sequence: press -> bezier move -> release
        bool result1 = m_impl->executeCommand(*pressCmd);
        bool result2 = m_impl->executeBezierMoveCommand(x, y, segments, ctrl_x, ctrl_y);
        bool result3 = m_impl->executeCommand(*releaseCmd);

        return result1 && result2 && result3;
    }




    bool Device::mouseWheel(int32_t delta) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeWheelCommand(delta);
    }


    // Mouse locking methods with caching
    bool Device::lockMouseX(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::X);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::X, lock);
        }
        return result;
    }

    bool Device::lockMouseY(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::Y);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::Y, lock);
        }
        return result;
    }

    bool Device::lockMouseLeft(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::LEFT);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::LEFT, lock);
        }
        return result;
    }

    bool Device::lockMouseMiddle(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::MIDDLE);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::MIDDLE, lock);
        }
        return result;
    }

    bool Device::lockMouseRight(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::RIGHT);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::RIGHT, lock);
        }
        return result;
    }

    bool Device::lockMouseSide1(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::SIDE1);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::SIDE1, lock);
        }
        return result;
    }

    bool Device::lockMouseSide2(bool lock) {
        if (!m_impl->connected.load()) return false;

        auto idx = std::to_underlying(Impl::LockTarget::SIDE2);
        const std::string& command = lock ?
            m_impl->commandCache.lock_commands[idx] :
            m_impl->commandCache.unlock_commands[idx];

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache(Impl::LockTarget::SIDE2, lock);
        }
        return result;
    }

    // Fast cached lock state queries
    bool Device::isMouseXLocked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::X);
    }

    bool Device::isMouseYLocked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::Y);
    }

    bool Device::isMouseLeftLocked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::LEFT);
    }

    bool Device::isMouseMiddleLocked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::MIDDLE);
    }

    bool Device::isMouseRightLocked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::RIGHT);
    }

    bool Device::isMouseSide1Locked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::SIDE1);
    }

    bool Device::isMouseSide2Locked() const {
        return m_impl->getLockStateFromCache(Impl::LockTarget::SIDE2);
    }

    std::unordered_map<std::string, bool> Device::getAllLockStates() const {
        return {
            {"X", isMouseXLocked()},
            {"Y", isMouseYLocked()},
            {"LEFT", isMouseLeftLocked()},
            {"RIGHT", isMouseRightLocked()},
            {"MIDDLE", isMouseMiddleLocked()},
            {"SIDE1", isMouseSide1Locked()},
            {"SIDE2", isMouseSide2Locked()}
        };
    }

    // Mouse input catching methods
    uint8_t Device::catchMouseLeft() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ml()", true,
            std::chrono::milliseconds(50));
        try {
            const std::string response = future.get();
            const auto parsed = parseUint8Decimal(response);
            return parsed.value_or(0);
        }
        catch (...) {
            return 0;
        }
    }

    uint8_t Device::catchMouseMiddle() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_mm()", true,
            std::chrono::milliseconds(50));
        try {
            const std::string response = future.get();
            const auto parsed = parseUint8Decimal(response);
            return parsed.value_or(0);
        }
        catch (...) {
            return 0;
        }
    }

    uint8_t Device::catchMouseRight() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_mr()", true,
            std::chrono::milliseconds(50));
        try {
            const std::string response = future.get();
            const auto parsed = parseUint8Decimal(response);
            return parsed.value_or(0);
        }
        catch (...) {
            return 0;
        }
    }

    uint8_t Device::catchMouseSide1() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ms1()", true,
            std::chrono::milliseconds(50));
        try {
            const std::string response = future.get();
            const auto parsed = parseUint8Decimal(response);
            return parsed.value_or(0);
        }
        catch (...) {
            return 0;
        }
    }

    uint8_t Device::catchMouseSide2() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ms2()", true,
            std::chrono::milliseconds(50));
        try {
            const std::string response = future.get();
            const auto parsed = parseUint8Decimal(response);
            return parsed.value_or(0);
        }
        catch (...) {
            return 0;
        }
    }

    // Button monitoring methods
    bool Device::enableButtonMonitoring(bool enable) {
        if (!m_impl->connected.load(std::memory_order_acquire)) {
            return false;
        }

        std::string command = enable ? "km.buttons(1)" : "km.buttons(0)";
        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->buttonMonitoringEnabled.store(enable, std::memory_order_release);
        }
        return result;
    }

    bool Device::isButtonMonitoringEnabled() const noexcept {
        return m_impl->buttonMonitoringEnabled.load(std::memory_order_acquire);
    }

    uint8_t Device::getButtonMask() const noexcept {
        return m_impl->currentButtonMask.load();
    }

    // Serial spoofing methods
    std::string Device::getMouseSerial() {
        if (!m_impl->connected.load()) return "";

        // Small delay to ensure any pending responses are cleared
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto future = m_impl->serialPort->sendTrackedCommand("km.serial()", true,
            std::chrono::milliseconds(50));
        try {
            return future.get();
        }
        catch (...) {
            return "";
        }
    }

    bool Device::setMouseSerial(const std::string& serial) {
        if (!m_impl->connected.load()) return false;

        std::string command = "km.serial('";
        command += escapeSingleQuotedCommandString(serial);
        command += "')";
        return m_impl->executeCommand(command);
    }

    bool Device::resetMouseSerial() {
        if (!m_impl->connected.load()) return false;
        return m_impl->executeCommand("km.serial(0)");
    }



    bool Device::setBaudRate(uint32_t baudRate, bool validateCommunication) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // Clamp baud rate to valid range as per MAKCU protocol
        if (baudRate < 115200) {
            baudRate = 115200;
        } else if (baudRate > 4000000) {
            baudRate = 4000000;
        }

        // Use the static helper method for the core baud rate change
        if (!Impl::performBaudRateChange(m_impl->serialPort.get(), baudRate)) {
            disconnect();
            return false;
        }

        // If validation is requested (for manual setBaudRate calls), test communication
        if (validateCommunication) {
            try {
                auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true, std::chrono::milliseconds(1000));
                auto response = future.get();
                
                // Check if we got a valid response containing "km.MAKCU"
                if (response.find("km.MAKCU") != std::string::npos) {
                    return true;
                } else {
                    // Communication test failed, attempt recovery to known-safe default speed.
                    bool recovered = (baudRate != 115200) &&
                        Impl::performBaudRateChange(m_impl->serialPort.get(), 115200);
                    if (!recovered) {
                        disconnect();
                    }
                    return false;
                }
            } catch (...) {
                // Exception occurred, attempt recovery to known-safe default speed.
                bool recovered = (baudRate != 115200) &&
                    Impl::performBaudRateChange(m_impl->serialPort.get(), 115200);
                if (!recovered) {
                    disconnect();
                }
                return false;
            }
        }

        return true;
    }

    void Device::setMouseButtonCallback(MouseButtonCallback callback) {
        std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
        m_impl->mouseButtonCallback = std::move(callback);
    }

    void Device::setConnectionCallback(ConnectionCallback callback) {
        std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
        m_impl->connectionCallback = std::move(callback);
    }

    // High-level automation methods
    bool Device::clickSequence(const std::vector<MouseButton>& buttons,
        std::chrono::milliseconds delay) {
        if (!m_impl->connected.load()) {
            return false;
        }

        for (const auto& button : buttons) {
            if (!click(button)) {
                return false;
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }
        return true;
    }


    bool Device::movePattern(const std::vector<std::pair<int32_t, int32_t>>& points,
        bool smooth, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        for (const auto& [x, y] : points) {
            if (smooth) {
                if (!mouseMoveSmooth(x, y, segments)) {
                    return false;
                }
            }
            else {
                if (!mouseMove(x, y)) {
                    return false;
                }
            }
        }
        return true;
    }

    void Device::enableHighPerformanceMode(bool enable) {
        m_impl->highPerformanceMode.store(enable);
    }

    bool Device::isHighPerformanceModeEnabled() const noexcept {
        return m_impl->highPerformanceMode.load();
    }

    // Batch command builder implementation
    Device::BatchCommandBuilder Device::createBatch() {
        return BatchCommandBuilder(this, m_lifetimeToken);
    }

    bool Device::BatchCommandBuilder::isDeviceAlive() const {
        return m_device != nullptr &&
               m_deviceLifetime &&
               m_deviceLifetime->load(std::memory_order_acquire);
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::move(int32_t x, int32_t y) {
        if (!isDeviceAlive()) {
            return *this;
        }
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + ")");
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::moveSmooth(int32_t x, int32_t y, uint32_t segments) {
        if (!isDeviceAlive()) {
            return *this;
        }
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(segments) + ")");
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::moveBezier(int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!isDeviceAlive()) {
            return *this;
        }
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," +
            std::to_string(segments) + "," + std::to_string(ctrl_x) + "," + std::to_string(ctrl_y) + ")");
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::click(MouseButton button) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* pressCmd = cache.getPressCommand(button);
        const auto* releaseCmd = cache.getReleaseCommand(button);

        if (pressCmd && releaseCmd) {
            m_commands.push_back(*pressCmd);
            m_commands.push_back(*releaseCmd);
        }
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::press(MouseButton button) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* cmd = cache.getPressCommand(button);
        if (cmd) {
            m_commands.push_back(*cmd);
        }
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::release(MouseButton button) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* cmd = cache.getReleaseCommand(button);
        if (cmd) {
            m_commands.push_back(*cmd);
        }
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::scroll(int32_t delta) {
        if (!isDeviceAlive()) {
            return *this;
        }
        m_commands.push_back("km.wheel(" + std::to_string(delta) + ")");
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::drag(MouseButton button, int32_t x, int32_t y) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* pressCmd = cache.getPressCommand(button);
        const auto* releaseCmd = cache.getReleaseCommand(button);

        if (pressCmd && releaseCmd) {
            // Add press, move, release commands to batch (consistent with normal mouseDrag format)
            m_commands.push_back(*pressCmd);
            m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + ")");
            m_commands.push_back(*releaseCmd);
        }
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::dragSmooth(MouseButton button, int32_t x, int32_t y, uint32_t segments) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* pressCmd = cache.getPressCommand(button);
        const auto* releaseCmd = cache.getReleaseCommand(button);

        if (pressCmd && releaseCmd) {
            // Add press, smooth move, release commands to batch
            m_commands.push_back(*pressCmd);
            m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(segments) + ")");
            m_commands.push_back(*releaseCmd);
        }
        return *this;
    }

    Device::BatchCommandBuilder& Device::BatchCommandBuilder::dragBezier(MouseButton button, int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!isDeviceAlive()) {
            return *this;
        }
        auto& cache = m_device->m_impl->commandCache;
        const auto* pressCmd = cache.getPressCommand(button);
        const auto* releaseCmd = cache.getReleaseCommand(button);

        if (pressCmd && releaseCmd) {
            // Add press, bezier move, release commands to batch
            m_commands.push_back(*pressCmd);
            m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," +
                std::to_string(segments) + "," + std::to_string(ctrl_x) + "," + std::to_string(ctrl_y) + ")");
            m_commands.push_back(*releaseCmd);
        }
        return *this;
    }

    bool Device::BatchCommandBuilder::execute() {
        if (!isDeviceAlive()) {
            return false;
        }

        if (!m_device->m_impl->connected.load()) {
            return false;
        }

        for (const auto& command : m_commands) {
            if (!m_device->m_impl->executeCommand(command)) {
                return false;
            }
        }
        return true;
    }

    // Legacy raw command interface (not recommended)
    bool Device::sendRawCommand(const std::string& command) const {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->serialPort->sendCommand(command);
    }

    std::string Device::receiveRawResponse() const {
        if (!m_impl->connected.load(std::memory_order_acquire)) {
            return "";
        }

        return m_impl->serialPort->readString();
    }


    // Utility functions
    std::string mouseButtonToString(MouseButton button) {
        switch (button) {
        case MouseButton::LEFT: return "LEFT";
        case MouseButton::RIGHT: return "RIGHT";
        case MouseButton::MIDDLE: return "MIDDLE";
        case MouseButton::SIDE1: return "SIDE1";
        case MouseButton::SIDE2: return "SIDE2";
        case MouseButton::UNKNOWN: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    MouseButton stringToMouseButton(const std::string& buttonName) {
        const std::string_view name{buttonName};

        if (equalsIgnoreAsciiCase(name, "LEFT")) return MouseButton::LEFT;
        if (equalsIgnoreAsciiCase(name, "RIGHT")) return MouseButton::RIGHT;
        if (equalsIgnoreAsciiCase(name, "MIDDLE")) return MouseButton::MIDDLE;
        if (equalsIgnoreAsciiCase(name, "SIDE1")) return MouseButton::SIDE1;
        if (equalsIgnoreAsciiCase(name, "SIDE2")) return MouseButton::SIDE2;

        return MouseButton::UNKNOWN;
    }

} // namespace makcu
