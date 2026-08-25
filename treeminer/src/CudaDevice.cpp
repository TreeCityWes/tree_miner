#include "CudaDevice.h"
#include "hashapi/CudaSkip.h"
#include <stdexcept>
#include <cstddef>
#include <cuda_runtime.h>
#include <sstream>
#include <cmath>

#include "CudaException.h"

CudaDevice::CudaDevice(int index) {
    deviceIndex = index;
    cudaDeviceProp prop;
    CudaException::check(cudaGetDeviceProperties(&prop, deviceIndex));
    picBusId = prop.pciBusID;
}

std::string CudaDevice::getName() const {
    cudaDeviceProp prop;
    CudaException::check(cudaGetDeviceProperties(&prop, deviceIndex));
    return std::string(prop.name);
}

std::string CudaDevice::getFullName() const {
    cudaDeviceProp prop;
    CudaException::check(cudaGetDeviceProperties(&prop, deviceIndex));

    std::string name = prop.name;

    int memoryInGB = static_cast<int>(std::round(static_cast<float>(prop.totalGlobalMem) / (1024 * 1024 * 1024)));

    std::ostringstream fullNameStream;
    fullNameStream << name << " | " << memoryInGB << " GB";

    return fullNameStream.str();
}

std::vector<CudaDevice> CudaDevice::getAllDevices() {
    int count;
    CudaException::check(cudaGetDeviceCount(&count));

    std::vector<CudaDevice> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; i++) {
        try {
            devices.emplace_back(i);
        } catch (const CudaException& error) {
            if (hashapi::shouldSkipDevice(static_cast<int>(error.code()))) {
                continue;
            }
            throw;
        }
    }
    return devices;
}
