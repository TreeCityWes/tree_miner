#include <iostream>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <string>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <map>
#include <sstream>
#include <set>
#include <cuda_runtime.h>
#include <boost/program_options.hpp>
#include "EthereumAddressValidator.h"
#include "MiningCommon.h"
#include "CudaDevice.h"
#include "CudaBackend.h"
#include "CpuMiningWorker.h"
#include "MineUnit.h"
#include "AppConfig.h"
#include "Logger.h"
#include "ConsoleLog.h"
#include "Argon2idHasher.h"
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "PowSubmitter.h"
#include "SHA256Hasher.h"
#include "RandomHexKeyGenerator.h"
#include "MachineIDGetter.h"
#include "MiningCoordinator.h"
#include "PlatformManager.h"
#include "ProcessMonitor.h"
#include "DifficultyManager.h"
#include "treeminer/PhcAssembler.h"
#include "journal/FallbackSink.h"
#include "journal/FindJournal.h"
#include "submit/HttpTransport.h"
#include "submit/SubmissionManager.h"
#include "StatReporter.h"
#include "TerminalUi.h"
#include "LocalServer.h"
#include "BlockSubmitter.h"
#include "hashapi/HashApiCli.h"
#include "hashapi/HashApiSelfTest.h"
#include "hashapi/CpuHashBackend.h"
#include "hashapi/CudaHashBackend.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;
namespace po = boost::program_options;

std::string globalCustomName = "";
bool globalPlatformMode = false;
std::string globalMqttBroker = "";
std::string globalWorkerId = "";
std::unique_ptr<PlatformManager> globalPlatformManager;

// Difficulty-headroom policy, resolved from config.txt + command line before mining starts
// and handed to the SubmissionManager, which owns the ramp (PLAN §5, §10.7).
static treeminer::MarginConfig globalMarginConfig;

// Journal database path (PLAN §5 `journal_path`), resolved from config.txt + command line.
// The default is CWD-relative for drop-in compatibility with existing deployments, but the
// resolved ABSOLUTE path is logged at startup: a miner launched from an unexpected working
// directory (systemd, HiveOS wrappers) would otherwise silently open a fresh empty journal
// and strand every queued find in the orphaned file.
static std::string globalJournalPath = "treeminer-journal.db";

#ifdef _WIN32
BOOL ctrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
        std::cout << "Ctrl-C event\n";
        ExitProcess(0);
    default:
        return FALSE;
    }
}
#endif

static void interruptSignalHandler(int signum)
{
    running = false;
    if (globalPlatformManager) {
        globalPlatformManager->stop();
    }
    cv.notify_all();
    getApp().stop();
}

std::string getMachineId(string userInputDeviceInfo)
{
    SHA256Hasher hasher;
    try {
        std::string machineId = MachineIDGetter::getMachineId();
        if(machineId.empty()) {
            throw std::runtime_error("Machine ID is empty");
        }
        return hasher.sha256(machineId + userInputDeviceInfo).substr(0, 16);
    }
    catch (const std::exception& e) {
        RandomHexKeyGenerator keyGenerator;
        return hasher.sha256(keyGenerator.nextRandomKey()).substr(0, 16);
    }
}

std::set<int> parseDeviceList(const std::string& deviceListText, int deviceCount) {
    std::set<int> devices;
    std::stringstream ss(deviceListText);
    std::string item;
    while (std::getline(ss, item, ',')) {
        try {
            int deviceId = std::stoi(item);
            if (deviceId < 0 || deviceId >= deviceCount) {
                continue;
            }
            devices.insert(deviceId);
        } catch (const std::invalid_argument& e) {
            continue;
        } catch (const std::out_of_range& e) {
            continue;
        }
    }
    if (devices.empty()) {
        for (int i = 0; i < deviceCount; ++i) {
            devices.insert(i);
        }
    }
    return devices;
}

