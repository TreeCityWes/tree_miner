//! FFI client for `treeminer_hash_run_batch`. Unsafe is confined to this module.

use std::ffi::{c_char, c_int, CStr, CString};

use crate::types::{HashBackend, HashMatch, HashRequest, HashResult, HashTimings};

#[repr(C)]
struct CRequest {
    request_id: *const c_char,
    algorithm: *const c_char,
    backend: *const c_char,
    salt_hex: *const c_char,
    key: *const c_char,
    key_prefix: *const c_char,
    target_pattern: *const c_char,
    difficulty: u32,
    batch_size: usize,
    device_id: i32,
    allow_xuni: i32,
    detailed_timings: i32,
    first_block_workers: usize,
    first_block_dynamic_chunk_size: usize,
    first_block_dynamic_chunk_auto: i32,
    gpu_first_blocks: i32,
}

#[repr(C)]
struct CTimings {
    validation_ms: f64,
    setup_ms: f64,
    setup_normalize_cpu_ms: f64,
    setup_activate_cpu_ms: f64,
    setup_device_info_cpu_ms: f64,
    setup_params_cpu_ms: f64,
    setup_backend_init_cpu_ms: f64,
    input_ms: f64,
    keygen_ms: f64,
    first_block_ms: f64,
    first_block_initial_hash_cpu_ms: f64,
    first_block_digest_cpu_ms: f64,
    first_block_max_worker_ms: f64,
    first_block_thread_launch_ms: f64,
    first_block_max_worker_start_ms: f64,
    first_block_worker_start_span_ms: f64,
    first_block_max_worker_finish_ms: f64,
    first_block_worker_finish_span_ms: f64,
    compute_ms: f64,
    kernel_ms: f64,
    host_to_device_ms: f64,
    gpu_first_block_ms: f64,
    device_to_host_ms: f64,
    finalize_ms: f64,
    finalize_hash_ms: f64,
    argon2_finalize_ms: f64,
    base64_ms: f64,
    match_ms: f64,
    total_ms: f64,
}

#[repr(C)]
struct CMatch {
    key: *mut c_char,
    hash: *mut c_char,
    matched_pattern: *mut c_char,
    attempt_index: usize,
    is_superblock: i32,
}

#[repr(C)]
struct CResult {
    request_id: *mut c_char,
    ok: i32,
    error: *mut c_char,
    algorithm: *mut c_char,
    backend: *mut c_char,
    device_id: i32,
    batch_size: usize,
    batch_size_min: usize,
    batch_size_max: usize,
    attempts: usize,
    first_block_dynamic_chunk_size: usize,
    first_block_dynamic_chunk_auto: i32,
    first_block_worker_count: usize,
    first_block_chunk_size: usize,
    first_block_dynamic_chunk_size_min: usize,
    first_block_dynamic_chunk_size_max: usize,
    first_block_chunk_size_min: usize,
    first_block_chunk_size_max: usize,
    gpu_first_blocks: i32,
    elapsed_ms: f64,
    hashrate: f64,
    timings: CTimings,
    hash: *mut c_char,
    matches: *mut CMatch,
    match_count: usize,
}

extern "C" {
    fn treeminer_hash_run_batch(request: *const CRequest, out: *mut CResult) -> c_int;
    fn treeminer_hash_result_free(out: *mut CResult);
}

fn cstring(s: &str) -> CString {
    let cleaned: String = s.chars().filter(|c| *c != '\0').collect();
    CString::new(cleaned).unwrap_or_else(|_| CString::new("").unwrap())
}

fn from_ptr(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() }
}

