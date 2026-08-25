#pragma once
// C ABI for Hash API hash-batch (IHashBackend::runBatch).
//
// The CUDA kernel stays in kernelrunner.cu and is reached through CudaHashBackend.
// This header is the FFI seam: Rust (treeminer-hash) and C++ (treeminer_hash.cpp) share
// these types. CMake target `treeminer_hash` (optional, TREEMINER_BUILD_HASH_FFI) builds
// the production .so; xenblocksMiner does not link it.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TreeminerHashRequest {
    const char* request_id;     /* nullable → empty */
    const char* algorithm;      /* nullable → "argon2id-xen" */
    const char* backend;        /* nullable → "cpu" */
    const char* salt_hex;
    const char* key;            /* nullable/empty → generate keys */
    const char* key_prefix;
    const char* target_pattern; /* nullable → "XEN11" */
    uint32_t difficulty;
    size_t batch_size;
    int32_t device_id;
    int32_t allow_xuni; /* 0/1 */
    int32_t detailed_timings;
    size_t first_block_workers;
    size_t first_block_dynamic_chunk_size;
    int32_t first_block_dynamic_chunk_auto;
    int32_t gpu_first_blocks;
} TreeminerHashRequest;

typedef struct TreeminerHashTimings {
    double validation_ms;
    double setup_ms;
    double setup_normalize_cpu_ms;
    double setup_activate_cpu_ms;
    double setup_device_info_cpu_ms;
    double setup_params_cpu_ms;
    double setup_backend_init_cpu_ms;
    double input_ms;
    double keygen_ms;
    double first_block_ms;
    double first_block_initial_hash_cpu_ms;
    double first_block_digest_cpu_ms;
    double first_block_max_worker_ms;
    double first_block_thread_launch_ms;
    double first_block_max_worker_start_ms;
    double first_block_worker_start_span_ms;
    double first_block_max_worker_finish_ms;
    double first_block_worker_finish_span_ms;
    double compute_ms;
    double kernel_ms;
    double host_to_device_ms;
    double gpu_first_block_ms;
    double device_to_host_ms;
    double finalize_ms;
    double finalize_hash_ms;
    double argon2_finalize_ms;
    double base64_ms;
    double match_ms;
    double total_ms;
} TreeminerHashTimings;

typedef struct TreeminerHashMatch {
    char* key;
    char* hash;
    char* matched_pattern;
    size_t attempt_index;
    int32_t is_superblock;
} TreeminerHashMatch;

typedef struct TreeminerHashResult {
    char* request_id;
    int32_t ok;
    char* error;
    char* algorithm;
    char* backend;
    int32_t device_id;
    size_t batch_size;
    size_t batch_size_min;
    size_t batch_size_max;
    size_t attempts;
    size_t first_block_dynamic_chunk_size;
    int32_t first_block_dynamic_chunk_auto;
    size_t first_block_worker_count;
    size_t first_block_chunk_size;
    size_t first_block_dynamic_chunk_size_min;
    size_t first_block_dynamic_chunk_size_max;
    size_t first_block_chunk_size_min;
    size_t first_block_chunk_size_max;
    int32_t gpu_first_blocks;
    double elapsed_ms;
    double hashrate;
    TreeminerHashTimings timings;
    char* hash;
    TreeminerHashMatch* matches;
    size_t match_count;
} TreeminerHashResult;

enum {
    TREEMINER_HASH_OK = 0,
    TREEMINER_HASH_ERR_NULL = 1
};

/* Fills *out. Returns TREEMINER_HASH_OK even when out->ok is 0 (validation/backend
   error is in out->error). Caller must treeminer_hash_result_free(out). */
int treeminer_hash_run_batch(const TreeminerHashRequest* request, TreeminerHashResult* out);
void treeminer_hash_result_free(TreeminerHashResult* out);

#ifdef __cplusplus
}
#endif
