#include "PlatformManager.h"

#include <iostream>
#include <chrono>

#include "ConfigManager.h"
#include "ConsoleLog.h"
#include "EthereumAddressValidator.h"

namespace {

std::int64_t nowEpochSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// A short, log-safe description of a command for rejection messages. Field values
// are re-validated by handlers; here we only need something greppable that cannot
// forge log lines (identifier charset only, bounded length).
std::string describeCommand(const nlohmann::json& msg)
{
	std::string command = msg.is_object() && msg.contains("command") && msg["command"].is_string()
		? msg["command"].get<std::string>() : "";
	std::string action = msg.is_object() && msg.contains("action") && msg["action"].is_string()
		? msg["action"].get<std::string>() : "";
	std::string label = !command.empty() ? command : action;
	if (label.empty()) return "<unknown>";
	if (!platform::isSafeIdentifier(label, 1, 48)) {
		// Anything outside the conservative charset is attacker-controlled junk;
		// don't echo it into the console.
		return "<unparseable>";
	}
	return command.empty() ? "action:" + label : label;
}

}  // namespace

const char* platformStateToString(PlatformState state)
{
	switch (state) {
		case PlatformState::IDLE:      return "IDLE";
		case PlatformState::AVAILABLE: return "AVAILABLE";
		case PlatformState::LEASED:    return "LEASED";
		case PlatformState::MINING:    return "MINING";
		case PlatformState::COMPLETED: return "COMPLETED";
		case PlatformState::ERROR_STATE: return "ERROR";
		default:                       return "UNKNOWN";
	}
}

PlatformManager::PlatformManager(const std::string& broker_uri,
								 const std::string& eth_address,
								 const std::vector<gpuInfo>& gpus)
	: mqtt_(std::make_shared<MqttClient>(broker_uri, machineId)),
	  reporter_(mqtt_),
	  eth_address_(eth_address),
	  gpus_(gpus),
	  expected_worker_id_(machineId)
{
	// Secret ingestion (security review finding 2): read config.txt ourselves so no
	// main.cpp change is required — main.cpp is owned elsewhere. setCommandSecret()
	// (e.g. a future --platformSecret flag) overrides this value if called before
	// start().
	ConfigManager config(CONFIG_FILENAME);
	config.loadConfig();
	const std::string secret = config.getConfigValue("platform_command_secret");
	if (!secret.empty()) {
		command_secret_ = secret;
	}
}

PlatformManager::~PlatformManager()
{
	stop();
	// stop() skips the self-join when a signed remote "shutdown" command invoked it
	// from the dispatch thread itself; finish that join here so no thread outlives
	// the members it touches (finding 9: use-after-free after destruction).
	if (dispatch_thread_.joinable()) dispatch_thread_.join();
}

void PlatformManager::setCommandSecret(std::string secret)
{
	std::lock_guard<std::mutex> lock(secret_mutex_);
	command_secret_ = std::move(secret);
}

