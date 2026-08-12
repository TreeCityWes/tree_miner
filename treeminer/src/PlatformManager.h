#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <functional>
#include <vector>
#include "MqttClient.h"
#include "WorkerReporter.h"
#include "LeaseManager.h"
#include "MiningCoordinator.h"
#include "MiningCommon.h"
#include "platform/CommandEnvelope.h"

// Platform states for the hashpower marketplace
enum class PlatformState {
	IDLE,       // Not connected to platform
	AVAILABLE,  // Registered and waiting for lease assignment
	LEASED,     // Lease assigned, preparing to mine
	MINING,     // Actively mining for a consumer
	COMPLETED,  // Lease completed, transitioning back
	ERROR_STATE // Error state, will attempt recovery
};

const char* platformStateToString(PlatformState state);

class PlatformManager
{
public:
	PlatformManager(const std::string& broker_uri,
					const std::string& eth_address,
					const std::vector<gpuInfo>& gpus);
	~PlatformManager();

	// Lifecycle
	bool start();
	void stop();

	// Shared secret for HMAC-signed platform commands (security review finding 2).
	// The constructor already reads config.txt key `platform_command_secret`; this
	// setter overrides it and MUST be called before start() — the dispatch worker
	// reads the secret under secret_mutex_, but changing trust mid-flight is not a
	// supported operation.
	// TODO(main.cpp owner): wire a --platformSecret command-line flag to this setter
	// (command line should take precedence over config.txt, same as the margin keys).
	void setCommandSecret(std::string secret);

	// State queries
	PlatformState getState() const;
	bool isRunning() const;

	// Called by submitCallback when a block is found during platform mining
	void onBlockFound(const std::string& hash,
					  const std::string& key,
					  const std::string& account,
					  size_t attempts,
					  float hashrate);

	// Access to lease manager for external queries
	const LeaseManager& getLeaseManager() const { return lease_manager_; }

	// State change callback (for external monitoring)
	using StateChangeCallback = std::function<void(PlatformState old_state, PlatformState new_state)>;
	void setStateChangeCallback(StateChangeCallback cb);

private:
	// State transitions
	void transitionTo(PlatformState new_state);

	// --- Command intake (security review finding 9) ---
	// The MQTT callback thread only ever enqueues; a single owned worker thread
	// drains the bounded queue in FIFO arrival order. This replaces the old
	// detached-thread-per-message dispatch, which allowed unbounded thread creation
	// under flooding, use-after-free after destruction, and command reordering.
	struct QueuedCommand {
		std::string topic;
		std::string payload;
	};
	void enqueueCommand(std::string topic, std::string payload);
	void dispatchLoop();

	// Envelope verification + no-secret legacy policy. Logs the rejection reason;
	// returns true iff the command may be dispatched.
	bool authorizeCommand(const nlohmann::json& msg);

	// MQTT message handler (dispatches to state-specific handlers).
	// Runs ONLY on the dispatch worker thread; catches all exceptions internally
	// (no exceptions across thread boundaries).
	void onMessage(const std::string& topic, const std::string& payload);

	// Command handlers (from platform via MQTT)
	void handleRegisterAck(const nlohmann::json& msg);
	void handleAssignTask(const nlohmann::json& msg);
	void handleRelease(const nlohmann::json& msg);
	void handleControl(const nlohmann::json& msg);
	void handleSetConfig(const nlohmann::json& msg);

	// Heartbeat thread
	void heartbeatLoop();

	// Lease expiry checker thread
	void leaseWatchdogLoop();

	// Switch mining mode via MiningCoordinator
	void switchToSelfMining();
	void switchToPlatformMining(const MiningContext& ctx);

	// State
	std::atomic<PlatformState> state_{PlatformState::IDLE};
	std::atomic<bool> running_{false};

	// Components
	std::shared_ptr<MqttClient> mqtt_;
	WorkerReporter reporter_;
	LeaseManager lease_manager_;

	// Config
	std::string eth_address_;
	std::vector<gpuInfo> gpus_;

	// Worker id envelopes must be addressed to. Snapshot of the global machineId
	// taken at construction so the dispatch thread never reads a mutable global.
	std::string expected_worker_id_;

	// Command authentication (finding 2)
	std::string command_secret_;   // guarded by secret_mutex_
	std::mutex secret_mutex_;
	// Replay cache: touched only by the dispatch worker thread, so unsynchronized.
	platform::NonceCache nonce_cache_{NONCE_CACHE_CAPACITY};

	// Bounded command queue (finding 9)
	std::deque<QueuedCommand> command_queue_;   // guarded by queue_mutex_
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::uint64_t dropped_commands_ = 0;        // guarded by queue_mutex_
	std::chrono::steady_clock::time_point last_drop_log_{};  // guarded by queue_mutex_

	// Threads
	std::thread heartbeat_thread_;
	std::thread watchdog_thread_;
	std::thread dispatch_thread_;

	// Callback
	StateChangeCallback state_change_cb_;
	std::mutex cb_mutex_;

	static constexpr int HEARTBEAT_INTERVAL_SEC = 30;
	static constexpr int WATCHDOG_INTERVAL_SEC = 5;
	static constexpr int ERROR_RECOVERY_DELAY_SEC = 10;

	// Queue capacity: legitimate platform traffic is a handful of commands per
	// lease lifecycle; 256 gives ample headroom while keeping worst-case memory
	// (256 * 64KiB payload cap) at 16MiB. Overflow drops the NEWEST message so a
	// flood cannot displace commands already accepted (FIFO head stays intact).
	static constexpr std::size_t COMMAND_QUEUE_CAPACITY = 256;
	// 4096 nonces * <=15min lifetime comfortably covers any legitimate signed
	// command rate; only authentically signed traffic can occupy slots.
	static constexpr std::size_t NONCE_CACHE_CAPACITY = 4096;
};
