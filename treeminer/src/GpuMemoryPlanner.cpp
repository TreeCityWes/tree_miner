#include "GpuMemoryPlanner.h"

#include "hashapi/HashApiTuning.h"

GpuMemoryPlanner& GpuMemoryPlanner::instance()
{
    static GpuMemoryPlanner planner;
    return planner;
}

void GpuMemoryPlanner::declareStream(int device, int stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    devices_[device].emplace(stream, Slot{});
}

void GpuMemoryPlanner::retireStream(int device, int stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto dev = devices_.find(device);
    if (dev != devices_.end()) {
        dev->second.erase(stream);
    }
}

std::size_t GpuMemoryPlanner::planPool(
    int device, int stream,
    const std::function<std::size_t()>& queryFreeBytes,
    const std::function<std::size_t(std::size_t share)>& choosePoolBytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slots = devices_[device];
    Slot& self = slots[stream];  // lazy-register defensively; declareStream is the norm
    self.reserved = 0;           // our own pool was released before sizing
    self.committed = false;

    hashapi::StreamShareInput in;
    in.active_streams = slots.size();
    in.headroom_bytes = kGpuDeviceHeadroomBytes;
    for (const auto& [sibling_stream, slot] : slots) {
        if (sibling_stream == stream) {
            continue;
        }
        if (slot.committed) {
            in.sibling_committed_bytes += slot.reserved;
        } else {
            in.sibling_pending_bytes += slot.reserved;
        }
    }
    in.device_free_bytes = queryFreeBytes();

    const std::size_t share = hashapi::computeStreamMemoryShare(in);
    const std::size_t pool = choosePoolBytes(share);
    self.reserved = pool;
    return pool;
}

void GpuMemoryPlanner::confirmPool(int device, int stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    devices_[device][stream].committed = true;
}

void GpuMemoryPlanner::releasePool(int device, int stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto dev = devices_.find(device);
    if (dev == devices_.end()) {
        return;
    }
    auto slot = dev->second.find(stream);
    if (slot == dev->second.end()) {
        return;
    }
    slot->second.reserved = 0;
    slot->second.committed = false;
}