bool PlatformManager::start()
{
	if (running_) return true;

	// Connect to MQTT broker
	if (!mqtt_->connect()) {
		std::cerr << RED << "PlatformManager: Failed to connect to MQTT broker" << RESET << std::endl;
		transitionTo(PlatformState::ERROR_STATE);
		return false;
	}

	running_ = true;

	// One-time SECURITY posture notice. Without a secret, the miner keeps the
	// legacy marketplace flow working (lease assignment, pause/resume) but refuses
	// every mutating command — see authorizeCommand().
	{
		std::lock_guard<std::mutex> lock(secret_mutex_);
		if (command_secret_.empty()) {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"SECURITY: no platform_command_secret configured — MQTT commands are "
				"UNAUTHENTICATED. Lease assignment and pause/resume remain enabled; "
				"payout-address/difficulty/prefix/pattern changes and remote shutdown "
				"are DISABLED until a shared secret is set in config.txt "
				"(platform_command_secret=...).");
		}
	}

	// Subscribe to incoming command topics
	mqtt_->subscribe(mqtt_->buildTopic(MqttClient::TOPIC_TASK));
	mqtt_->subscribe(mqtt_->buildTopic(MqttClient::TOPIC_CONTROL));

	// Message handler: ONLY enqueue (bounded, non-blocking) off the Paho callback
	// thread. Dispatch happens on our single owned worker thread — never on the
	// broker's thread (publish-from-callback deadlock) and never on detached
	// threads (finding 9: unbounded threads, use-after-free, reordering).
	mqtt_->setMessageCallback([this](const std::string& topic, const std::string& payload) {
		enqueueCommand(topic, payload);
	});

	// Start the command dispatch worker before anything can be enqueued for long.
	dispatch_thread_ = std::thread(&PlatformManager::dispatchLoop, this);

	// Send registration
	reporter_.sendRegistration(eth_address_, gpus_);

	// Transition to AVAILABLE (will be confirmed by register_ack)
	// For now, go AVAILABLE optimistically; handleRegisterAck confirms it
	transitionTo(PlatformState::AVAILABLE);

	// Start heartbeat thread
	heartbeat_thread_ = std::thread(&PlatformManager::heartbeatLoop, this);

	// Start lease watchdog thread
	watchdog_thread_ = std::thread(&PlatformManager::leaseWatchdogLoop, this);

	std::cout << GREEN << "PlatformManager: Started" << RESET << std::endl;
	return true;
}

void PlatformManager::stop()
{
	// exchange() makes stop() idempotent and race-free when invoked concurrently
	// (e.g. signal handler in main and a signed remote shutdown command).
	if (!running_.exchange(false)) return;

	// Sever the MQTT callback FIRST so the broker thread can no longer enter code
	// that captures `this`. MqttClient holds its callback mutex while invoking, so
	// once this returns no old callback is still running.
	mqtt_->setMessageCallback(nullptr);

	// Wake the dispatch worker so it observes running_ == false. Any commands
	// still queued are deliberately dropped — obeying remote commands during
	// shutdown would race the teardown below.
	queue_cv_.notify_all();

	// If in a lease, clean up
	if (lease_manager_.hasActiveLease()) {
		lease_manager_.endLease();
		switchToSelfMining();
	}

	// Wait for threads
	if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
	if (watchdog_thread_.joinable()) watchdog_thread_.join();

	// Join the dispatch worker — unless stop() is running ON the dispatch worker
	// (remote "shutdown" command path): a thread cannot join itself. In that case
	// the loop exits as soon as the current handler returns, and the destructor
	// performs the final join before any member is destroyed.
	if (dispatch_thread_.joinable() &&
		dispatch_thread_.get_id() != std::this_thread::get_id()) {
		dispatch_thread_.join();
	}

	// Report offline status and disconnect
	reporter_.sendStatusUpdate("offline");
	mqtt_->disconnect();

	transitionTo(PlatformState::IDLE);
	std::cout << "PlatformManager: Stopped" << std::endl;
}

PlatformState PlatformManager::getState() const
{
	return state_.load();
}

bool PlatformManager::isRunning() const
{
	return running_;
}

void PlatformManager::onBlockFound(const std::string& hash,
								   const std::string& key,
								   const std::string& account,
								   size_t attempts,
								   float hashrate)
{
	PlatformState current = state_.load();
	if (current == PlatformState::MINING) {
		auto lease = lease_manager_.getLease();
		if (!lease.has_value()) return;

		lease_manager_.recordBlock();
		reporter_.sendBlockFound(lease->lease_id, hash, key, account, attempts, hashrate);
	} else if (current == PlatformState::AVAILABLE) {
		reporter_.sendBlockFound("", hash, key, account, attempts, hashrate);
	}
}

void PlatformManager::setStateChangeCallback(StateChangeCallback cb)
{
	std::lock_guard<std::mutex> lock(cb_mutex_);
	state_change_cb_ = std::move(cb);
}

// --- State Transitions ---

