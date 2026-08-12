#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace treeminer {

class TerminalUi {
public:
    TerminalUi() = default;
    ~TerminalUi();

    TerminalUi(const TerminalUi&) = delete;
    TerminalUi& operator=(const TerminalUi&) = delete;

    void start();
    void stop() noexcept;
    void postEvent(const std::string& message);
    bool isRunning() const noexcept { return running_.load(); }
    // Dashboard bind address, used only to render the console URL in the footer. Set before
    // start(); defaults to loopback (the getConsoleUrl default when the dashboard is local).
    void setBindAddress(std::string bind_address) { bindAddress_ = std::move(bind_address); }

private:
    void run() noexcept;
    void render();

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
    std::mutex eventMutex_;
    std::condition_variable wake_;
    std::deque<std::string> events_;
    std::uint64_t frame_ = 0;
    std::string bindAddress_ = "127.0.0.1";
};

} // namespace treeminer
