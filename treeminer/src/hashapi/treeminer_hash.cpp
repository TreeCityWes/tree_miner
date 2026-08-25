// C ABI implementation of hash-batch. Dispatches to CpuHashBackend / CudaHashBackend.
// The CUDA kernel stays in kernelrunner.cu. Not linked into xenblocksMiner yet
// (orchestrator crate will wire this). Compile-checked by the Rust crate's C stub twin.

#include "treeminer_hash.h"

#include "CpuHashBackend.h"
#include "HashApiTypes.h"
#include "HashApiValidation.h"

#if defined(XENBLOCKS_BUILD_MINER)
#include "CudaHashBackend.h"
#include "../CudaBackend.h"
#endif

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace {

const char* or_empty(const char* s) { return s ? s : ""; }

std::string or_default(const char* s, const char* fallback) {
    if (s == nullptr) {
        return fallback;
    }
    return s;
}

char* dup_str(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) {
        return nullptr;
    }
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

hashapi::HashApiRequest from_c(const TreeminerHashRequest& in) {
    hashapi::HashApiRequest r;
    r.request_id = or_empty(in.request_id);
    r.algorithm = or_default(in.algorithm, "argon2id-xen");
    r.backend = or_default(in.backend, "cpu");
    r.salt_hex = or_empty(in.salt_hex);
    r.key = or_empty(in.key);
    r.key_prefix = or_empty(in.key_prefix);
    r.target_pattern = or_default(in.target_pattern, "XEN11");
    r.difficulty = in.difficulty;
    r.batch_size = in.batch_size;
    r.device_id = in.device_id;
    r.allow_xuni = in.allow_xuni != 0;
    r.detailed_timings = in.detailed_timings != 0;
    r.first_block_workers = in.first_block_workers;
    r.first_block_dynamic_chunk_size = in.first_block_dynamic_chunk_size;
    r.first_block_dynamic_chunk_auto = in.first_block_dynamic_chunk_auto != 0;
    r.gpu_first_blocks = in.gpu_first_blocks != 0;
    return r;
}

void copy_timings(const hashapi::HashApiTimings& t, TreeminerHashTimings* out) {
    out->validation_ms = t.validation_ms;
    out->setup_ms = t.setup_ms;
    out->setup_normalize_cpu_ms = t.setup_normalize_cpu_ms;
    out->setup_activate_cpu_ms = t.setup_activate_cpu_ms;
    out->setup_device_info_cpu_ms = t.setup_device_info_cpu_ms;
    out->setup_params_cpu_ms = t.setup_params_cpu_ms;
    out->setup_backend_init_cpu_ms = t.setup_backend_init_cpu_ms;
    out->input_ms = t.input_ms;
    out->keygen_ms = t.keygen_ms;
    out->first_block_ms = t.first_block_ms;
    out->first_block_initial_hash_cpu_ms = t.first_block_initial_hash_cpu_ms;
    out->first_block_digest_cpu_ms = t.first_block_digest_cpu_ms;
    out->first_block_max_worker_ms = t.first_block_max_worker_ms;
    out->first_block_thread_launch_ms = t.first_block_thread_launch_ms;
    out->first_block_max_worker_start_ms = t.first_block_max_worker_start_ms;
    out->first_block_worker_start_span_ms = t.first_block_worker_start_span_ms;
    out->first_block_max_worker_finish_ms = t.first_block_max_worker_finish_ms;
    out->first_block_worker_finish_span_ms = t.first_block_worker_finish_span_ms;
    out->compute_ms = t.compute_ms;
    out->kernel_ms = t.kernel_ms;
    out->host_to_device_ms = t.host_to_device_ms;
    out->gpu_first_block_ms = t.gpu_first_block_ms;
    out->device_to_host_ms = t.device_to_host_ms;
    out->finalize_ms = t.finalize_ms;
    out->finalize_hash_ms = t.finalize_hash_ms;
    out->argon2_finalize_ms = t.argon2_finalize_ms;
    out->base64_ms = t.base64_ms;
    out->match_ms = t.match_ms;
    out->total_ms = t.total_ms;
}

void fill_result(const hashapi::HashApiResult& r, TreeminerHashResult* out) {
    std::memset(out, 0, sizeof(*out));
    out->request_id = dup_str(r.request_id);
    out->ok = r.ok ? 1 : 0;
    out->error = dup_str(r.error);
    out->algorithm = dup_str(r.algorithm);
    out->backend = dup_str(r.backend);
    out->device_id = r.device_id;
    out->batch_size = r.batch_size;
    out->batch_size_min = r.batch_size_min;
    out->batch_size_max = r.batch_size_max;
    out->attempts = r.attempts;
    out->first_block_dynamic_chunk_size = r.first_block_dynamic_chunk_size;
    out->first_block_dynamic_chunk_auto = r.first_block_dynamic_chunk_auto ? 1 : 0;
    out->first_block_worker_count = r.first_block_worker_count;
    out->first_block_chunk_size = r.first_block_chunk_size;
    out->first_block_dynamic_chunk_size_min = r.first_block_dynamic_chunk_size_min;
    out->first_block_dynamic_chunk_size_max = r.first_block_dynamic_chunk_size_max;
    out->first_block_chunk_size_min = r.first_block_chunk_size_min;
    out->first_block_chunk_size_max = r.first_block_chunk_size_max;
    out->gpu_first_blocks = r.gpu_first_blocks ? 1 : 0;
    out->elapsed_ms = r.elapsed_ms;
    out->hashrate = r.hashrate;
    copy_timings(r.timings, &out->timings);
    out->hash = dup_str(r.hash);
    out->match_count = r.matches.size();
    if (!r.matches.empty()) {
        out->matches = static_cast<TreeminerHashMatch*>(
            std::calloc(r.matches.size(), sizeof(TreeminerHashMatch)));
        if (out->matches) {
            for (std::size_t i = 0; i < r.matches.size(); ++i) {
                out->matches[i].key = dup_str(r.matches[i].key);
                out->matches[i].hash = dup_str(r.matches[i].hash);
                out->matches[i].matched_pattern = dup_str(r.matches[i].matched_pattern);
                out->matches[i].attempt_index = r.matches[i].attempt_index;
                out->matches[i].is_superblock = r.matches[i].is_superblock ? 1 : 0;
            }
        } else {
            out->match_count = 0;
        }
    }
}

hashapi::HashApiResult run_batch(const hashapi::HashApiRequest& request) {
    if (request.backend == "cuda") {
        const auto errors = hashapi::validateRequest(request);
        if (!errors.empty()) {
            hashapi::HashApiResult result;
            result.request_id = request.request_id;
            result.algorithm = request.algorithm;
            result.backend = request.backend;
            result.device_id = request.device_id;
            result.batch_size = request.batch_size;
            result.error = hashapi::joinErrors(errors);
            return result;
        }
#if defined(XENBLOCKS_BUILD_MINER)
        try {
            hashapi::CudaHashBackend backend(
                std::make_unique<CudaBackend>(request.device_id));
            return backend.runBatch(request);
        } catch (const std::exception& ex) {
            hashapi::HashApiResult result;
            result.request_id = request.request_id;
            result.algorithm = request.algorithm;
            result.backend = "cuda";
            result.device_id = request.device_id;
            result.batch_size = request.batch_size;
            result.error = ex.what();
            return result;
        }
#else
        hashapi::HashApiResult result;
        result.request_id = request.request_id;
        result.algorithm = request.algorithm;
        result.backend = "cuda";
        result.device_id = request.device_id;
        result.batch_size = request.batch_size;
        result.error = "cuda backend is not available in this build";
        return result;
#endif
    }
    hashapi::CpuHashBackend backend;
    return backend.runBatch(request);
}

}  // namespace

extern "C" int treeminer_hash_run_batch(const TreeminerHashRequest* request,
                                        TreeminerHashResult* out) {
    if (!request || !out) {
        return TREEMINER_HASH_ERR_NULL;
    }
    fill_result(run_batch(from_c(*request)), out);
    return TREEMINER_HASH_OK;
}

extern "C" void treeminer_hash_result_free(TreeminerHashResult* out) {
    if (!out) {
        return;
    }
    std::free(out->request_id);
    std::free(out->error);
    std::free(out->algorithm);
    std::free(out->backend);
    std::free(out->hash);
    if (out->matches) {
        for (size_t i = 0; i < out->match_count; ++i) {
            std::free(out->matches[i].key);
            std::free(out->matches[i].hash);
            std::free(out->matches[i].matched_pattern);
        }
        std::free(out->matches);
    }
    std::memset(out, 0, sizeof(*out));
}