void PlatformManager::transitionTo(PlatformState new_state)
{
	PlatformState old_state = state_.exchange(new_state);
	if (old_state == new_state) return;

	std::cout << "PlatformManager: " << platformStateToString(old_state)
			  << " -> " << platformStateToString(new_state) << std::endl;

	// Report state change to platform
	if (mqtt_->isConnected()) {
		auto lease = lease_manager_.getLease();
		std::string lease_id = lease.has_value() ? lease->lease_id : "";
		reporter_.sendStatusUpdate(platformStateToString(new_state), lease_id);
	}

	// Invoke external callback
	{
		std::lock_guard<std::mutex> lock(cb_mutex_);
		if (state_change_cb_) {
			state_change_cb_(old_state, new_state);
		}
	}
}

// --- Command Queue (finding 9) ---

void PlatformManager::enqueueCommand(std::string topic, std::string payload)
{
	// Size gate BEFORE the queue: an attacker with broker access must not be able
	// to park megabytes per slot or feed the JSON parser unbounded input.
	const bool oversized = payload.size() > platform::kMaxPayloadBytes;

	std::lock_guard<std::mutex> lock(queue_mutex_);
	if (!running_) return;  // shutting down: nothing may touch the queue anymore

	if (oversized || command_queue_.size() >= COMMAND_QUEUE_CAPACITY) {
		++dropped_commands_;
		// Throttle the warning so the log itself cannot become the DoS vector;
		// the running total keeps every drop accounted for.
		const auto now = std::chrono::steady_clock::now();
		if (last_drop_log_.time_since_epoch().count() == 0 ||
			now - last_drop_log_ >= std::chrono::seconds(5)) {
			last_drop_log_ = now;
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				std::string(oversized ? "oversized MQTT payload dropped"
									  : "command queue full — dropping newest") +
				" (total dropped: " + std::to_string(dropped_commands_) + ")");
		}
		return;  // drop NEWEST: already-accepted commands keep their FIFO order
	}

	command_queue_.push_back(QueuedCommand{std::move(topic), std::move(payload)});
	queue_cv_.notify_one();
}

void PlatformManager::dispatchLoop()
{
	// Single consumer; commands run strictly in FIFO arrival order.
	while (true) {
		QueuedCommand cmd;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait(lock, [this] { return !running_ || !command_queue_.empty(); });
			if (!running_) return;
			cmd = std::move(command_queue_.front());
			command_queue_.pop_front();
		}
		// onMessage catches everything internally — an exception must never cross
		// this thread boundary (it would terminate the process).
		onMessage(cmd.topic, cmd.payload);
	}
}

// --- Command Authentication (finding 2) ---

bool PlatformManager::authorizeCommand(const nlohmann::json& msg)
{
	std::string secret;
	{
		std::lock_guard<std::mutex> lock(secret_mutex_);
		secret = command_secret_;
	}

	if (!secret.empty()) {
		// Secret configured: EVERY command must carry a valid envelope. nonce_cache_
		// is only touched here, on the dispatch thread — no lock needed.
		const platform::VerifyStatus status = platform::verifyEnvelope(
			msg, secret, expected_worker_id_, nowEpochSeconds(), nonce_cache_);
		if (status != platform::VerifyStatus::Ok) {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"rejected command '" + describeCommand(msg) + "': " +
				platform::verifyStatusName(status));
			return false;
		}
		return true;
	}

	// No secret configured (legacy deployment). Keep the historical non-mutating
	// marketplace flow working so live operators are not broken, but refuse every
	// command that could redirect payouts, change mining parameters, or kill the
	// miner. The one-time warning at start() explains how to enable signing.
	if (platform::isMutatingCommand(msg)) {
		ConsoleLog::event(ConsoleLog::Level::Error, "platform",
			"REFUSED unsigned mutating command '" + describeCommand(msg) +
			"' — set platform_command_secret in config.txt to enable signed control");
		return false;
	}
	return true;
}

// --- MQTT Message Dispatch ---

