#include "GpuTelemetry.h"

#if defined(TREEMINER_GPU_HIP)
#if defined(TREEMINER_HAVE_ROCM_SMI)
#include <rocm_smi/rocm_smi.h>
#endif
#else
#include <nvml.h>
#endif

namespace gputelemetry {

#if !defined(TREEMINER_GPU_HIP)

// ---------------------------------------------------------------- NVIDIA / NVML

TelemetrySession::TelemetrySession()
{
	available_ = nvmlInit() == NVML_SUCCESS;
}

TelemetrySession::~TelemetrySession()
{
	if (available_) {
		nvmlShutdown();
	}
}

DeviceTelemetry TelemetrySession::query(int deviceIndex, int busId) const
{
	(void)busId;
	DeviceTelemetry telemetry;
	if (!available_ || deviceIndex < 0) {
		return telemetry;
	}
	nvmlDevice_t device;
	if (nvmlDeviceGetHandleByIndex(static_cast<unsigned int>(deviceIndex), &device) != NVML_SUCCESS) {
		return telemetry;
	}
	unsigned int power = 0;
	if (nvmlDeviceGetPowerUsage(device, &power) == NVML_SUCCESS) {
		telemetry.hasPower = true;
		telemetry.powerMilliwatts = power;
	}
	nvmlUtilization_t utilization;
	if (nvmlDeviceGetUtilizationRates(device, &utilization) == NVML_SUCCESS) {
		telemetry.hasUtilization = true;
		telemetry.utilizationPercent = utilization.gpu;
	}
	return telemetry;
}

const char* TelemetrySession::sourceName()
{
	return "NVML";
}

#elif defined(TREEMINER_HAVE_ROCM_SMI)

// ------------------------------------------------------------- AMD / ROCm SMI

namespace {

// ROCm SMI enumerates devices in its own order, which need not match HIP's. Resolve
// the SMI index by PCI bus id — rsmi_dev_pci_id_get packs the BDF as
// (domain << 32) | (bus << 8) | (device << 3) | function.
bool findSmiIndexForBus(int busId, uint32_t& smiIndexOut)
{
	uint32_t deviceCount = 0;
	if (rsmi_num_monitor_devices(&deviceCount) != RSMI_STATUS_SUCCESS) {
		return false;
	}
	for (uint32_t i = 0; i < deviceCount; ++i) {
		uint64_t pciId = 0;
		if (rsmi_dev_pci_id_get(i, &pciId) != RSMI_STATUS_SUCCESS) {
			continue;
		}
		const int bus = static_cast<int>((pciId >> 8) & 0xFF);
		if (bus == busId) {
			smiIndexOut = i;
			return true;
		}
	}
	return false;
}

} // namespace

TelemetrySession::TelemetrySession()
{
	available_ = rsmi_init(0) == RSMI_STATUS_SUCCESS;
}

TelemetrySession::~TelemetrySession()
{
	if (available_) {
		rsmi_shut_down();
	}
}

DeviceTelemetry TelemetrySession::query(int deviceIndex, int busId) const
{
	DeviceTelemetry telemetry;
	if (!available_ || deviceIndex < 0) {
		return telemetry;
	}
	uint32_t smiIndex = static_cast<uint32_t>(deviceIndex);
	if (busId >= 0) {
		findSmiIndexForBus(busId, smiIndex);
	}

	uint64_t microwatts = 0;
	// rsmi_dev_power_ave_get is deprecated in ROCm 6 in favour of rsmi_dev_power_get,
	// but the average-power sensor is what every supported release exposes.
	if (rsmi_dev_power_ave_get(smiIndex, 0, &microwatts) == RSMI_STATUS_SUCCESS) {
		telemetry.hasPower = true;
		telemetry.powerMilliwatts = static_cast<unsigned int>(microwatts / 1000ULL);
	}
	uint32_t busyPercent = 0;
	if (rsmi_dev_busy_percent_get(smiIndex, &busyPercent) == RSMI_STATUS_SUCCESS) {
		telemetry.hasUtilization = true;
		telemetry.utilizationPercent = busyPercent;
	}
	return telemetry;
}

const char* TelemetrySession::sourceName()
{
	return "ROCm SMI";
}

#else

// ------------------------------------- AMD build without the ROCm SMI library

TelemetrySession::TelemetrySession() = default;
TelemetrySession::~TelemetrySession() = default;

DeviceTelemetry TelemetrySession::query(int deviceIndex, int busId) const
{
	(void)deviceIndex;
	(void)busId;
	return DeviceTelemetry{};
}

const char* TelemetrySession::sourceName()
{
	return "none";
}

#endif

} // namespace gputelemetry
