#pragma once

// Vendor-neutral GPU power/utilization telemetry.
//
// NVIDIA exposes this through NVML, AMD through ROCm SMI. Both are optional at
// runtime — a driver without the management library still mines, it just reports
// no power or utilization. Callers get the same struct either way.

namespace gputelemetry {

struct DeviceTelemetry {
	bool hasPower = false;
	unsigned int powerMilliwatts = 0;
	bool hasUtilization = false;
	unsigned int utilizationPercent = 0;
};

// Initializes the management library on construction and shuts it down on
// destruction; keep one alive for the duration of a reporting pass.
class TelemetrySession {
public:
	TelemetrySession();
	~TelemetrySession();

	TelemetrySession(const TelemetrySession&) = delete;
	TelemetrySession& operator=(const TelemetrySession&) = delete;

	bool available() const { return available_; }

	// deviceIndex is the compute-runtime device index; busId is the PCI bus id
	// reported for that device (used by backends whose management-library
	// ordering differs from the compute ordering).
	DeviceTelemetry query(int deviceIndex, int busId) const;

	// Name of the telemetry source, for logs ("NVML", "ROCm SMI", "none").
	static const char* sourceName();

private:
	bool available_ = false;
};

} // namespace gputelemetry
