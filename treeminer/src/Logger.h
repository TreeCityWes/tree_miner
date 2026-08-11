#ifndef LOGGER_H
#define LOGGER_H

#include <fstream>
#include <functional>
#include <mutex>
#include <string>

class Logger {
public:
    Logger(const std::string& baseFilename, size_t maxFileSize);

    void log(const std::string& message);
    static void logToConsole(const std::string& message);
    static void setConsoleSink(std::function<void(const std::string&)> sink);
    static void clearConsoleSink();

private:
    std::string getCurrentTimestamp();
    size_t getCurrentFileSize(const std::string& filename);
    std::string getCurrentFilename();
    void switchFile();

    std::string baseFilename_;
    std::ofstream outputFile_;
    size_t maxFileSize_;
    size_t currentFileSize_ = 0;
    int fileIndex_ = 0;
    std::mutex mutex_;
    static std::mutex consoleMutex_;
    static std::function<void(const std::string&)> consoleSink_;
};

#endif
