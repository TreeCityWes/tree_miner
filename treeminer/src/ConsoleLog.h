#pragma once

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

// Thread-safe terminal presentation for miner events and the single-line progress display.
// ANSI styling is enabled only for an interactive terminal and is disabled by NO_COLOR.
class ConsoleLog {
public:
    enum class Level { Debug, Info, Found, Ok, Retry, Park, Warn, Error };

    static void event(Level level, const std::string& component, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex());
        const bool color = colorEnabled();
        if (interactiveTerminal()) {
            std::cout << "\033[2K\r";
        }
        std::cout << (color ? "\033[2m" : "") << timestamp()
                  << (color ? "\033[0m" : "") << "  ";
        if (color) {
            std::cout << levelColor(level);
        }
        std::cout << std::left << std::setw(6) << levelName(level);
        if (color) {
            std::cout << "\033[0m\033[36m";
        }
        std::cout << std::left << std::setw(12) << component;
        if (color) {
            std::cout << "\033[0m";
        }
        std::cout << std::right << message << '\n';
        std::cout.flush();
    }

    static void progress(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex());
        if (interactiveTerminal()) {
            std::cout << message;
        } else {
            // Redirected/systemd output cannot redraw one line. Emit a plain snapshot at
            // most every 30 seconds instead of turning per-batch refreshes into huge logs.
            const auto now = std::chrono::steady_clock::now();
            auto& last = lastPlainProgress();
            if (last.time_since_epoch().count() != 0 &&
                now - last < std::chrono::seconds(30)) {
                return;
            }
            last = now;
            std::cout << stripAnsi(message) << '\n';
        }
        std::cout.flush();
    }

    // Plain console messages share the progress/event lock so background threads cannot
    // splice text into the live hashrate row. Interactive output always starts on a clean
    // line; redirected output remains ordinary line-oriented text.
    static void line(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex());
        if (interactiveTerminal()) {
            std::cout << "\033[2K\r";
        }
        std::cout << message;
        if (message.empty() || message.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
    }

    static bool colorEnabled() {
        static const bool enabled = [] {
            if (std::getenv("NO_COLOR") != nullptr) return false;
            return interactiveTerminal();
        }();
        return enabled;
    }

    static bool interactiveTerminal() {
        static const bool interactive = [] {
            if (std::getenv("TERM") == nullptr) return false;
#ifdef _WIN32
            return ::_isatty(::_fileno(stdout)) != 0;
#else
            return ::isatty(STDOUT_FILENO) != 0;
#endif
        }();
        return interactive;
    }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    static std::chrono::steady_clock::time_point& lastPlainProgress() {
        static std::chrono::steady_clock::time_point value{};
        return value;
    }

    static std::string timestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t raw = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &raw);
#else
        localtime_r(&raw, &local);
#endif
        std::ostringstream out;
        out << std::put_time(&local, "%H:%M:%S");
        return out.str();
    }

    static const char* levelName(Level level) {
        switch (level) {
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO";
            case Level::Found: return "FOUND";
            case Level::Ok:    return "OK";
            case Level::Retry: return "RETRY";
            case Level::Park:  return "PARK";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
        }
        return "INFO";
    }

    static const char* levelColor(Level level) {
        switch (level) {
            case Level::Debug: return "\033[90m";
            case Level::Info:  return "\033[34m";
            case Level::Found: return "\033[35m";
            case Level::Ok:    return "\033[32m";
            case Level::Retry: return "\033[33m";
            case Level::Park:  return "\033[33m";
            case Level::Warn:  return "\033[33m";
            case Level::Error: return "\033[31m";
        }
        return "";
    }

    static std::string stripAnsi(const std::string& value) {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size();) {
            if (value[i] == '\r') {
                ++i;
                continue;
            }
            if (value[i] == '\033' && i + 1 < value.size() && value[i + 1] == '[') {
                i += 2;
                while (i < value.size() &&
                       !((value[i] >= 'A' && value[i] <= 'Z') ||
                         (value[i] >= 'a' && value[i] <= 'z'))) {
                    ++i;
                }
                if (i < value.size()) {
                    ++i;
                }
                continue;
            }
            result.push_back(value[i++]);
        }
        return result;
    }
};