void PlatformManager::onMessage(const std::string& topic, const std::string& payload)
{
	(void)topic;  // authorization is envelope-based; the topic adds no trust
	try {
		auto msg = nlohmann::json::parse(payload);
		if (!msg.is_object()) {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"ignored non-object MQTT message");
			return;
		}

		if (!authorizeCommand(msg)) return;

		std::string command = msg.value("command", "");
		if (command.size() > 64) {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"ignored message with oversized command field");
			return;
		}

		if (command == "register_ack") {
			handleRegisterAck(msg);
		} else if (command == "assign_task") {
			handleAssignTask(msg);
		} else if (command == "release") {
			handleRelease(msg);
		} else {
			handleControl(msg);
		}
	} catch (const nlohmann::json::exception& e) {
		std::cerr << YELLOW << "PlatformManager: Invalid JSON message: " << e.what() << RESET << std::endl;
	} catch (const std::exception& e) {
		// Belt and braces: nothing may escape onto the dispatch thread.
		std::cerr << RED << "PlatformManager: command handler error: " << e.what() << RESET << std::endl;
	}
}

void PlatformManager::handleRegisterAck(const nlohmann::json& msg)
{
	bool accepted = msg.contains("accepted") && msg["accepted"].is_boolean()
		? msg["accepted"].get<bool>() : false;
	if (accepted) {
		std::cout << GREEN << "PlatformManager: Registration accepted by platform" << RESET << std::endl;
		if (state_ != PlatformState::AVAILABLE) {
			transitionTo(PlatformState::AVAILABLE);
		}
	} else {
		std::string reason = msg.value("reason", "unknown");
		if (!platform::isPrintableAscii(reason, 1, 128)) reason = "<unparseable>";
		std::cerr << RED << "PlatformManager: Registration rejected: " << reason << RESET << std::endl;
		transitionTo(PlatformState::ERROR_STATE);
	}
}

void PlatformManager::handleAssignTask(const nlohmann::json& msg)
{
	if (state_ != PlatformState::AVAILABLE) {
		std::cerr << YELLOW << "PlatformManager: Received assign_task but not AVAILABLE (state="
				  << platformStateToString(state_) << ")" << RESET << std::endl;
		return;
	}

	std::string lease_id = msg.value("lease_id", "");
	std::string consumer_id = msg.value("consumer_id", "");
	std::string consumer_address = msg.value("consumer_address", "");
	std::string prefix = msg.value("prefix", "");
	int duration_sec = msg.contains("duration_sec") && msg["duration_sec"].is_number_integer()
		? msg["duration_sec"].get<int>() : 3600;

	// Strict field validation BEFORE any state changes (finding 2 / review req. 4).
	// WHY reject-and-stay-AVAILABLE instead of the old ERROR_STATE transition: a
	// malformed message must not let a broker peer knock the rig out of service.
	if (!platform::isSafeIdentifier(lease_id, 1, 64) ||
		!platform::isSafeIdentifier(consumer_id, 1, 64)) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"assign_task rejected: invalid lease_id/consumer_id");
		return;
	}
	// The consumer address becomes the mining salt (payout identity for the lease):
	// full EIP-55 validation, not just a length check.
	EthereumAddressValidator addr_validator;
	if (!addr_validator.isValid(consumer_address)) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"assign_task rejected: invalid consumer_address");
		return;
	}
	// Prefix feeds hex key generation directly: exact platform length, hex only.
	if (!prefix.empty() &&
		!platform::isHexString(prefix, PLATFORM_PREFIX_LENGTH, PLATFORM_PREFIX_LENGTH)) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"assign_task rejected: invalid prefix (need " +
			std::to_string(PLATFORM_PREFIX_LENGTH) + " hex chars)");
		return;
	}
	// 1 minute .. 7 days: outside that is either a typo or an attempt to pin the
	// rig to a consumer indefinitely.
	if (duration_sec < 60 || duration_sec > 7 * 24 * 3600) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"assign_task rejected: duration_sec out of range");
		return;
	}

	transitionTo(PlatformState::LEASED);

	// Start the lease
	if (!lease_manager_.startLease(lease_id, consumer_id, consumer_address, prefix, duration_sec)) {
		std::cerr << RED << "PlatformManager: Failed to start lease" << RESET << std::endl;
		transitionTo(PlatformState::ERROR_STATE);
		return;
	}

	// Switch mining to platform mode
	MiningContext ctx = lease_manager_.toMiningContext();
	switchToPlatformMining(ctx);

	transitionTo(PlatformState::MINING);
}

