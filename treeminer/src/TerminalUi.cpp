#include "TerminalUi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "ConsoleLog.h"
#include "StatReporter.h"
#include "LocalServer.h"

namespace treeminer {
namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kAmber = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kDim = "\033[2m";
constexpr std::size_t kMaxEvents = 100;

std::string stripAnsi(const std::string& input)
{
    std::string output;
    bool escape = false;
    for (char c : input) {
        if (!escape && c == '\033') {
            escape = true;
            continue;
        }
        if (escape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                escape = false;
            }
            continue;
        }
        if (c != '\r' && c != '\n') {
            output.push_back(c);
        }
    }
    while (!output.empty() && output.front() == ' ') output.erase(output.begin());
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%H:%M:%S");
    return out.str();
}

std::size_t terminalWidth()
{
#ifndef _WIN32
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return std::max<std::size_t>(72, size.ws_col);
    }
#endif
    return 100;
}

std::size_t terminalHeight()
{
#ifndef _WIN32
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) {
        return std::max<std::size_t>(22, size.ws_row);
    }
#endif
    return 30;
}

std::string fit(const std::string& value, std::size_t width)
{
    if (value.size() <= width) return value + std::string(width - value.size(), ' ');
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::string rate(double hashes)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (hashes >= 1'000'000.0) out << hashes / 1'000'000.0 << " MH/s";
    else out << hashes / 1'000.0 << " kH/s";
    return out.str();
}

std::string duration(std::int64_t seconds)
{
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto secs = seconds % 60;
    std::ostringstream out;
    if (hours > 0) out << hours << "h ";
    out << std::setw(2) << std::setfill('0') << minutes << "m "
        << std::setw(2) << secs << "s";
    return out.str();
}

std::string divider(std::size_t width, const std::string& label)
{
    const std::string prefix = "+-- " + label + " ";
    return prefix + std::string(width > prefix.size() + 1 ? width - prefix.size() - 1 : 0, '-') + "+";
}

std::string columns(const std::vector<std::string>& values, std::size_t width)
{
    if (values.empty()) return std::string(width, ' ');
    const std::size_t columnWidth = width / values.size();
    std::string result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t available = i + 1 == values.size()
            ? width - result.size()
            : columnWidth;
        result += fit("  " + values[i], available);
    }
    return result;
}

std::string panelLine(std::size_t width, const std::string& content = {})
{
    return "|" + fit(content, width - 2) + "|";
}

std::string matrixRow(std::size_t width, std::size_t row, std::uint64_t frame)
{
    static constexpr char symbols[] = "0123456789abcdef#$@%";
    std::string result(width, ' ');
    for (std::size_t column = 0; column < width; ++column) {
        const std::uint64_t seed = column * 37 + row * 17 + frame;
        const std::size_t head = (frame / 2 + column * 5) % 23;
        if ((row + head) % 23 < 7 && seed % 4 != 0) {
            result[column] = symbols[(seed * 13 + frame / 3) % (sizeof(symbols) - 1)];
        }
    }
    return result;
}

} // namespace

TerminalUi::~TerminalUi()
{
    stop();
}

void TerminalUi::start()
{
    if (running_.exchange(true)) return;
    stopRequested_.store(false);
    ConsoleLog::setTuiOwnsStdout(true);
    ConsoleLog::writeRaw("\033[?1049h\033[?25l\033[2J\033[H");
    thread_ = std::thread(&TerminalUi::run, this);
}

void TerminalUi::stop() noexcept
{
    if (!running_.load()) return;
    stopRequested_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    ConsoleLog::setTuiOwnsStdout(false);
    ConsoleLog::writeRaw("\033[?25h\033[?1049l");
}

void TerminalUi::postEvent(const std::string& message)
{
    const std::string clean = stripAnsi(message);
    if (clean.empty() || clean.rfind("Mining:", 0) == 0) return;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        events_.push_back(timestamp() + "  " + clean);
        while (events_.size() > kMaxEvents) events_.pop_front();
    }
    wake_.notify_one();
}

void TerminalUi::run() noexcept
{
    while (!stopRequested_.load()) {
        try {
            render();
        } catch (...) {
            // A display failure must never affect mining.
        }
        std::unique_lock<std::mutex> lock(eventMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(500), [this] {
            return stopRequested_.load();
        });
    }
}

