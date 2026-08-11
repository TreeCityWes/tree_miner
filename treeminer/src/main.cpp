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
                              StatCallback statCallback)
{
    backend.activate();

    while (running)
    {
        // difficulty + margin: the unit mines, sizes its batch, and bakes m= from this one
        // value (MiningCommon.cpp). A margin change breaks the loop and rebuilds the unit.
        MineUnit unit(backend, effectiveMiningDifficulty(), submitCallback, statCallback);
        int rc = unit.runMineLoop();
        if (rc < 0)
        {
            std::cerr << "Mining loop failed on device #" << backend.getDeviceInfo().index << std::endl;
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
            ("journalPath", po::value<std::string>(), "find journal database file (default: treeminer-journal.db in the working directory)");
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
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
        submissionManager->setDifficultyHintCallback([](std::uint32_t d) {
            std::lock_guard<std::mutex> lock(mtx);
            if (globalDifficulty != static_cast<int>(d)) {
                globalDifficulty = static_cast<int>(d);
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

    Logger logger("log", 1024 * 1024);
    SubmitCallback submitCallback = [&logger, &findJournal, &submissionManager, &fallbackSink](const std::string &hexsalt, const std::string &key, const std::string &hashed_pure, const std::uint32_t memory_cost, const size_t attempts, const float hashrate) {

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
        std::int64_t journalId = -1;
        try {
            journalId = findJournal->append(payload);
            journaled = true;
        } catch (const treeminer::JournalError& e) {
            // Last line of defense: the fsync'd fallback sink. Its failure domain is
            // deliberately disjoint from SQLite's (no locks, no WAL, plain O_APPEND), so
            // most journal failures still end in durable capture. The next boot imports
            // the sink back into the journal (idempotent by key).
            const bool sunk = fallbackSink.append(payload);
            if (sunk) {
                ConsoleLog::event(ConsoleLog::Level::Warn, "JOURNAL",
                                  "write failed; find captured in fallback sink | " +
                                  fallbackSink.path() + " | " + std::string(e.what()));
                logger.log("JOURNAL WRITE FAILED, fallback sink OK error=" +
                           std::string(e.what()));
            } else {
                // Both durability paths failed — this find really is at risk, and the
                // disk itself is the prime suspect. Loudest severity we have.
                ConsoleLog::event(ConsoleLog::Level::Error, "JOURNAL",
                                  "write failed AND fallback sink failed — find at risk | " +
                                  std::string(e.what()));
                logger.log("JOURNAL WRITE FAILED, fallback sink FAILED error=" +
                           std::string(e.what()));
            }
        }
        logger.log("found id=" + (journaled ? std::to_string(journalId) : std::string("none")) +
                   " kind=" + treeminer::to_string(payload.kind) +
                   " mined_m=" + std::to_string(payload.memory_cost) +
                   (journaled ? " journaled" : " NOT-JOURNALED"));

        int observedDifficulty = 0;
        {
            std::lock_guard<std::mutex> lock(mtx);
            observedDifficulty = globalDifficulty;
        }
        std::ostringstream findMessage;
        findMessage << "id=";
        if (journaled) {
            findMessage << journalId;
        } else {
            findMessage << "none";
        }
        findMessage << " | kind=" << treeminer::to_string(payload.kind)
                    << " | mined_m=" << payload.memory_cost
                    << " | observed_m=" << observedDifficulty
                    << " | margin="
                    << (static_cast<std::int64_t>(payload.memory_cost) -
                        static_cast<std::int64_t>(observedDifficulty))
                    << " | journaled=" << (journaled ? "yes" : "no");

        std::string findClass;
        if (payload.kind == treeminer::FindKind::XEN11) {
            size_t capitalCount = std::count_if(hashed_pure.begin(), hashed_pure.end(), [](unsigned char c) { return std::isupper(c); });
            if (capitalCount >= 50) {
                findClass = "superblock";
                globalSuperBlockCount++;
            } else {
                findClass = "normal";
                globalNormalBlockCount++;
            }
        } else {
            findClass = "xuni";
            globalXuniBlockCount++;
        }
        findMessage << " | class=" << findClass;
        ConsoleLog::event(journaled ? ConsoleLog::Level::Found : ConsoleLog::Level::Error,
                          treeminer::to_string(payload.kind), findMessage.str());

        if (submissionManager) {
            submissionManager->notifyFindAppended();
        }
    };

    StatCallback statCallback = [](const gpuInfo gpuinfo)
    {
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            globalGpuInfos[gpuinfo.index] = {gpuinfo, std::chrono::steady_clock::now()};
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
            int gpuCount = 0;
            for (const auto &kv : globalGpuInfos)
            {
                auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - kv.second.second);
                if (duration.count() > 2)
                {
                    continue;
                }
                gpuCount++;
                const gpuInfo &info = kv.second.first;
                totalHashCount += info.hashCount;
                totalHashrate += info.hashrate;
            }

            std::ostringstream stream;
            auto elapsed_time = chrono::system_clock::now() - start_time;
            auto hours = chrono::duration_cast<chrono::hours>(elapsed_time).count();
            auto minutes = chrono::duration_cast<chrono::minutes>(elapsed_time).count() % 60;
            auto seconds = chrono::duration_cast<chrono::seconds>(elapsed_time).count() % 60;
            stream << "\033[2K\r";
            stream << std::fixed << std::setprecision(2) << totalHashrate / 1000.0f
                   << " kH/s | " << globalHashCount << " hashes | ";
            if (hours > 0) {
                stream << hours << ":";
            }
            stream  << std::setw(2) << std::setfill('0') << minutes << ":";
            stream << std::setw(2) << std::setfill('0') << seconds << ", ";
            stream << " | " << gpuCount << " GPU" << (gpuCount == 1 ? "" : "s");
            if(globalSuperBlockCount > 0) {
                stream << " | " << RED << "super " << globalSuperBlockCount << RESET;
            }
            if(globalNormalBlockCount > 0) {
                stream << " | " << GREEN << "blocks " << globalNormalBlockCount << RESET;
            }
            if(globalXuniBlockCount > 0) {
                stream << " | " << YELLOW << "xuni " << globalXuniBlockCount << RESET;
            }
            SubmissionLineStats submissionStats;
            if (globalSubmissionLineStatsProvider &&
                globalSubmissionLineStatsProvider(submissionStats)) {
                stream << " | submit " << submissionStats.submitted;
                if (submissionStats.resubmitted > 0) {
                    stream << " (retry " << submissionStats.resubmitted << ")";
                }
                stream << " | confirmed " << submissionStats.confirmed;
                if (submissionStats.accepted_unconfirmed > 0) {
                    stream << " | unconfirmed " << submissionStats.accepted_unconfirmed;
                }
                if (submissionStats.transport_failures > 0) {
                    stream << " | failures " << submissionStats.transport_failures;
                }
                if (submissionStats.pool_down) {
                    stream << " | " << RED << "pool DOWN" << RESET;
                }
            }
            stream << " | diff " << difficulty;
            std::string logMessage = stream.str();
            Logger::logToConsole(logMessage);
        }
    };

    std::size_t i = 0;
    for (auto &device : devices)
    {
        if(usedDevices.find(i) != usedDevices.end()){
            std::cout << "Device #" << i << ": "
                    << device.getName() << std::endl;
        }
        i++;
    }
    start_time = std::chrono::system_clock::now();

    std::map<int, std::unique_ptr<ComputeBackend>> backends;
    for (auto deviceIndex : usedDevices) {
        backends[deviceIndex] = std::make_unique<CudaBackend>(static_cast<int>(deviceIndex));
    }

    for (auto deviceIndex : usedDevices) {
        std::thread t(runMiningOnDevice, std::ref(*backends[deviceIndex]), submitCallback, statCallback);
        t.detach();
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

    setupRoutes(findJournal.get(), submissionManager.get());
    std::thread serverThread(startServer);
    serverThread.detach();
    if(!donotupload){
        std::thread uploadStatThread(UploadDataPeriodically, 60);
        uploadStatThread.detach();
    }

    while (running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (globalPlatformManager) {
        globalPlatformManager->stop();
        globalPlatformManager.reset();
    }

    std::cout << std::endl;
    return 0;
}