void PlatformManager::handleRelease(const nlohmann::json& msg)
{
	std::string lease_id = msg.value("lease_id", "");
	if (!lease_id.empty() && !platform::isSafeIdentifier(lease_id, 1, 64)) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"release rejected: invalid lease_id");
		return;
	}

	auto current = lease_manager_.getLease();
	if (!current.has_value()) {
		std::cout << YELLOW << "PlatformManager: Release received but no active lease" << RESET << std::endl;
		return;
	}

	if (!lease_id.empty() && current->lease_id != lease_id) {
		std::cerr << YELLOW << "PlatformManager: Release for wrong lease_id: " << lease_id
				  << " (current: " << current->lease_id << ")" << RESET << std::endl;
		return;
	}

	std::cout << "PlatformManager: Releasing lease " << current->lease_id << std::endl;

	transitionTo(PlatformState::COMPLETED);

	lease_manager_.endLease();
	switchToSelfMining();

	transitionTo(PlatformState::AVAILABLE);
}

void PlatformManager::handleControl(const nlohmann::json& msg)
{
	std::string action = msg.value("action", "");
	if (action.size() > 32) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"ignored control message with oversized action field");
		return;
	}

	if (action == "pause") {
		std::cout << "PlatformManager: Pause requested" << std::endl;
		if (lease_manager_.hasActiveLease()) {
			lease_manager_.endLease();
			switchToSelfMining();
		}
		transitionTo(PlatformState::IDLE);
	} else if (action == "resume") {
		std::cout << "PlatformManager: Resume requested" << std::endl;
		if (state_ == PlatformState::IDLE) {
			reporter_.sendRegistration(eth_address_, gpus_);
			transitionTo(PlatformState::AVAILABLE);
		}
	} else if (action == "shutdown") {
		// Only reachable with a valid signature (isMutatingCommand classifies
		// shutdown as mutating, so the unsigned legacy path refuses it).
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"signed remote shutdown command accepted — stopping platform manager");
		// stop() detects it is running on the dispatch thread and defers that one
		// join to the destructor; everything else tears down here.
		stop();
	} else if (action == "set_config") {
		handleSetConfig(msg);
	}
}

void PlatformManager::handleSetConfig(const nlohmann::json& msg)
{
	// Only reachable through a valid HMAC envelope (set_config is mutating, so the
	// unsigned legacy path never gets here). Every field is still bounds-checked:
	// a compromised or buggy platform server must not push nonsense into the miner.
	auto config = msg.value("config", nlohmann::json::object());
	if (!config.is_object()) {
		ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
			"set_config rejected: config is not an object");
		return;
	}

	if (config.contains("difficulty")) {
		// Argon2 memory cost in KiB. Lower bound 1 matches the existing check; the
		// 10M-KiB (~10 GiB) upper bound is far above any plausible network value
		// but stops a hostile value from OOMing every GPU in the rig.
		if (config["difficulty"].is_number_integer()) {
			int newDiff = config["difficulty"].get<int>();
			if (newDiff >= 1 && newDiff <= 10'000'000) {
				globalDifficulty.store(newDiff);
				std::cout << GREEN << "PlatformManager: Difficulty set to "
						  << newDiff << RESET << std::endl;
			} else {
				ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
					"set_config: difficulty out of range — ignored");
			}
		} else {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"set_config: difficulty not an integer — ignored");
		}
	}

	if (config.contains("address")) {
		std::string addr = config["address"].is_string()
			? config["address"].get<std::string>() : "";
		// Highest-risk command in the protocol: this redirects every future block
		// reward. Requires (a) the HMAC signature that gated entry to this handler
		// and (b) full EIP-55 address validation, and it is logged at Error level
		// so an operator scanning the console cannot miss it (review req. 5).
		EthereumAddressValidator addr_validator;
		if (!addr.empty() && addr_validator.isValid(addr)) {
			setMiningUserAddress(addr);
			ConsoleLog::event(ConsoleLog::Level::Error, "platform",
				"REMOTE PAYOUT ADDRESS CHANGE via signed platform command: mining "
				"address is now " + addr);
		} else {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"set_config: invalid payout address — ignored");
		}
	}

	if (config.contains("prefix")) {
		std::string pfx = config["prefix"].is_string()
			? config["prefix"].get<std::string>() : std::string("\x01");  // sentinel fails hex check
		// Key prefix flows into hex key generation; empty clears it. 32 hex chars
		// is half the 64-char key — more prefix than that guts entropy.
		if (pfx.empty() || platform::isHexString(pfx, 1, 32)) {
			setSelfMiningPrefix(pfx);
			std::cout << GREEN << "PlatformManager: Self-mining prefix set to '"
					  << pfx << "'" << RESET << std::endl;
		} else {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"set_config: invalid prefix (hex, <=32 chars) — ignored");
		}
	}

	if (config.contains("block_pattern")) {
		std::string pat = config["block_pattern"].is_string()
			? config["block_pattern"].get<std::string>() : std::string("\x01");
		// Pattern is matched against hex hash output ("XEN11"-style); empty resets
		// to the default. Alphanumeric-bounded keeps it sane and log-safe.
		if (pat.empty() || platform::isSafeIdentifier(pat, 1, 16)) {
			setTestBlockPattern(pat);
			std::cout << GREEN << "PlatformManager: Block pattern set to '"
					  << pat << "'" << RESET << std::endl;
		} else {
			ConsoleLog::event(ConsoleLog::Level::Warn, "platform",
				"set_config: invalid block_pattern — ignored");
		}
	}

	// Send immediate heartbeat so server/dashboard sees the change
	float total_hashrate = 0;
	int active_gpus = 0;
	{
		std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
		for (const auto& [idx, pair] : globalGpuInfos) {
			total_hashrate += pair.first.hashrate;
			active_gpus++;
		}
	}
	reporter_.sendHeartbeat(total_hashrate, active_gpus,
							globalNormalBlockCount + globalSuperBlockCount);
}