fn timings_from_c(t: &CTimings) -> HashTimings {
    HashTimings {
        validation_ms: t.validation_ms,
        setup_ms: t.setup_ms,
        setup_normalize_cpu_ms: t.setup_normalize_cpu_ms,
        setup_activate_cpu_ms: t.setup_activate_cpu_ms,
        setup_device_info_cpu_ms: t.setup_device_info_cpu_ms,
        setup_params_cpu_ms: t.setup_params_cpu_ms,
        setup_backend_init_cpu_ms: t.setup_backend_init_cpu_ms,
        input_ms: t.input_ms,
        keygen_ms: t.keygen_ms,
        first_block_ms: t.first_block_ms,
        first_block_initial_hash_cpu_ms: t.first_block_initial_hash_cpu_ms,
        first_block_digest_cpu_ms: t.first_block_digest_cpu_ms,
        first_block_max_worker_ms: t.first_block_max_worker_ms,
        first_block_thread_launch_ms: t.first_block_thread_launch_ms,
        first_block_max_worker_start_ms: t.first_block_max_worker_start_ms,
        first_block_worker_start_span_ms: t.first_block_worker_start_span_ms,
        first_block_max_worker_finish_ms: t.first_block_max_worker_finish_ms,
        first_block_worker_finish_span_ms: t.first_block_worker_finish_span_ms,
        compute_ms: t.compute_ms,
        kernel_ms: t.kernel_ms,
        host_to_device_ms: t.host_to_device_ms,
        gpu_first_block_ms: t.gpu_first_block_ms,
        device_to_host_ms: t.device_to_host_ms,
        finalize_ms: t.finalize_ms,
        finalize_hash_ms: t.finalize_hash_ms,
        argon2_finalize_ms: t.argon2_finalize_ms,
        base64_ms: t.base64_ms,
        match_ms: t.match_ms,
        total_ms: t.total_ms,
    }
}

/// Call the C `hash-batch` ABI. Cargo tests link `native/stub.c`; production replaces
/// that with `src/hashapi/treeminer_hash.cpp` → Cpu/CudaHashBackend (kernel stays nvcc).
pub fn hash_batch(request: &HashRequest) -> HashResult {
    let request_id = cstring(&request.request_id);
    let algorithm = cstring(&request.algorithm);
    let backend = cstring(&request.backend);
    let salt_hex = cstring(&request.salt_hex);
    let key = cstring(&request.key);
    let key_prefix = cstring(&request.key_prefix);
    let target_pattern = cstring(&request.target_pattern);

    let c_req = CRequest {
        request_id: request_id.as_ptr(),
        algorithm: algorithm.as_ptr(),
        backend: backend.as_ptr(),
        salt_hex: salt_hex.as_ptr(),
        key: key.as_ptr(),
        key_prefix: key_prefix.as_ptr(),
        target_pattern: target_pattern.as_ptr(),
        difficulty: request.difficulty,
        batch_size: request.batch_size,
        device_id: request.device_id,
        allow_xuni: i32::from(request.allow_xuni),
        detailed_timings: i32::from(request.detailed_timings),
        first_block_workers: request.first_block_workers,
        first_block_dynamic_chunk_size: request.first_block_dynamic_chunk_size,
        first_block_dynamic_chunk_auto: i32::from(request.first_block_dynamic_chunk_auto),
        gpu_first_blocks: i32::from(request.gpu_first_blocks),
    };

    unsafe {
        let mut out = std::mem::zeroed::<CResult>();
        let rc = treeminer_hash_run_batch(&c_req, &mut out);
        if rc != 0 {
            return HashResult {
                error: "null request or result pointer".into(),
                ..HashResult::default()
            };
        }
        let mut result = HashResult {
            request_id: from_ptr(out.request_id),
            ok: out.ok != 0,
            error: from_ptr(out.error),
            algorithm: from_ptr(out.algorithm),
            backend: from_ptr(out.backend),
            device_id: out.device_id,
            batch_size: out.batch_size,
            batch_size_min: out.batch_size_min,
            batch_size_max: out.batch_size_max,
            attempts: out.attempts,
            first_block_dynamic_chunk_size: out.first_block_dynamic_chunk_size,
            first_block_dynamic_chunk_auto: out.first_block_dynamic_chunk_auto != 0,
            first_block_worker_count: out.first_block_worker_count,
            first_block_chunk_size: out.first_block_chunk_size,
            first_block_dynamic_chunk_size_min: out.first_block_dynamic_chunk_size_min,
            first_block_dynamic_chunk_size_max: out.first_block_dynamic_chunk_size_max,
            first_block_chunk_size_min: out.first_block_chunk_size_min,
            first_block_chunk_size_max: out.first_block_chunk_size_max,
            gpu_first_blocks: out.gpu_first_blocks != 0,
            elapsed_ms: out.elapsed_ms,
            hashrate: out.hashrate,
            timings: timings_from_c(&out.timings),
            hash: from_ptr(out.hash),
            matches: Vec::new(),
        };
        if !out.matches.is_null() && out.match_count > 0 {
            let slice = std::slice::from_raw_parts(out.matches, out.match_count);
            result.matches = slice
                .iter()
                .map(|m| HashMatch {
                    key: from_ptr(m.key),
                    hash: from_ptr(m.hash),
                    matched_pattern: from_ptr(m.matched_pattern),
                    attempt_index: m.attempt_index,
                    is_superblock: m.is_superblock != 0,
                })
                .collect();
        }
        treeminer_hash_result_free(&mut out);
        result
    }
}

