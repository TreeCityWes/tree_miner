#include <iostream>
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
#include "journal/FindJournal.h"
#include "submit/HttpTransport.h"
#include "submit/SubmissionManager.h"
#include "StatReporter.h"
#include "TerminalUi.h"
#include "LocalServer.h"
#include "BlockSubmitter.h"
#include "hashapi/HashApiCli.h"

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
std::string globalTestBlockPattern = "";

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
        MineUnit unit(backend, globalDifficulty, submitCallback, statCallback, streamIndex);
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
    std::string displayMode = "logs";

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
            ("cudaStreams", po::value<int>(), "independent CUDA work streams per device (1-2)")
            ("cpuWorkers", po::value<int>(), "independent CPU sidecar mining workers (0 disables)")
            ("display", po::value<std::string>(), "terminal display: logs, terminal, or prompt");
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
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
            globalTestBlockPattern = vm["testBlockPattern"].as<std::string>();
            std::cout << "Test block pattern override: " << globalTestBlockPattern << std::endl;
        }

        if(vm.count("batchSize")){
            globalMaxBatchSize = vm["batchSize"].as<int>();
            std::cout << "Max batch size override: " << globalMaxBatchSize << std::endl;
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

        AppConfig appConfig(CONFIG_FILENAME);
        if (!isTestFixedDiff) {
            if(!vm.count("minerAddr") || !vm.count("totalDevFee")){
                appConfig.load();
            } else {
                appConfig.tryLoad();
            }
            globalUserAddress = appConfig.getAccountAddress();
            globalEcoDevfeeAddress = appConfig.getEcoDevAddr();
            globalDevfeePermillage = appConfig.getDevfeePermillage();
        } else {
            globalUserAddress = "0x0000000000000000000000000000000000000000";
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
            globalUserAddress = userAddr;
            std::cout << "Miner address: " << userAddr << "\n";
        }

        EthereumAddressValidator validator;

        if (!globalEcoDevfeeAddress.empty() && !validator.isValid(globalEcoDevfeeAddress)){
            std::cerr << "The argument (" << globalEcoDevfeeAddress << ") for ecosystem developer fee address (EIP55) is invalid." << std::endl;
            return -1;
        }
        if (!validator.isValid(globalUserAddress)){
            std::cerr << "The argument (" << globalUserAddress << ") for miner address (EIP55) is invalid." << std::endl;
            return -1;
        }
        if (globalDevfeePermillage < 0 || globalDevfeePermillage > 1000) {
            std::cerr << "The argument (" << globalDevfeePermillage << ") for total developer fee (0-1000) is invalid." << std::endl;
            return -1;
        }

        signal(SIGINT, interruptSignalHandler);

        if (vm.count("saveConfig")) {
            appConfig.setAccountAddress(globalUserAddress);
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

        std::cout << GREEN << "Logged in as " << globalUserAddress
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

    // --- TreeMiner journal-first pipeline (replaces the upstream in-RAM closure queue) ---
    // Every find is durably journaled before any network attempt; the SubmissionManager
    // drains the journal with outage-aware retry, parking, and /get_block confirmation.
    Logger logger("log", 1024 * 1024);
    std::unique_ptr<treeminer::FindJournal> findJournal;
    std::unique_ptr<treeminer::HttpTransport> findTransport;
    std::unique_ptr<treeminer::SubmissionManager> submissionManager;
    try {
        findJournal = std::make_unique<treeminer::FindJournal>("treeminer-journal.db");
    } catch (const treeminer::JournalError& e) {
        std::cerr << RED << "FATAL: cannot open find journal: " << e.what() << RESET << std::endl;
        return -1;
    }
    {
        auto rec = findJournal->recoverOnStartup();
        const auto counts = findJournal->counts();
        globalQueuedXnm = counts.queued_xen11;
        globalQueuedXuni = counts.queued_xuni;
        std::cout << "Journal recovered: " << rec.pending << " pending, "
                  << rec.accepted_unconfirmed << " awaiting confirmation, "
                  << (rec.parked_difficulty + rec.parked_xuni) << " parked, "
                  << rec.acked << " acked, " << rec.quarantined << " quarantined" << std::endl;
    }
    if (!isTestFixedDiff) {
        findTransport = std::make_unique<treeminer::HttpTransport>(globalRpcLink, machineId);
        submissionManager = std::make_unique<treeminer::SubmissionManager>(*findJournal, *findTransport);
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
                Logger::logToConsole("\033[2K\r" + message.str() + "\n");
            });
        submissionManager->setNetworkStateCallback(
            [](treeminer::CircuitBreaker::State state) {
                globalNetworkState = state;
            });
        submissionManager->setDifficultyHintCallback([](std::uint32_t d) {
            std::lock_guard<std::mutex> lock(mtx);
            if (globalDifficulty != static_cast<int>(d)) {
                globalDifficulty = static_cast<int>(d);
                Logger::logToConsole("Difficulty updated from server hint: " +
                                     std::to_string(d) + "\n");
            }
        });
        globalDifficultyObserver = [&manager = *submissionManager](std::uint32_t d) {
            manager.observeDifficulty(d);
        };
        submissionManager->start();
    }

    if (!isTestFixedDiff) {
        // Seed from the local cache so a restart during a server outage mines at the
        // last known real difficulty instead of the 42069 fallback (~50x wasted work).
        int cachedDifficulty = loadCachedDifficulty();
        if (cachedDifficulty > 0) {
            globalDifficulty = cachedDifficulty;
            std::cout << "Seeded difficulty from local cache: " << cachedDifficulty << std::endl;
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

    SubmitCallback submitCallback = [&logger, &findJournal, &submissionManager](const std::string &hexsalt, const std::string &key, const std::string &hashed_pure, const std::uint32_t memory_cost, const size_t attempts, const float hashrate, const std::string &source) {

        // Immutable payload capture: the PHC string is assembled once from the parameters the
        // GPU batch actually used. Upstream re-hashed with globalDifficulty at submit time and
        // silently dropped the find if difficulty had ticked in between.
        const std::string hashed_data = treeminer::assemblePhc(memory_cost, hexsalt, hashed_pure);

        if (globalPlatformManager && globalPlatformManager->isRunning()) {
            globalPlatformManager->onBlockFound(hashed_data, key, "0x" + hexsalt, attempts, hashrate);
        }

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
        try {
            findJournal->append(payload);
            journaled = true;
        } catch (const treeminer::JournalError& e) {
            Logger::logToConsole(std::string(RED) +
                                 "CRITICAL - find could not be secured: " + e.what() +
                                 RESET + "\n");
            logger.log("JOURNAL WRITE FAILED key=" + key + " error=" + e.what());
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
        logger.log("found source=" + source + " key=" + key + " hash=" + hashed_data + (journaled ? " journaled" : " NOT-JOURNALED"));

        std::ostringstream findMessage;
        findMessage << source << " ";
        if (payload.kind == treeminer::FindKind::XEN11) {
            size_t capitalCount = std::count_if(hashed_pure.begin(), hashed_pure.end(), [](unsigned char c) { return std::isupper(c); });
            if (capitalCount >= 50) {
                findMessage << GREEN << "Superblock found!" << RESET;
                globalSuperBlockCount++;
            } else {
                findMessage << GREEN << "Normalblock found!" << RESET;
                globalNormalBlockCount++;
            }
        } else {
            findMessage << YELLOW << "Xuni found!" << RESET;
            globalXuniBlockCount++;
        }
        findMessage << (journaled
            ? " Secured locally; uplink queued."
            : " LOCAL SAVE FAILED; not queued.");
        Logger::logToConsole("\033[2K\r" + findMessage.str() + "\n");

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
            stream << "\033[2K\r"
                   << "Mining: " << combinedHashCount << " [";
            if (hours > 0) {
                stream << hours << ":";
            }
            stream  << std::setw(2) << std::setfill('0') << minutes << ":";
            stream << std::setw(2) << std::setfill('0') << seconds << ", ";
            stream << "GPU:" << activeDevices.size();
            if (streamCount > 1) {
                stream << " streams:" << streamCount;
            }
            if (cpuStats.active_workers > 0) {
                stream << ", CPU:" << cpuStats.active_workers
                       << " " << std::fixed << std::setprecision(2)
                       << cpuStats.hashrate / 1000.0 << " kH/s";
            }
            stream << ", ";
            if(globalSuperBlockCount > 0) {
                stream << RED  << " super:" << globalSuperBlockCount<< RESET << ", " ;
            }
            if(globalNormalBlockCount > 0) {
                stream << GREEN << "normal:"  << globalNormalBlockCount << RESET << ", " ;
            }
            if(globalXuniBlockCount > 0) {
                stream << YELLOW << "xuni:"  << globalXuniBlockCount << RESET << ", " ;
            }
            stream << "Q_XNM:" << globalQueuedXnm
                   << " Q_XUNI:" << globalQueuedXuni
                   << " net:" << networkStateLabel(globalNetworkState)
                   << " last:" << submissionStateLabel(globalLastSubmission) << ", ";
            const double combinedHashrate = static_cast<double>(totalHashrate) + cpuStats.hashrate;
            stream << std::fixed << std::setprecision(2) << combinedHashrate / 1000.0
                   << " kH/s total, D:" << difficulty << "]";
            if (!terminalUi) {
                Logger::logToConsole(stream.str());
            }
        }
    };

    if (displayMode == "terminal") {
        terminalUi = std::make_unique<treeminer::TerminalUi>();
        terminalUi->start();
        Logger::setConsoleSink([ui = terminalUi.get()](const std::string& message) {
            ui->postEvent(message);
        });
    }

    if (cpuWorkerCount > 0) {
        constexpr std::size_t kCpuMiningBatchSize = 64;
        auto cpuWorkSequence = std::make_shared<std::atomic<std::uint64_t>>(0);
        treeminer::CpuMiningWorker::Config cpuConfig{cpuWorkerCount, kCpuMiningBatchSize};
        cpuMiningWorker = std::make_unique<treeminer::CpuMiningWorker>(
            cpuConfig,
            [] { return static_cast<std::uint32_t>(globalDifficulty.load()); },
            [cpuWorkSequence] {
                treeminer::CpuMiningWorker::Work work;
                const MiningContext ctx = MiningCoordinator::getInstance().getContext();
                if (ctx.mode == MiningMode::PLATFORM_MINING) {
                    work.salt_hex = ctx.address.substr(0, 2) == "0x"
                        ? ctx.address.substr(2)
                        : ctx.address;
                    work.key_prefix = ctx.prefix;
                } else {
                    work.salt_hex = globalUserAddress.substr(2);
                    work.key_prefix = globalSelfMiningPrefix;
                    if (work.key_prefix.empty() && globalDevfeePermillage > 0) {
                        const std::uint64_t slot = cpuWorkSequence->fetch_add(1) % 1000;
                        const std::uint64_t feeStart = 1000 - static_cast<std::uint64_t>(globalDevfeePermillage.load());
                        if (slot >= feeStart) {
                            const bool useEcoFee = !globalEcoDevfeeAddress.empty() &&
                                slot >= 1000 - static_cast<std::uint64_t>(globalDevfeePermillage.load() / 2);
                            work.salt_hex = (useEcoFee ? globalEcoDevfeeAddress : globalDevfeeAddress).substr(2);
                            work.key_prefix = (useEcoFee ? ECODEVFEE_PREFIX : DEVFEE_PREFIX) +
                                globalUserAddress.substr(2);
                        }
                    }
                }
                work.target_pattern = globalTestBlockPattern.empty() ? "XEN11" : globalTestBlockPattern;
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
            globalMqttBroker, globalUserAddress, gpuList);

        if (!globalPlatformManager->start()) {
            std::cerr << RED << "Failed to start PlatformManager. Continuing in self-mining mode." << RESET << std::endl;
            globalPlatformManager.reset();
        } else {
            std::cout << GREEN << "Platform mode enabled. Broker: " << globalMqttBroker << RESET << std::endl;
        }
    }

    setupRoutes();
    std::thread serverThread(startServer);
    Logger::logToConsole("LAN console: " + getConsoleUrl() + "\n");
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

    std::cout << std::endl;
    return 0;
}