// --- Heartbeat ---

void PlatformManager::heartbeatLoop()
{
	while (running_) {
		// Sleep in small increments so we can exit promptly
		for (int i = 0; i < HEARTBEAT_INTERVAL_SEC && running_; ++i) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		if (!running_) break;

		// Gather stats from global state
		float total_hashrate = 0;
		int active_gpus = 0;
		{
			std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
			for (const auto& [idx, pair] : globalGpuInfos) {
				total_hashrate += pair.first.hashrate;
				active_gpus++;
			}
		}
		int accepted_blocks = globalNormalBlockCount + globalSuperBlockCount;

		reporter_.sendHeartbeat(total_hashrate, active_gpus, accepted_blocks);
	}
}

// --- Lease Watchdog ---

void PlatformManager::leaseWatchdogLoop()
{
	while (running_) {
		for (int i = 0; i < WATCHDOG_INTERVAL_SEC && running_; ++i) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		if (!running_) break;

		// Check if the current lease has expired
		if (state_ == PlatformState::MINING && lease_manager_.isExpired()) {
			std::cout << YELLOW << "PlatformManager: Lease expired" << RESET << std::endl;

			transitionTo(PlatformState::COMPLETED);

			lease_manager_.endLease();
			switchToSelfMining();

			transitionTo(PlatformState::AVAILABLE);
		}

		// Attempt recovery from error state
		if (state_ == PlatformState::ERROR_STATE) {
			std::cout << "PlatformManager: Attempting recovery..." << std::endl;
			transitionTo(PlatformState::IDLE);
			if (mqtt_->isConnected()) {
				reporter_.sendRegistration(eth_address_, gpus_);
				transitionTo(PlatformState::AVAILABLE);
			} else {
				mqtt_->connect();
			}
		}
	}
}

// --- Mining Mode Switching ---

void PlatformManager::switchToSelfMining()
{
	MiningContext ctx;
	ctx.mode = MiningMode::SELF_MINING;
	ctx.address = miningIdentitySnapshot()->userAddress;
	MiningCoordinator::getInstance().updateContext(ctx);
	std::cout << "PlatformManager: Switched to self-mining" << std::endl;
}

void PlatformManager::switchToPlatformMining(const MiningContext& ctx)
{
	MiningCoordinator::getInstance().updateContext(ctx);
	std::cout << GREEN << "PlatformManager: Switched to platform mining for "
			  << ctx.address << RESET << std::endl;
}
