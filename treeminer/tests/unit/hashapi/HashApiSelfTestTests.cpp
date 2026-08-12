#include "hashapi/HashApiSelfTest.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr char kDigest[] = "known-good-digest";

class FakeBackend final : public hashapi::IHashBackend {
public:
    explicit FakeBackend(hashapi::HashApiResult result) : result_(std::move(result)) {}
    hashapi::HashApiResult runBatch(const hashapi::HashApiRequest& request) override
    {
        last_request = request;
        return result_;
    }
    hashapi::HashApiRequest last_request;
private:
    hashapi::HashApiResult result_;
};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

hashapi::HashApiResult success(std::string hash)
{
    hashapi::HashApiResult result;
    result.ok = true;
    result.hash = std::move(hash);
    return result;
}
} // namespace

int main()
{
    try {
        FakeBackend cpu(success(std::string("$argon2id$v=19$m=8,t=1,p=1$salt$") + kDigest));
        FakeBackend cuda(success(kDigest));
        const auto passed = hashapi::runCpuCudaSelfTest(cpu, cuda, 3, false);
        require(passed.ok, "matching CPU/CUDA digests did not pass");
        require(cpu.last_request.backend == "cpu", "CPU request selected wrong backend");
        require(cuda.last_request.backend == "cuda", "CUDA request selected wrong backend");
        require(cuda.last_request.device_id == 3, "CUDA request selected wrong device");
        require(!cuda.last_request.gpu_first_blocks, "CUDA request selected wrong kernel path");

        FakeBackend wrong_cpu(success(std::string("$argon2id$v=19$m=8,t=1,p=1$salt$") + kDigest));
        FakeBackend wrong_cuda(success("wrong-digest"));
        const auto mismatch = hashapi::runCpuCudaSelfTest(wrong_cpu, wrong_cuda, 0, true);
        require(!mismatch.ok, "digest mismatch was accepted");
        require(mismatch.error.find("mismatch") != std::string::npos,
                "digest mismatch lacked a useful diagnostic");
        require(wrong_cuda.last_request.gpu_first_blocks,
                "self-test did not exercise requested first-block mode");

        hashapi::HashApiResult failed;
        failed.error = "device failure";
        FakeBackend good_cpu(success(std::string("$argon2id$v=19$m=8,t=1,p=1$salt$") + kDigest));
        FakeBackend failed_cuda(failed);
        require(!hashapi::runCpuCudaSelfTest(good_cpu, failed_cuda, 0).ok,
                "CUDA backend failure was accepted");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
