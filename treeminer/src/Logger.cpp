#include "Logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

std::mutex Logger::consoleMutex_;
std::function<void(const std::string&)> Logger::consoleSink_;

Logger::Logger(const std::string& baseFilename, size_t maxFileSize)
    : baseFilename_(baseFilename), maxFileSize_(maxFileSize) {
    currentFileSize_ = getCurrentFileSize(getCurrentFilename());
    outputFile_.open(getCurrentFilename(), std::ios::app);
}

void Logger::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto fullMessage = getCurrentTimestamp() + " " + message;
    if (currentFileSize_ + fullMessage.length() >= maxFileSize_) {
        switchFile();
    }
    outputFile_ << fullMessage << std::endl;
    currentFileSize_ += fullMessage.length() + 1;
}

std::string Logger::getCurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const std::tm localTime = *std::localtime(&time);

    std::ostringstream output;
    output << std::put_time(&localTime, "%m-%d %H:%M");
    return output.str();
}

size_t Logger::getCurrentFileSize(const std::string& filename) {
    if (std::filesystem::exists(filename)) {
        return std::filesystem::file_size(filename);
    }
    return 0;
}

std::string Logger::getCurrentFilename() {
    return baseFilename_ + std::to_string(fileIndex_) + ".txt";
}

void Logger::switchFile() {
    outputFile_.close();
    fileIndex_ = (fileIndex_ + 1) % 2;
    outputFile_.open(getCurrentFilename(), std::ios::out | std::ios::trunc);
    currentFileSize_ = 0;
}

void Logger::logToConsole(const std::string& message) {
    std::lock_guard<std::mutex> lock(consoleMutex_);
    if (consoleSink_) {
        consoleSink_(message);
        return;
    }
    std::cout << message;
    std::cout.flush();
}

void Logger::setConsoleSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(consoleMutex_);
    consoleSink_ = std::move(sink);
}

void Logger::clearConsoleSink() {
    std::lock_guard<std::mutex> lock(consoleMutex_);
    consoleSink_ = {};
}