static void runMiningOnDevice(ComputeBackend& backend,
                              SubmitCallback submitCallback,
                              StatCallback statCallback,
                              int streamIndex)
{
    backend.activate();

    while (running)
    {
        // difficulty + margin: the unit mines, sizes its batch, and bakes m= from this one
        // value (MiningCommon.cpp). A margin change breaks the loop and rebuilds the unit.
        MineUnit unit(backend, effectiveMiningDifficulty(), submitCallback, statCallback,
                      streamIndex);
        int rc = unit.runMineLoop();
        if (rc < 0)
        {
            Logger::logToConsole("Mining loop failed on device #" +
                                 std::to_string(backend.getDeviceInfo().index) + "\n");
            break;
        }
        if (rc > 0)
        {
            // Recoverable (e.g. batch allocation failed); upstream retried instantly,
            // spinning the CPU and flooding the log. Back off before the next attempt.
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

int main(int argc, const char *const *argv)
{
    if (hashapi::isHashApiCommand(argc, argv)) {
        return hashapi::runHashApiCli(argc, argv);
    }

    bool executeTask = false;
    bool donotupload = false;
    static bool isTestFixedDiff = false;
    std::string deviceList = "";
    std::size_t cpuWorkerCount = 0;
    // CPU hashing only pays near the difficulty floor; above this ceiling the workers
    // idle and auto-resume when difficulty falls back (see CpuMiningWorker::Config).
    std::uint32_t cpuMaxDifficulty = 100;
    std::string displayMode = "logs";
    // Reachable on the LAN by default so the operator can open the dashboard from
    // another device (phone/laptop) by browsing to this rig's IP. All dashboard
    // routes are read-only stats; set --dashboard-bind 127.0.0.1 to keep it private.
    std::string dashboardBind = "0.0.0.0";

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--execute") {
            executeTask = true;
            break;
        }
    }
    if(!executeTask){
        int i = 0;
        while(i++ < 42069){
            if (monitorProcess(argc, const_cast<char**>(argv)) >= 0) {
                break;
            }
        }
        return 0;
    }

    try {
        po::options_description desc("XenblocksMiner options");
        desc.add_options()
            ("help,h", "display help information")
            ("totalDevFee", po::value<int>(), "set total developer fee")
            ("ecoDevAddr", po::value<std::string>(), "set ecosystem developer address (will receive half of the total dev fee)")
            ("minerAddr", po::value<std::string>(), "set miner address")
            ("execute", "execute the miner otherwise it will run as a mointor server")
            ("donotupload", "do not upload the data to the server")
            ("device", po::value<std::string>(), "device index list[--device=1,2,7] to run the miner on")
            ("saveConfig", "update configuration file with console inputs")
            ("testFixedDiff", po::value<int>(), "run in test mode with a fixed difficulty")
            ("rpcLink", po::value<std::string>(), "set rpc link")
            ("customName", po::value<std::string>(), "set custom name")
            ("platform-mode", "enable hashpower marketplace platform mode")
            ("mqtt-broker", po::value<std::string>(), "MQTT broker URI for platform mode (e.g. tcp://broker:1883)")
            ("worker-id", po::value<std::string>(), "override worker ID for platform registration")
            ("testBlockPattern", po::value<std::string>(), "override block detection pattern for testing (default: XEN11)")
            ("batchSize", po::value<int>(), "limit GPU batch size (reduces VRAM usage)")
            ("difficultyMarginMode", po::value<std::string>(), "difficulty headroom policy: off (default) | fixed | auto")
            ("difficultyMargin", po::value<int>(), "headroom in KiB; fixed mode: the constant, auto mode: one ramp step (default 1000)")
            ("difficultyMarginMax", po::value<int>(), "auto mode only: ceiling on the headroom ramp in KiB (default 5000)")
            ("journalPath", po::value<std::string>(), "find journal database file (default: treeminer-journal.db in the working directory)")
            ("cudaStreams", po::value<int>(), "independent CUDA work streams per device (1-2)")
            ("cpuWorkers", po::value<int>(), "independent CPU sidecar mining workers (0 disables)")
            ("cpuMaxDifficulty", po::value<int>(), "CPU workers hash only while difficulty <= this ceiling; they idle above it and resume when it falls (default 100; 0 = no ceiling)")
            ("dashboard-bind", po::value<std::string>(), "dashboard listen IP (default: 0.0.0.0 = reachable on your LAN; set 127.0.0.1 to restrict to this machine)")
            ("display", po::value<std::string>(), "terminal display: logs, terminal, or prompt");
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
        }

        if (vm.count("dashboard-bind")) {
            dashboardBind = vm["dashboard-bind"].as<std::string>();
        }

        if (vm.count("display")) {
            displayMode = vm["display"].as<std::string>();
        }
        if (displayMode != "logs" && displayMode != "terminal" && displayMode != "prompt") {
            std::cerr << "The display mode must be logs, terminal, or prompt." << std::endl;
            return -1;
        }
        if (displayMode == "prompt") {
            std::cout << "\nTreeMiner display\n"
                      << "  1. Presentation terminal\n"
                      << "  2. Scrolling logs\n"
                      << "Select [1]: ";
            std::string selection;
            std::getline(std::cin, selection);
            displayMode = selection == "2" ? "logs" : "terminal";
        }

        if(vm.count("testFixedDiff")){
            isTestFixedDiff = true;
            globalDifficulty = vm["testFixedDiff"].as<int>();
        }

        if(vm.count("testBlockPattern")){
            const auto pattern = vm["testBlockPattern"].as<std::string>();
            setTestBlockPattern(pattern);
            std::cout << "Test block pattern override: " << pattern << std::endl;
        }

        if(vm.count("batchSize")){
            globalMaxBatchSize = vm["batchSize"].as<int>();
            std::cout << "Max batch size override: " << globalMaxBatchSize << std::endl;
        }

        // --- difficulty margin (PLAN §5, §10.7) ---
        // config.txt supplies the defaults; command-line flags override. An unparseable value
        // is fatal rather than ignored: silently mining at the wrong memory cost would show up
        // only as unexplained 401s hours later.
        {
            ConfigManager marginConfig(CONFIG_FILENAME);
            marginConfig.loadConfig();
            auto configuredValue = [&marginConfig](const std::string& key) {
                return marginConfig.getConfigValue(key);
            };

            std::string modeText = configuredValue("difficulty_margin_mode");
            if (vm.count("difficultyMarginMode")) {
                modeText = vm["difficultyMarginMode"].as<std::string>();
            }
            if (!modeText.empty() && !treeminer::parseMarginMode(modeText, globalMarginConfig.mode)) {
                std::cerr << "Invalid difficulty margin mode '" << modeText
                          << "' (expected: off | fixed | auto)." << std::endl;
                return -1;
            }

            auto readPositiveInt = [&](const std::string& key, const char* flag,
                                       std::uint32_t& target) -> bool {
                std::string text = configuredValue(key);
                if (vm.count(flag)) {
                    text = std::to_string(vm[flag].as<int>());
                }
                if (text.empty()) {
                    return true;
                }
                try {
                    const long long value = std::stoll(text);
                    if (value < 0 || value > 100000000LL) {
                        std::cerr << "Value for " << key << " (" << text
                                  << ") is out of range (0-100000000)." << std::endl;
                        return false;
                    }
                    target = static_cast<std::uint32_t>(value);
                    return true;
                } catch (const std::exception&) {
                    std::cerr << "Value for " << key << " (" << text
                              << ") is not a number." << std::endl;
                    return false;
                }
            };
            if (!readPositiveInt("difficulty_margin", "difficultyMargin",
                                 globalMarginConfig.margin_kib) ||
                !readPositiveInt("difficulty_margin_max", "difficultyMarginMax",
                                 globalMarginConfig.max_kib)) {
                return -1;
            }

            if (globalMarginConfig.mode != treeminer::MarginMode::Off) {
                std::cout << "Difficulty margin: mode=" << treeminer::to_string(globalMarginConfig.mode)
                          << " step=" << globalMarginConfig.margin_kib << " KiB";
                if (globalMarginConfig.mode == treeminer::MarginMode::Auto) {
                    std::cout << " max=" << globalMarginConfig.max_kib << " KiB";
                } else {
                    // Fixed headroom is paid for on every single hash, forever.
                    globalDifficultyMargin = static_cast<int>(globalMarginConfig.margin_kib);
                }
                std::cout << std::endl;
            }

            // --- journal path (PLAN §5 `journal_path`) ---
            // Same precedence as the margin keys: config.txt supplies the default, the
            // command line overrides. An empty value keeps the compatibility default.
            {
                std::string pathText = configuredValue("journal_path");
                if (vm.count("journalPath")) {
                    pathText = vm["journalPath"].as<std::string>();
                }
                if (!pathText.empty()) {
                    globalJournalPath = pathText;
                }
            }

            // Dashboard is LAN-reachable by default; config.txt can override the bind
            // (e.g. to 127.0.0.1 for a private console). Command line wins over config.
            if (!vm.count("dashboard-bind")) {
                const std::string configuredBind = configuredValue("dashboard_bind");
                if (!configuredBind.empty()) {
                    dashboardBind = configuredBind;
                }
            }
        }

        if (!isValidDashboardBind(dashboardBind)) {
            std::cerr << "Invalid dashboard bind address '" << dashboardBind
                      << "': expected an IPv4 or IPv6 address.\n";
            return -1;
        }

        if (vm.count("cudaStreams")) {
            const int requestedStreams = vm["cudaStreams"].as<int>();
            if (requestedStreams < 1 || requestedStreams > 2) {
                std::cerr << "The argument (" << requestedStreams
                          << ") for CUDA streams must be 1 or 2." << std::endl;
                return -1;
            }
            globalCudaStreamsPerDevice = static_cast<std::size_t>(requestedStreams);
            std::cout << "CUDA streams per device: " << globalCudaStreamsPerDevice << std::endl;
        }

        if (vm.count("cpuWorkers")) {
            const int requestedWorkers = vm["cpuWorkers"].as<int>();
            const unsigned int logicalThreads = std::thread::hardware_concurrency();
            if (requestedWorkers < 0 ||
                (logicalThreads > 0 && requestedWorkers > static_cast<int>(logicalThreads))) {
                std::cerr << "The argument (" << requestedWorkers
                          << ") for CPU workers must be between 0 and "
                          << (logicalThreads > 0 ? logicalThreads : 256) << "." << std::endl;
                return -1;
            }
            cpuWorkerCount = static_cast<std::size_t>(requestedWorkers);
            std::cout << "CPU sidecar workers: " << cpuWorkerCount << std::endl;
        }

        if (vm.count("cpuMaxDifficulty")) {
            const int requestedCeiling = vm["cpuMaxDifficulty"].as<int>();
            if (requestedCeiling < 0 || requestedCeiling > 100000000) {
                std::cerr << "The argument (" << requestedCeiling
                          << ") for the CPU difficulty ceiling must be 0-100000000." << std::endl;
                return -1;
            }
            cpuMaxDifficulty = static_cast<std::uint32_t>(requestedCeiling);
        }

        AppConfig appConfig(CONFIG_FILENAME);
        if (!isTestFixedDiff) {
            if(!vm.count("minerAddr") || !vm.count("totalDevFee")){
                appConfig.load();
            } else {
                appConfig.tryLoad();
            }
            setMiningUserAddress(appConfig.getAccountAddress());
            globalEcoDevfeeAddress = appConfig.getEcoDevAddr();
            globalDevfeePermillage = appConfig.getDevfeePermillage();
        } else {
            setMiningUserAddress("0x0000000000000000000000000000000000000000");
            globalEcoDevfeeAddress = "0x0000000000000000000000000000000000000000";
            globalDevfeePermillage = 0;
        }

        if (vm.count("totalDevFee")) {
            int totalDevFee = vm["totalDevFee"].as<int>();
            if (totalDevFee < 0 || totalDevFee > 1000) {
                std::cerr << "The argument (" << totalDevFee << ") for total developer fee (0-1000) is invalid." << std::endl;
                return -1;
            }
            globalDevfeePermillage = totalDevFee;
            std::cout << "Total developer fee set to: " << vm["totalDevFee"].as<int>() << "\n";
        }
        if (vm.count("ecoDevAddr")) {
            std::string ecoDevAddr = vm["ecoDevAddr"].as<std::string>();
            EthereumAddressValidator validator;
            if (!validator.isValid(ecoDevAddr)){
                std::cerr << "The argument (" << ecoDevAddr << ") for ecosystem developer fee address (EIP55) is invalid." << std::endl;
                return -1;
            }
            globalEcoDevfeeAddress = ecoDevAddr;
            std::cout << "Ecosystem developer fee address: " << ecoDevAddr << "\n";
        }
        if (vm.count("minerAddr")) {
            std::string userAddr = vm["minerAddr"].as<std::string>();
            EthereumAddressValidator validator;
            if (!validator.isValid(userAddr)){
                std::cerr << "The argument (" << userAddr << ") for miner address (EIP55) is invalid." << std::endl;
                return -1;
            }
            setMiningUserAddress(userAddr);
            std::cout << "Miner address: " << userAddr << "\n";
        }

        EthereumAddressValidator validator;

        if (!globalEcoDevfeeAddress.empty() && !validator.isValid(globalEcoDevfeeAddress)){
            std::cerr << "The argument (" << globalEcoDevfeeAddress << ") for ecosystem developer fee address (EIP55) is invalid." << std::endl;
            return -1;
        }
        const auto configuredIdentity = miningIdentitySnapshot();
        if (!validator.isValid(configuredIdentity->userAddress)){
            std::cerr << "The argument (" << configuredIdentity->userAddress << ") for miner address (EIP55) is invalid." << std::endl;
            return -1;
        }
        if (globalDevfeePermillage < 0 || globalDevfeePermillage > 1000) {
            std::cerr << "The argument (" << globalDevfeePermillage << ") for total developer fee (0-1000) is invalid." << std::endl;
            return -1;
        }

        signal(SIGINT, interruptSignalHandler);

        if (vm.count("saveConfig")) {
            appConfig.setAccountAddress(configuredIdentity->userAddress);
            if(!globalEcoDevfeeAddress.empty()){
                appConfig.setEcoDevAddr(globalEcoDevfeeAddress);
            }
            appConfig.setDevfeePermillage(globalDevfeePermillage);
            appConfig.save();
            std::cout << "Configuration file updated with console inputs." << std::endl;
        }

        if (vm.count("donotupload")) {
            donotupload = true;
        }

        if(vm.count("device")){
            deviceList = vm["device"].as<std::string>();
        }

        if (vm.count("rpcLink")) {
            globalRpcLink = vm["rpcLink"].as<std::string>();
        }

        if (vm.count("customName")) {
            globalCustomName = vm["customName"].as<std::string>();
        }

        if (vm.count("platform-mode")) {
            globalPlatformMode = true;
        }
        if (vm.count("mqtt-broker")) {
            globalMqttBroker = vm["mqtt-broker"].as<std::string>();
        }
        if (vm.count("worker-id")) {
            globalWorkerId = vm["worker-id"].as<std::string>();
        }

        if (globalPlatformMode && globalMqttBroker.empty()) {
            std::cerr << "Platform mode requires --mqtt-broker to be set." << std::endl;
            return -1;
        }

        std::cout << "RPC Link: " << globalRpcLink << std::endl;

        std::cout << GREEN << "Logged in as " << miningIdentitySnapshot()->userAddress
        << ". Devfee set at " << globalDevfeePermillage << "/1000."
        << ((globalDevfeePermillage != 0 && !globalEcoDevfeeAddress.empty()) ? " Ecosystem devfee address: " + globalEcoDevfeeAddress : "")
        << RESET << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Parameter parsing error: " << e.what() << "\n";
        return -1;
    } catch (...) {
        std::cerr << "Unknown error!\n";
        return -1;
    }

    int deviceCount;
    cudaError_t cudaStatus = cudaGetDeviceCount(&deviceCount);
    if (cudaStatus != cudaSuccess)
    {
        std::cerr << "cudaGetDeviceCount failed! Do you have a CUDA-capable GPU installed?" << std::endl;
        return -1;
    }

    auto devices = CudaDevice::getAllDevices();
    std::set<int> usedDevices = parseDeviceList(deviceList, CudaDevice::getAllDevices().size());
    std::ostringstream oss_usedDevices;
    for(auto iter = usedDevices.begin(); iter != usedDevices.end(); ++iter) {
        oss_usedDevices << *iter << ",";
    }
    machineId = getMachineId(oss_usedDevices.str());
    if (!globalWorkerId.empty()) {
        machineId = globalWorkerId;
    }
    std::cout << "Machine ID: " << machineId << std::endl;

    // Fail closed before starting journals, network threads, dashboards, or mining. A
    // deterministic reference comparison catches a broken CUDA kernel even when its output
    // happens to contain XEN11 and would otherwise look like a valuable find.
    try {
        hashapi::CpuHashBackend cpuReference;
        for (const int deviceIndex : usedDevices) {
            CudaBackend selfTestDevice(deviceIndex);
            hashapi::CudaHashBackend cudaCandidate(selfTestDevice);
            const hashapi::HashApiSelfTestResult selfTest = hashapi::runCpuCudaSelfTest(
                cpuReference,
                cudaCandidate,
                deviceIndex,
                hashapi::kGpuFirstBlocksEnabled);
            if (!selfTest.ok) {
                std::cerr << "FATAL: GPU #" << deviceIndex
                          << " failed startup Argon2 CPU/CUDA self-test: " << selfTest.error
                          << ". Mining was not started." << std::endl;
                return EXIT_FAILURE;
            }
            std::cout << "GPU #" << deviceIndex
                      << " Argon2 CPU/CUDA self-test passed." << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "FATAL: startup Argon2 CPU/CUDA self-test could not run: "
                  << e.what() << ". Mining was not started." << std::endl;
        return EXIT_FAILURE;
    }

    // --- TreeMiner journal-first pipeline (replaces the upstream in-RAM closure queue) ---
    // Every find is durably journaled before any network attempt; the SubmissionManager
    // drains the journal with outage-aware retry, parking, and /get_block confirmation.
    Logger logger("log", 1024 * 1024);
    std::unique_ptr<treeminer::FindJournal> findJournal;
    std::unique_ptr<treeminer::HttpTransport> findTransport;
    std::unique_ptr<treeminer::SubmissionManager> submissionManager;
    // Last line of defense (audit finding A): when FindJournal::append throws, the find
    // falls into this append-only fsync'd JSONL sink instead of being dropped, and the
    // next boot drains it back into the journal (idempotent by key). Constructed
    // unconditionally — it is inert until the first failure.
    treeminer::FallbackSink fallbackSink(globalJournalPath + ".fallback.jsonl");
    try {
        findJournal = std::make_unique<treeminer::FindJournal>(globalJournalPath);
    } catch (const treeminer::JournalError& e) {
        ConsoleLog::event(ConsoleLog::Level::Error, "JOURNAL",
                          "cannot open " + globalJournalPath + " | " + e.what());
        return -1;
    }
    {
        // Log the RESOLVED absolute path. A relative path silently depends on the CWD the
        // process happened to start in; when a service launches from an unexpected directory
        // this line is the difference between "why is my journal empty" and an instant
        // diagnosis (the old queued finds are sitting in the other directory's file).
        std::error_code pathEc;
        const auto absolutePath = std::filesystem::absolute(globalJournalPath, pathEc);
        ConsoleLog::event(ConsoleLog::Level::Info, "JOURNAL",
                          "path=" + (pathEc ? globalJournalPath : absolutePath.string()));

        // Drain the last-resort sink BEFORE recovery so any find that fell into it while
        // SQLite was broken is counted and drained like every other journaled find this
        // boot. Import is idempotent by key; the file only exists if a previous run hit a
        // journal write failure, so file_present alone is worth a warning.
        const auto sinkStats = treeminer::FallbackSink::importInto(
            *findJournal, globalJournalPath + ".fallback.jsonl");
        if (sinkStats.file_present) {
            std::ostringstream drained;
            drained << "fallback sink drained | imported=" << sinkStats.imported
                    << " | malformed=" << sinkStats.malformed
                    << " — a previous run could not write the journal; investigate why";
            ConsoleLog::event(sinkStats.malformed > 0 ? ConsoleLog::Level::Error
                                                      : ConsoleLog::Level::Warn,
                              "JOURNAL", drained.str());
        }

        auto rec = findJournal->recoverOnStartup();
        // Seed the terminal status line's queued counters from the recovered journal.
        const auto counts = findJournal->counts();
        globalQueuedXnm = counts.queued_xen11;
        globalQueuedXuni = counts.queued_xuni;
        std::ostringstream recovered;
        recovered << "recovered"
                  << " | pending=" << rec.pending
                  << " | unconfirmed=" << rec.accepted_unconfirmed
                  << " | parked=" << (rec.parked_difficulty + rec.parked_xuni)
                  << " | acked=" << rec.acked
                  << " | quarantined=" << rec.quarantined;
        ConsoleLog::event(ConsoleLog::Level::Info, "JOURNAL", recovered.str());
    }
    if (!isTestFixedDiff) {
        findTransport = std::make_unique<treeminer::HttpTransport>(globalRpcLink, machineId);
        treeminer::SubmissionManager::Config submitConfig;
        submitConfig.margin = globalMarginConfig;
        submissionManager = std::make_unique<treeminer::SubmissionManager>(
            *findJournal, *findTransport, submitConfig);
        // The drain thread's own unrecoverable-journal detection converges on the SAME fatal
        // state as the submit callback's double-failure path. The callback fires once, ON the
        // submission thread; declareFatalDurabilityFailure only sets atomics (flag + reason +
        // running=false), never joins, so it is safe here (per the setFatalCallback contract).
        submissionManager->setFatalCallback([](const std::string& reason) {
            declareFatalDurabilityFailure("submission drain thread halted: " + reason);
        });
        // The ramp publishes here; the mine loop picks it up on its next batch boundary.
        submissionManager->setMarginCallback([](std::uint32_t kib) {
            const int previous = globalDifficultyMargin.exchange(static_cast<int>(kib));
            if (previous != static_cast<int>(kib)) {
                std::ostringstream message;
                message << previous << " -> " << kib
                        << " | effective_m=" << effectiveMiningDifficulty()
                        << " | headroom costs proportional hashrate";
                ConsoleLog::event(ConsoleLog::Level::Info, "MARGIN", message.str());
            }
        });
        submissionManager->setOutcomeCallback(
            [&logger, &findJournal](const treeminer::FindRecord& record,
                      const treeminer::Classification& classification,
                      std::optional<int> httpStatus) {
                const char* outcome = "UPLINK UPDATED";
                std::string detail = classification.reason;
                switch (classification.next_status) {
                    case treeminer::FindStatus::Acked:
                        outcome = "UPLINK CONFIRMED";
                        detail = "server record verified";
                        globalLastSubmission = LastSubmissionState::Accepted;
                        break;
                    case treeminer::FindStatus::AcceptedUnconfirmed:
                        outcome = "UPLINK ACCEPTED";
                        detail = "confirmation pending";
                        globalLastSubmission = LastSubmissionState::Unconfirmed;
                        break;
                    case treeminer::FindStatus::Pending:
                        outcome = "UPLINK RETRY";
                        if (classification.reason.rfind("transport failure", 0) == 0) {
                            detail = "network unavailable; retry scheduled";
                        }
                        globalLastSubmission = LastSubmissionState::Retry;
                        break;
                    case treeminer::FindStatus::ParkedDifficulty:
                    case treeminer::FindStatus::ParkedXuniWindow:
                        outcome = "UPLINK PARKED";
                        globalLastSubmission = LastSubmissionState::Parked;
                        break;
                    case treeminer::FindStatus::Quarantined:
                        outcome = "UPLINK QUARANTINED";
                        globalLastSubmission = LastSubmissionState::Failed;
                        break;
                    case treeminer::FindStatus::Dead:
                    case treeminer::FindStatus::PermanentlyInvalid:
                        outcome = "UPLINK REJECTED";
                        globalLastSubmission = LastSubmissionState::Failed;
                        break;
                    default:
                        break;
                }
                try {
                    const auto counts = findJournal->counts();
                    globalQueuedXnm = counts.queued_xen11;
                    globalQueuedXuni = counts.queued_xuni;
                } catch (const treeminer::JournalError&) {
                    // Outcome persistence succeeded; retain the last display count.
                }
                constexpr std::size_t kMaxDetailLength = 64;
                if (detail.length() > kMaxDetailLength) {
                    detail.resize(kMaxDetailLength - 3);
                    detail += "...";
                }

                std::ostringstream message;
                message << outcome
                        << " [" << treeminer::to_string(record.payload.kind) << "]"
                        << " id=" << record.id;
                if (httpStatus) {
                    message << " HTTP=" << *httpStatus;
                }
                message << " - " << detail;
                logger.log(message.str());
            });
        submissionManager->setNetworkStateCallback(
            [](treeminer::CircuitBreaker::State state) {
                globalNetworkState = state;
            });
        submissionManager->setDifficultyHintCallback([](std::uint32_t d) {
            std::lock_guard<std::mutex> lock(mtx);
            if (globalDifficulty != static_cast<int>(d)) {
                globalDifficulty = static_cast<int>(d);
                ConsoleLog::event(ConsoleLog::Level::Info, "DIFFICULTY",
                                  "updated from server hint | current=" +
                                      std::to_string(d));
            }
        });
        globalDifficultyObserver = [&manager = *submissionManager](std::uint32_t d) {
            manager.observeDifficulty(d);
        };
        globalTreeminerStatsProvider = [&manager = *submissionManager,
                                        &journal = *findJournal](TreeminerStats& out) {
            out.difficulty = globalDifficulty.load();
            out.margin_in_effect = static_cast<int>(manager.marginInEffect());
            out.effective_difficulty = effectiveMiningDifficulty();
            out.margin_mode = treeminer::to_string(globalMarginConfig.mode);
            switch (manager.breakerState()) {
                case treeminer::CircuitBreaker::State::Closed:   out.breaker_state = "up"; break;
                case treeminer::CircuitBreaker::State::Open:     out.breaker_state = "down"; break;
                case treeminer::CircuitBreaker::State::HalfOpen: out.breaker_state = "half-open"; break;
            }
            out.outage_ms = static_cast<long long>(manager.outageDurationMs());
            out.drain_rate_per_second = manager.drainRatePerSecond();
            const treeminer::IFindJournal::Counts counts = journal.counts();
            out.pending = counts.pending;
            out.parked = counts.parked;
            out.quarantined = counts.quarantined;
            out.acked_total = counts.acked_total;
            out.dead_total = counts.dead_total;
            out.accepted_unconfirmed = counts.accepted_unconfirmed;
            out.permanently_invalid = counts.permanently_invalid;
            return true;
        };
        globalSubmissionLineStatsProvider = [&manager = *submissionManager](SubmissionLineStats& out) {
            const auto metrics = manager.metrics();
            out.submitted = metrics.submitted;
            out.resubmitted = metrics.resubmitted;
            out.confirmed = metrics.acked;
            out.accepted_unconfirmed = metrics.accepted_unconfirmed;
            out.transport_failures = metrics.transport_failures;
            out.pool_down = manager.outageDurationMs() > 0 ||
                            globalDifficultyEndpointDown.load();
            return true;
        };
        submissionManager->start();
    }

    if (!isTestFixedDiff) {
        // Seed from the local cache so a restart during a server outage mines at the
        // last known real difficulty instead of the 42069 fallback (~50x wasted work).
        int cachedDifficulty = loadCachedDifficulty();
        if (cachedDifficulty > 0) {
            globalDifficulty = cachedDifficulty;
            ConsoleLog::event(ConsoleLog::Level::Info, "DIFFICULTY",
                              "seeded from cache | current=" +
                                  std::to_string(cachedDifficulty));
        } else {
            globalDifficulty = 42069;
        }
        updateDifficulty();
        std::thread difficultyThread(updateDifficultyPeriodically);
        difficultyThread.detach();
    } else {
        std::cout << "Running in TEST MODE with fixed difficulty " << globalDifficulty << std::endl;
    }

    std::thread uploadThread(uploadGpuInfos);
    uploadThread.detach();

    SubmitCallback submitCallback = [&logger, &findJournal, &submissionManager, &fallbackSink](const std::string &hexsalt, const std::string &key, const std::string &hashed_pure, const std::uint32_t memory_cost, const size_t attempts, const float hashrate, const std::string &source) {

        // Immutable payload capture: the PHC string is assembled once from the parameters the
        // GPU batch actually used. Upstream re-hashed with globalDifficulty at submit time and
        // silently dropped the find if difficulty had ticked in between.
        const std::string hashed_data = treeminer::assemblePhc(memory_cost, hexsalt, hashed_pure);

        // (Platform reporting deliberately does NOT happen here — see the durablyCaptured
        // block below. Review finding 5: journal-first means nothing that can block,
        // throw, race shutdown, or put the key on the wire runs before local persistence.)

        treeminer::FoundPayload payload;
        payload.key = key;
        payload.hash_to_verify = hashed_data;
        payload.account = "0x" + hexsalt;
        payload.kind = hashed_pure.find("XEN11") != std::string::npos ? treeminer::FindKind::XEN11
                                                                     : treeminer::FindKind::XUNI;
        payload.memory_cost = memory_cost;
        payload.worker = machineId;
        payload.attempts = attempts;
        payload.hashes_per_second = hashrate;
        payload.found_at_utc = treeminer::SubmissionManager::isoUtc(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // Journal-first: durable before any network attempt. A journal failure is the one
        // event this miner must never be quiet about.
        bool journaled = false;
        bool sunk = false;
        std::int64_t journalId = -1;
        try {
            journalId = findJournal->append(payload);
            journaled = true;
        } catch (const treeminer::JournalError& e) {
            // Last line of defense: the fsync'd fallback sink. Its failure domain is
            // deliberately disjoint from SQLite's (no locks, no WAL, plain O_APPEND), so
            // most journal failures still end in durable capture. The next boot imports
            // the sink back into the journal (idempotent by key).
            sunk = fallbackSink.append(payload);
            if (sunk) {
                ConsoleLog::event(ConsoleLog::Level::Warn, "JOURNAL",
                                  "write failed; find captured in fallback sink | " +
                                  fallbackSink.path() + " | " + std::string(e.what()));
                logger.log("JOURNAL WRITE FAILED, fallback sink OK error=" +
                           std::string(e.what()));
            } else {
                // Both durability paths failed — this find really is at risk, and the
                // disk itself is the prime suspect. A miner that cannot persist finds is
                // destroying every future find it makes, so this is FATAL (review
                // finding 6): declare the state, which stops the mining loops via
                // `running`; main() then exits NONZERO so a supervisor (systemd
                // Restart=always) restarts us against a hopefully-recovered disk.
                ConsoleLog::event(ConsoleLog::Level::Error, "JOURNAL",
                                  "write failed AND fallback sink failed — find at risk; "
                                  "HALTING miner (exit nonzero for supervisor restart) | " +
                                  std::string(e.what()));
                logger.log("JOURNAL WRITE FAILED, fallback sink FAILED — FATAL, "
                           "stopping miner error=" + std::string(e.what()));
                declareFatalDurabilityFailure(
                    "journal append and fallback sink both failed: " +
                    std::string(e.what()));
            }
        }
        // The one fact the rest of this callback keys off: does this find exist anywhere
        // durable? Journal and sink are equally acceptable — the sink imports back into
        // the journal on the next boot.
        const bool durablyCaptured = journaled || sunk;

        // Review finding 5: platform reporting strictly AFTER durable capture. MQTT can
        // block, throw, or race shutdown, and it puts the key on the wire — none of which
        // may precede local persistence. And a find that reached NEITHER durability path
        // is not reported at all: advertising a block the disk never received would have
        // the platform (and its consumer) accounting for value that no longer exists.
        if (durablyCaptured && globalPlatformManager && globalPlatformManager->isRunning()) {
            globalPlatformManager->onBlockFound(hashed_data, key, "0x" + hexsalt, attempts, hashrate);
        }

        if (journaled) {
            try {
                const auto counts = findJournal->counts();
                globalQueuedXnm = counts.queued_xen11;
                globalQueuedXuni = counts.queued_xuni;
            } catch (const treeminer::JournalError&) {
                // The durable append succeeded; the next outcome refreshes the display count.
            }
        }
        logger.log("found id=" + (journaled ? std::to_string(journalId) : std::string("none")) +
                   " source=" + source +
                   " kind=" + treeminer::to_string(payload.kind) +
                   " mined_m=" + std::to_string(payload.memory_cost) +
                   (journaled ? " journaled" : " NOT-JOURNALED"));

        std::ostringstream findMessage;
        findMessage << "#";
        if (journaled) {
            findMessage << journalId;
        } else if (sunk) {
            findMessage << "fallback";
        } else {
            findMessage << "none";
        }
        findMessage << "  •  " << source << "  •  m=" << payload.memory_cost;

        std::string findClass;
        if (payload.kind == treeminer::FindKind::XEN11) {
            size_t capitalCount = std::count_if(hashed_pure.begin(), hashed_pure.end(), [](unsigned char c) { return std::isupper(c); });
            findClass = capitalCount >= 50 ? "superblock" : "normal";
        } else {
            findClass = "xuni";
        }
        // Call out a superblock — it is worth far more than a normal block, so it should be
        // impossible to miss scrolling the log. Normal/xuni are already clear from the kind.
        if (findClass == "superblock") {
            findMessage << "  •  " << RED << "SUPERBLOCK" << RESET;
        }
        // Review finding 6: the lifetime counters mean "finds this run that still
        // exist". A find that reached neither the journal nor the sink is gone; counting
        // it would let the status line and dashboards claim value the disk never
        // received.
        if (durablyCaptured) {
            if (findClass == "superblock") {
                globalSuperBlockCount++;
            } else if (findClass == "normal") {
                globalNormalBlockCount++;
            } else {
                globalXuniBlockCount++;
            }
        }
        if (journaled) {
            findMessage << "  •  saved locally  •  queued";
        } else if (sunk) {
            findMessage << "  •  saved to fallback";
        } else {
            // Must not read as "handled": nothing durable holds this find, and the
            // fatal state declared above is about to take the whole miner down.
            findMessage << "  •  SAVE FAILED  •  stopping";
        }
        ConsoleLog::event(journaled ? ConsoleLog::Level::Found
                                    : (sunk ? ConsoleLog::Level::Warn
                                            : ConsoleLog::Level::Error),
                          treeminer::to_string(payload.kind), findMessage.str());

        if (submissionManager) {
            submissionManager->notifyFindAppended();
        }
    };

    std::unique_ptr<treeminer::CpuMiningWorker> cpuMiningWorker;
    std::unique_ptr<treeminer::TerminalUi> terminalUi;

    StatCallback statCallback = [&cpuMiningWorker, &terminalUi](const gpuInfo gpuinfo)
    {
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            const int statsKey = gpuinfo.index * 16 + gpuinfo.streamIndex;
            globalGpuInfos[statsKey] = {gpuinfo, std::chrono::steady_clock::now()};
        }
        int difficulty = 40404;
        {
            std::lock_guard<std::mutex> lock(mtx);
            difficulty = globalDifficulty;
        }
        size_t totalHashCount = 0;
        float totalHashrate = 0.0;

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            std::set<int> activeDevices;
            int streamCount = 0;
            for (const auto &kv : globalGpuInfos)
            {
                auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - kv.second.second);
                if (duration.count() > 2)
                {
                    continue;
                }
                const gpuInfo &info = kv.second.first;
                activeDevices.insert(info.index);
                streamCount++;
                totalHashCount += info.hashCount;
                totalHashrate += info.hashrate;
            }

            std::ostringstream stream;
            auto elapsed_time = chrono::system_clock::now() - start_time;
            auto hours = chrono::duration_cast<chrono::hours>(elapsed_time).count();
            auto minutes = chrono::duration_cast<chrono::minutes>(elapsed_time).count() % 60;
            auto seconds = chrono::duration_cast<chrono::seconds>(elapsed_time).count() % 60;
            const auto cpuStats = cpuMiningWorker
                ? cpuMiningWorker->stats()
                : treeminer::CpuMiningWorker::Stats{};
            globalCpuWorkers = cpuStats.active_workers;
            globalCpuHashrate = cpuStats.hashrate;
            const std::uint64_t combinedHashCount =
                static_cast<std::uint64_t>(globalHashCount.load()) + cpuStats.attempts;
            stream << "\033[2K\r";
            stream << std::fixed << std::setprecision(1)
                   << (totalHashrate + cpuStats.hashrate) / 1000.0f
                   << " kH/s  •  " << activeDevices.size() << " GPU"
                   << (activeDevices.size() == 1 ? "" : "s") << "  •  ";
            if (combinedHashCount >= 1'000'000) {
                stream << std::fixed << std::setprecision(1)
                       << combinedHashCount / 1'000'000.0 << "M hashes";
            } else if (combinedHashCount >= 1'000) {
                stream << std::fixed << std::setprecision(1)
                       << combinedHashCount / 1'000.0 << "K hashes";
            } else {
                stream << combinedHashCount << " hashes";
            }
            stream << "  •  ";
            if (hours > 0) {
                stream << hours << ":";
            }
            stream  << std::setw(2) << std::setfill('0') << minutes << ":";
            stream << std::setw(2) << std::setfill('0') << seconds;
            if (streamCount > static_cast<int>(activeDevices.size())) {
                stream << "  •  " << streamCount << " streams";
            }
            if (cpuStats.active_workers > 0) {
                if (cpuStats.paused_for_difficulty) {
                    stream << "  •  CPU idle";
                } else {
                    stream << "  •  CPU " << cpuStats.active_workers;
                }
            }
            if(globalSuperBlockCount > 0) {
                stream << "  •  " << RED << globalSuperBlockCount << " super" << RESET;
            }
            if(globalNormalBlockCount > 0) {
                stream << "  •  " << GREEN << globalNormalBlockCount << " block"
                       << (globalNormalBlockCount == 1 ? "" : "s") << RESET;
            }
            if(globalXuniBlockCount > 0) {
                stream << "  •  " << YELLOW << globalXuniBlockCount << " XUNI" << RESET;
            }
            const std::size_t queued = globalQueuedXnm.load() + globalQueuedXuni.load();
            if (queued > 0) stream << "  •  " << queued << " queued";
            SubmissionLineStats submissionStats;
            if (globalSubmissionLineStatsProvider &&
                globalSubmissionLineStatsProvider(submissionStats)) {
                if (submissionStats.confirmed > 0) {
                    stream << "  •  " << submissionStats.confirmed << " confirmed";
                }
                if (submissionStats.accepted_unconfirmed > 0) {
                    stream << "  •  " << submissionStats.accepted_unconfirmed << " unconfirmed";
                }
                // The one indicator that must never disappear on this line: an outage means
                // finds are piling up in the journal, not being lost — but the operator has
                // to be able to SEE that at a glance.
                if (submissionStats.pool_down) {
                    stream << "  •  " << RED << "pool DOWN" << RESET;
                }
            }
            stream << "  •  diff " << difficulty;
            if (!terminalUi) {
                ConsoleLog::progress(stream.str());
            }
        }
    };

    if (displayMode == "terminal") {
        terminalUi = std::make_unique<treeminer::TerminalUi>();
        terminalUi->setBindAddress(dashboardBind);
        terminalUi->start();
        Logger::setConsoleSink([ui = terminalUi.get()](const std::string& message) {
            ui->postEvent(message);
        });
    }

    if (cpuWorkerCount > 0) {
        constexpr std::size_t kCpuMiningBatchSize = 64;
        auto cpuWorkSequence = std::make_shared<std::atomic<std::uint64_t>>(0);
        treeminer::CpuMiningWorker::Config cpuConfig{cpuWorkerCount, kCpuMiningBatchSize,
                                                     cpuMaxDifficulty};
        if (cpuMaxDifficulty > 0) {
            std::cout << "CPU workers hash only at difficulty <= " << cpuMaxDifficulty
                      << " (idle above; auto-resume)" << std::endl;
        }
        cpuMiningWorker = std::make_unique<treeminer::CpuMiningWorker>(
            cpuConfig,
            [] { return static_cast<std::uint32_t>(globalDifficulty.load()); },
            [cpuWorkSequence] {
                treeminer::CpuMiningWorker::Work work;
                const MiningContext ctx = MiningCoordinator::getInstance().getContext();
                const auto identity = miningIdentitySnapshot();
                if (ctx.mode == MiningMode::PLATFORM_MINING) {
                    work.salt_hex = ctx.address.substr(0, 2) == "0x"
                        ? ctx.address.substr(2)
                        : ctx.address;
                    work.key_prefix = ctx.prefix;
                } else {
                    work.salt_hex = identity->userAddress.substr(2);
                    work.key_prefix = identity->selfMiningPrefix;
                    if (work.key_prefix.empty() && globalDevfeePermillage > 0) {
                        const std::uint64_t slot = cpuWorkSequence->fetch_add(1) % 1000;
                        const std::uint64_t feeStart = 1000 - static_cast<std::uint64_t>(globalDevfeePermillage.load());
                        if (slot >= feeStart) {
                            const bool useEcoFee = !globalEcoDevfeeAddress.empty() &&
                                slot >= 1000 - static_cast<std::uint64_t>(globalDevfeePermillage.load() / 2);
                            work.salt_hex = (useEcoFee ? globalEcoDevfeeAddress : globalDevfeeAddress).substr(2);
                            work.key_prefix = (useEcoFee ? ECODEVFEE_PREFIX : DEVFEE_PREFIX) +
                                identity->userAddress.substr(2);
                        }
                    }
                }
                work.target_pattern = identity->testBlockPattern.empty() ? "XEN11" : identity->testBlockPattern;
                work.allow_xuni = isWithinXuniWindow();
                return work;
            },
            submitCallback,
            [] { return running.load(); });
        cpuMiningWorker->start();
    }

    std::size_t i = 0;
    for (auto &device : devices)
    {
        if(usedDevices.find(i) != usedDevices.end()){
            Logger::logToConsole("Device #" + std::to_string(i) + ": " + device.getName() + "\n");
        }
        i++;
    }
    start_time = std::chrono::system_clock::now();

    std::vector<std::unique_ptr<ComputeBackend>> backends;
    std::vector<int> backendStreamIndexes;
    for (auto deviceIndex : usedDevices) {
        for (std::size_t streamIndex = 0; streamIndex < globalCudaStreamsPerDevice; ++streamIndex) {
            backends.push_back(std::make_unique<CudaBackend>(static_cast<int>(deviceIndex)));
            backendStreamIndexes.push_back(static_cast<int>(streamIndex));
        }
    }

    std::vector<std::thread> miningThreads;
    miningThreads.reserve(backends.size());
    for (std::size_t worker = 0; worker < backends.size(); ++worker) {
        miningThreads.emplace_back(runMiningOnDevice,
                                   std::ref(*backends[worker]),
                                   submitCallback,
                                   statCallback,
                                   backendStreamIndexes[worker]);
    }

    if (globalPlatformMode) {
        std::vector<gpuInfo> gpuList;
        for (auto idx : usedDevices) {
            gpuInfo gi;
            gi.index = static_cast<int>(idx);
            gi.name = devices[idx].getName();
            size_t freeMem, totalMem;
            cudaSetDevice(idx);
            cudaMemGetInfo(&freeMem, &totalMem);
            gi.memory = static_cast<int>(std::round(static_cast<float>(totalMem) / (1024 * 1024 * 1024)));
            gi.busId = devices[idx].getPicBusId();
            gi.usingMemory = 0;
            gi.temperature = 0;
            gi.hashrate = 0;
            gi.hashCount = 0;
            gpuList.push_back(gi);
        }

        globalPlatformManager = std::make_unique<PlatformManager>(
            globalMqttBroker, miningIdentitySnapshot()->userAddress, gpuList);

        if (!globalPlatformManager->start()) {
            std::cerr << RED << "Failed to start PlatformManager. Continuing in self-mining mode." << RESET << std::endl;
            globalPlatformManager.reset();
        } else {
            std::cout << GREEN << "Platform mode enabled. Broker: " << globalMqttBroker << RESET << std::endl;
        }
    }

    setupRoutes(findJournal.get(), submissionManager.get());
    std::thread serverThread(startServer, dashboardBind);
    Logger::logToConsole("Dashboard ready — open " + getConsoleUrl(dashboardBind) +
                         " in a browser on your network"
                         + (dashboardBind == "127.0.0.1" || dashboardBind == "::1"
                                ? " (private: this machine only)"
                                : "") + "\n");
    serverThread.detach();
    if(!donotupload){
        std::thread uploadStatThread(UploadDataPeriodically, 60);
        uploadStatThread.detach();
    }

    while (running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (cpuMiningWorker) {
        cpuMiningWorker->stop();
        cpuMiningWorker->join();
    }

    for (auto& miningThread : miningThreads) {
        if (miningThread.joinable()) {
            miningThread.join();
        }
    }

    if (terminalUi) {
        Logger::clearConsoleSink();
        terminalUi->stop();
    }

    if (globalPlatformManager) {
        globalPlatformManager->stop();
        globalPlatformManager.reset();
    }

    if (globalFatalDurabilityFailure.load()) {
        // The submit callback proved a find could be persisted by NEITHER the journal
        // NOR the fallback sink (review finding 6). Exit NONZERO so a supervisor
        // (systemd Restart=always) brings the miner back up against a possibly-recovered
        // disk instead of leaving a "running" process that destroys every find it makes.
        // On the Ctrl-C path the signal handler already stops the web server; no signal
        // fired here, so stop it before returning or the detached crow thread would race
        // static destruction.
        getApp().stop();
        std::cerr << RED << "FATAL: durability failure — " << fatalDurabilityFailureReason()
                  << " — exiting nonzero for supervisor restart" << RESET << std::endl;
        return 2;
    }

    std::cout << std::endl;
    return 0;
}
