#pragma once

// Export macros for shared library support across platforms
#ifdef _WIN32
    #ifdef MAKCU_EXPORTS
        #define MAKCU_API __declspec(dllexport)
    #elif defined(MAKCU_SHARED)
        #define MAKCU_API __declspec(dllimport)
    #else
        #define MAKCU_API
    #endif
#else
    #ifdef __GNUC__
        #define MAKCU_API __attribute__((visibility("default")))
    #else
        #define MAKCU_API
    #endif
#endif

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <future>
#include <unordered_map>
#include <functional>
#include <thread>
#include <deque>
#include <chrono>
#include <span>
#include <stop_token>

#ifdef _WIN32
#include <windows.h>
// Define ssize_t for Windows compatibility
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fstream>
#endif

namespace makcu {

    struct PendingCommand {
        std::string command;
        std::promise<std::string> promise;
        std::chrono::steady_clock::time_point timestamp;
        std::chrono::milliseconds timeout;
        int command_id;
        bool expect_response;

        PendingCommand(int id, const std::string& cmd, bool expect_resp, std::chrono::milliseconds to)
            : command(cmd), expect_response(expect_resp), timeout(to), command_id(id) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    class MAKCU_API SerialPort {
    public:
        SerialPort();
        ~SerialPort();

        [[nodiscard]] bool open(const std::string& port, uint32_t baudRate);
        void close();
        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool isActuallyConnected() const;

        [[nodiscard]] bool setBaudRate(uint32_t baudRate);
        [[nodiscard]] uint32_t getBaudRate() const noexcept;
        [[nodiscard]] std::string getPortName() const;

        // High-performance command execution with tracking
        [[nodiscard]] std::future<std::string> sendTrackedCommand(const std::string& command,
            bool expectResponse = false,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

        // Fast fire-and-forget commands
        [[nodiscard]] bool sendCommand(const std::string& command);

        // Legacy methods for compatibility
        [[nodiscard]] bool write(std::span<const uint8_t> data);
        [[nodiscard]] bool write(const std::vector<uint8_t>& data);
        [[deprecated("Use sendCommand() for text commands.")]]
        [[nodiscard]] bool write(const std::string& data);
        [[deprecated("Use tracked commands and callbacks instead of synchronous reads.")]]
        [[nodiscard]] std::vector<uint8_t> read(size_t maxBytes = 1024);
        [[deprecated("Use tracked commands and callbacks instead of synchronous reads.")]]
        [[nodiscard]] std::string readString(size_t maxBytes = 1024);

        [[nodiscard]] size_t available() const;
        [[nodiscard]] bool flush();

        // Optimized timeout control
        void setTimeout(uint32_t timeoutMs);
        [[nodiscard]] uint32_t getTimeout() const noexcept;

        // Port enumeration
        [[nodiscard]] static std::vector<std::string> getAvailablePorts();
        [[nodiscard]] static std::vector<std::string> findMakcuPorts();

        // Button callback support
        using ButtonCallback = std::function<void(uint8_t, bool)>;
        void setButtonCallback(ButtonCallback callback);

        // Get last platform error
        std::string getLastError();

    private:
        std::string m_portName;
        std::atomic<uint32_t> m_baudRate;
        std::atomic<uint32_t> m_timeout;
        std::atomic<bool> m_isOpen;
        mutable std::mutex m_mutex;
        mutable std::mutex m_nativeHandleMutex;

#ifdef _WIN32
        HANDLE m_handle;
        DCB m_dcb;
        COMMTIMEOUTS m_timeouts;
#else
        int m_fd;
        struct termios m_oldTermios;
        struct termios m_newTermios;
#endif

        // Command tracking system
        uint32_t m_commandCounter{ 0 };
        std::unordered_map<int, std::unique_ptr<PendingCommand>> m_pendingCommands;
        std::deque<int> m_pendingCommandOrder;
        std::mutex m_commandMutex;

        // High-performance listener thread
        std::jthread m_listenerThread;

        // Button data processing
        ButtonCallback m_buttonCallback;
        mutable std::mutex m_buttonCallbackMutex;
        std::atomic<uint8_t> m_lastButtonMask{ 0 };

        // Optimized parsing buffers
        static constexpr size_t BUFFER_SIZE = 4096;
        static constexpr size_t LINE_BUFFER_SIZE = 256;

        bool configurePort() { return platformConfigurePort(); }
        void updateTimeouts() { platformUpdateTimeouts(); }
        void listenerLoop(std::stop_token stopToken);
        void processIncomingData();
        void handleButtonData(uint8_t data);
        void processResponse(const std::string& response);
        void cleanupTimedOutCommands();
        int generateCommandId();

        // Platform abstraction helpers for unified logic
        bool platformOpen(const std::string& devicePath);
        void platformClose();
        bool platformConfigurePort();
        void platformUpdateTimeouts();
        void platformUpdateTimeoutsUnlocked();
        ssize_t platformWrite(const void* data, size_t length);
        ssize_t platformRead(void* buffer, size_t maxBytes);
        size_t platformBytesAvailable() const;
        bool platformFlush();
        std::string getLastPlatformError();

        // Disable copy
        SerialPort(const SerialPort&) = delete;
        SerialPort& operator=(const SerialPort&) = delete;
    };

} // namespace makcu