/// Backend that talks to the C ABI (stub in tests, C++ shim in a miner build).
#[derive(Default)]
pub struct FfiBackend;

impl HashBackend for FfiBackend {
    fn run_batch(&mut self, request: &HashRequest) -> HashResult {
        hash_batch(request)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{HashBackend, HASH_API_KEY_LENGTH};

    fn req() -> HashRequest {
        HashRequest {
            salt_hex: "aabbccddeeff0011".into(),
            difficulty: 8,
            batch_size: 2,
            target_pattern: "stub".into(),
            allow_xuni: false,
            ..HashRequest::default()
        }
    }

    #[test]
    fn ffi_batch_round_trips_through_the_c_abi() {
        let r = hash_batch(&req());
        assert!(r.ok, "{}", r.error);
        assert_eq!(r.attempts, 2);
        assert_eq!(r.backend, "cpu-stub");
        assert_eq!(r.matches.len(), 2);
        assert_eq!(r.matches[0].key.len(), HASH_API_KEY_LENGTH);
        assert!(r.matches[0].hash.contains("stub$argon2id-xen$"));
    }

    #[test]
    fn ffi_validation_errors_match_cpp_strings() {
        let mut r = req();
        r.salt_hex.clear();
        let out = hash_batch(&r);
        assert!(!out.ok);
        assert!(out.error.contains("salt_hex is required"));
    }

    #[test]
    fn ffi_null_guard() {
        unsafe {
            assert_eq!(
                treeminer_hash_run_batch(std::ptr::null(), std::ptr::null_mut()),
                1
            );
        }
    }

    #[test]
    fn ffi_hash_one_fixed_key() {
        let mut r = req();
        r.key = "ab".repeat(32);
        let out = hash_batch(&r);
        assert!(out.ok, "{}", out.error);
        assert_eq!(out.attempts, 1);
        assert!(out.hash.contains(&r.key));
    }

    #[test]
    fn rust_stub_and_c_abi_agree_on_fixed_key_hash() {
        let mut req = req();
        req.key = "ab".repeat(32);
        let ffi = hash_batch(&req);
        let rust = crate::StubBackend.run_batch(&req);
        assert_eq!(ffi.hash, rust.hash);
        assert_eq!(ffi.ok, rust.ok);
        assert_eq!(ffi.attempts, rust.attempts);
    }

    #[test]
    fn ffi_cuda_rejected_by_stub() {
        let mut r = req();
        r.backend = "cuda".into();
        let out = hash_batch(&r);
        assert!(!out.ok);
        assert!(out.error.contains("cuda backend is not available"));
    }
}
