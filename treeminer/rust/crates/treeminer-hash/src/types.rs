//! Hash API request/result models. Port of `src/hashapi/HashApiTypes.h`.

pub const HASH_API_KEY_LENGTH: usize = 64;
pub const DEFAULT_HASH_LENGTH: usize = 64;
pub const MAX_TARGET_PATTERN_LENGTH: usize = 128;
pub const MAX_CPU_BATCH_SIZE: usize = 10_000;
pub const MIN_ARGON2_CPU_DIFFICULTY: u32 = 8;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HashRequest {
    pub request_id: String,
    pub algorithm: String,
    pub backend: String,
    pub salt_hex: String,
    pub key: String,
    pub key_prefix: String,
    pub target_pattern: String,
    pub difficulty: u32,
    pub batch_size: usize,
    pub device_id: i32,
    pub allow_xuni: bool,
    pub detailed_timings: bool,
    pub first_block_workers: usize,
    pub first_block_dynamic_chunk_size: usize,
    pub first_block_dynamic_chunk_auto: bool,
    pub gpu_first_blocks: bool,
}

impl Default for HashRequest {
    fn default() -> Self {
        Self {
            request_id: String::new(),
            algorithm: "argon2id-xen".into(),
            backend: "cpu".into(),
            salt_hex: String::new(),
            key: String::new(),
            key_prefix: String::new(),
            target_pattern: "XEN11".into(),
            difficulty: 42069,
            batch_size: 1,
            device_id: 0,
            allow_xuni: true,
            detailed_timings: false,
            first_block_workers: 0,
            first_block_dynamic_chunk_size: 0,
            first_block_dynamic_chunk_auto: false,
            gpu_first_blocks: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HashMatch {
    pub key: String,
    pub hash: String,
    pub matched_pattern: String,
    pub attempt_index: usize,
    pub is_superblock: bool,
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct HashTimings {
    pub validation_ms: f64,
    pub setup_ms: f64,
    pub setup_normalize_cpu_ms: f64,
    pub setup_activate_cpu_ms: f64,
    pub setup_device_info_cpu_ms: f64,
    pub setup_params_cpu_ms: f64,
    pub setup_backend_init_cpu_ms: f64,
    pub input_ms: f64,
    pub keygen_ms: f64,
    pub first_block_ms: f64,
    pub first_block_initial_hash_cpu_ms: f64,
    pub first_block_digest_cpu_ms: f64,
    pub first_block_max_worker_ms: f64,
    pub first_block_thread_launch_ms: f64,
    pub first_block_max_worker_start_ms: f64,
    pub first_block_worker_start_span_ms: f64,
    pub first_block_max_worker_finish_ms: f64,
    pub first_block_worker_finish_span_ms: f64,
    pub compute_ms: f64,
    pub kernel_ms: f64,
    pub host_to_device_ms: f64,
    pub gpu_first_block_ms: f64,
    pub device_to_host_ms: f64,
    pub finalize_ms: f64,
    pub finalize_hash_ms: f64,
    pub argon2_finalize_ms: f64,
    pub base64_ms: f64,
    pub match_ms: f64,
    pub total_ms: f64,
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct HashResult {
    pub request_id: String,
    pub ok: bool,
    pub error: String,
    pub algorithm: String,
    pub backend: String,
    pub device_id: i32,
    pub batch_size: usize,
    pub batch_size_min: usize,
    pub batch_size_max: usize,
    pub attempts: usize,
    pub first_block_dynamic_chunk_size: usize,
    pub first_block_dynamic_chunk_auto: bool,
    pub first_block_worker_count: usize,
    pub first_block_chunk_size: usize,
    pub first_block_dynamic_chunk_size_min: usize,
    pub first_block_dynamic_chunk_size_max: usize,
    pub first_block_chunk_size_min: usize,
    pub first_block_chunk_size_max: usize,
    pub gpu_first_blocks: bool,
    pub elapsed_ms: f64,
    pub hashrate: f64,
    pub timings: HashTimings,
    pub hash: String,
    pub matches: Vec<HashMatch>,
}

/// `IHashBackend::runBatch`.
pub trait HashBackend {
    fn run_batch(&mut self, request: &HashRequest) -> HashResult;
}