void TerminalUi::render()
{
    const auto data = getMinerDashboardData();
    const auto& identity = data.at("identity");
    const auto& engine = data.at("engine");
    const auto& finds = data.at("finds");
    const auto& delivery = data.at("delivery");
    const auto& gpus = data.at("gpus");
    const std::size_t screenWidth = terminalWidth();
    const std::size_t rainWidth = screenWidth >= 100 ? std::min<std::size_t>(24, screenWidth / 5) : 0;
    const std::size_t width = rainWidth > 0 ? screenWidth - rainWidth - 1 : screenWidth;
    const std::size_t height = terminalHeight();
    const std::size_t inner = width - 2;
    const double totalRate = engine.at("total_hashrate").get<double>();
    const std::uint64_t difficulty = engine.at("difficulty").get<std::uint64_t>();
    const double workRate = totalRate * static_cast<double>(difficulty) / 1'000'000.0;
    const std::string network = delivery.at("network").get<std::string>();
    const char* networkColor = network == "online" ? kGreen : (network == "probing" ? kCyan : kAmber);

    std::vector<std::string> lines;
    lines.push_back(std::string(inner + 2, ' '));
    lines.push_back(std::string(kCyan) + fit("  HASHHEAD // TREEMINER", inner + 2) + kReset);
    lines.push_back(std::string(kDim) + fit("  TERMINAL OPS :: " + identity.at("name").get<std::string>(), inner + 2) + kReset);
    lines.push_back(std::string(inner + 2, ' '));
    lines.push_back(divider(width, "ENGINE"));
    lines.push_back(panelLine(width));
    lines.push_back(panelLine(width, columns({"RATE", "WORK RATE", "DIFFICULTY", "UPTIME"}, inner)));
    std::ostringstream workValue;
    workValue << std::fixed << std::setprecision(1) << workRate << " M-units/s";
    lines.push_back(panelLine(width, columns({
        rate(totalRate),
        workValue.str(),
        std::to_string(difficulty),
        duration(engine.at("uptime_seconds").get<std::int64_t>())
    }, inner)));
    lines.push_back(panelLine(width, columns({
        "GPU  " + rate(engine.at("gpu_hashrate").get<double>()) + " / " +
            std::to_string(engine.at("cuda_streams").get<std::size_t>()) + " streams",
        "CPU  " + rate(engine.at("cpu_hashrate").get<double>()) + " / " +
            std::to_string(engine.at("cpu_workers").get<std::size_t>()) + " workers"
    }, inner)));
    lines.push_back(panelLine(width));
    lines.push_back(divider(width, "DELIVERY"));
    lines.push_back(panelLine(width));
    lines.push_back(panelLine(width, columns({"NETWORK", "LAST UPLINK", "Q_XNM", "Q_XUNI"}, inner)));
    lines.push_back("|" + std::string(networkColor) + columns({
        network,
        delivery.at("last_submission").get<std::string>(),
        std::to_string(delivery.at("queued_xnm").get<std::size_t>()),
        std::to_string(delivery.at("queued_xuni").get<std::size_t>())
    }, inner) + kReset + "|");
    lines.push_back(panelLine(width, columns({
        "SESSION XNM  " + std::to_string(finds.at("xnm").get<std::size_t>()),
        "SESSION XUNI  " + std::to_string(finds.at("xuni").get<std::size_t>()),
        "SUPER  " + std::to_string(finds.at("super").get<std::size_t>()),
        "REJECTED  " + std::to_string(finds.at("rejected").get<std::size_t>())
    }, inner)));
    lines.push_back(panelLine(width));
    lines.push_back(divider(width, "COMPUTE"));
    lines.push_back(panelLine(width));

    if (gpus.empty()) {
        lines.push_back(panelLine(width, "  No active GPU telemetry"));
    } else {
        for (const auto& gpu : gpus) {
            std::ostringstream gpuLine;
            gpuLine << " GPU " << gpu.at("index") << " / S" << (gpu.at("stream").get<int>() + 1)
                    << "  " << gpu.at("name").get<std::string>()
                    << "  |  " << rate(gpu.at("hashrate").get<double>())
                    << "  |  VRAM " << std::fixed << std::setprecision(1)
                    << gpu.at("memory_used_percent").get<double>() << "%";
            lines.push_back(panelLine(width, "  " + gpuLine.str()));
        }
    }
    lines.push_back(panelLine(width));
    lines.push_back(divider(width, "EVENTS"));

    const std::size_t fixedRows = lines.size() + 2;
    const std::size_t eventRows = height > fixedRows ? height - fixedRows : 3;
    std::deque<std::string> events;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        events = events_;
    }
    const std::size_t start = events.size() > eventRows ? events.size() - eventRows : 0;
    for (std::size_t i = start; i < events.size(); ++i) {
        const char* color = events[i].find("CONFIRMED") != std::string::npos ? kGreen
            : events[i].find("RETRY") != std::string::npos || events[i].find("PARKED") != std::string::npos ? kAmber
            : events[i].find("FAILED") != std::string::npos || events[i].find("REJECTED") != std::string::npos ? kRed
            : kReset;
        lines.push_back("|" + std::string(color) + fit(" " + events[i], inner) + kReset + "|");
    }
    while (lines.size() + 1 < height) lines.push_back("|" + std::string(inner, ' ') + "|");

    std::string footer = " Ctrl-C stop  |  " + getConsoleUrl(bindAddress_) + "  |  " + identity.at("name").get<std::string>()
        + "  |  " + identity.at("address").get<std::string>();
    lines.push_back("+" + std::string(inner, '-') + "+");
    if (lines.size() < height) lines.push_back(std::string(kDim) + fit(footer, width) + kReset);
    else lines.back() = std::string(kDim) + fit(footer, width) + kReset;

    std::ostringstream frame;
    frame << "\033[H";
    for (std::size_t i = 0; i < std::min(lines.size(), height); ++i) {
        frame << lines[i];
        if (rainWidth > 0) {
            frame << kDim << kGreen << " " << matrixRow(rainWidth, i, frame_) << kReset;
        }
        if (i + 1 < height) frame << '\n';
    }
    frame << "\033[J";
    ConsoleLog::writeRaw(frame.str());
    ++frame_;
}

} // namespace treeminer
