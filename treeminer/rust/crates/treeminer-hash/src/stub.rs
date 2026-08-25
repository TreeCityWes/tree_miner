//! Pure-Rust stub backend (XENBLOCKS_HASHAPI_STUB_BACKEND equivalent).
//! Same hash format as `native/stub.c`. Not a mining backend.

use crate::matching::append_matches;
use crate::types::{HashBackend, HashRequest, HashResult, HASH_API_KEY_LENGTH};
use crate::validate::{join_errors, normalize_hex, validate_request};

pub struct StubBackend;

fn stub_hash(salt: &str, key: &str, difficulty: u32) -> String {
    format!("stub$argon2id-xen${salt}${key}${difficulty}")
}

pub fn make_key(prefix: &str, index: usize) -> String {
    let plen = prefix.len().min(HASH_API_KEY_LENGTH);
    let prefix = &prefix[..plen];
    let suffix_len = HASH_API_KEY_LENGTH - prefix.len();
    if suffix_len == 0 {
        return prefix.to_string();
    }
    let hex = format!("{index:x}");
    let suffix = if hex.len() >= suffix_len {
        hex[hex.len() - suffix_len..].to_string()
    } else {
        format!("{hex:0>width$}", width = suffix_len)
    };
    format!("{prefix}{suffix}")
}

impl HashBackend for StubBackend {
    fn run_batch(&mut self, request: &HashRequest) -> HashResult {
        let mut result = HashResult {
            request_id: request.request_id.clone(),
            algorithm: request.algorithm.clone(),
            backend: if request.backend == "reference" {
                "reference-stub".into()
            } else {
                "cpu-stub".into()
            },
            device_id: request.device_id,
            batch_size: request.batch_size,
            ..HashResult::default()
        };
        let errors = validate_request(request);
        if !errors.is_empty() {
            result.error = join_errors(&errors);
            return result;
        }
        if request.backend == "cuda" {
            result.backend = "cuda".into();
            result.error = "cuda backend is not available in the stub backend".into();
            return result;
        }

        let salt = normalize_hex(&request.salt_hex);
        let prefix = normalize_hex(&request.key_prefix);
        let fixed_key = normalize_hex(&request.key);
        let single = !fixed_key.is_empty();
        let attempts = if single { 1 } else { request.batch_size };

        for i in 0..attempts {
            let key = if single {
                fixed_key.clone()
            } else {
                make_key(&prefix, i)
            };
            let hash = stub_hash(&salt, &key, request.difficulty);
            if single {
                result.hash = hash.clone();
            }
            append_matches(request, &mut result, &key, &hash, i);
        }
        result.ok = true;
        result.attempts = attempts;
        result.batch_size = attempts;
        result.batch_size_min = attempts;
        result.batch_size_max = attempts;
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn batch_is_ok_and_attempts_match() {
        let mut b = StubBackend;
        let r = b.run_batch(&req());
        assert!(r.ok, "{}", r.error);
        assert_eq!(r.attempts, 2);
        assert_eq!(r.backend, "cpu-stub");
        assert_eq!(r.matches.len(), 2);
        assert_eq!(r.matches[0].key.len(), HASH_API_KEY_LENGTH);
    }

    #[test]
    fn fixed_key_is_hash_one() {
        let mut b = StubBackend;
        let mut r = req();
        r.key = "a".repeat(64);
        r.batch_size = 99;
        let out = b.run_batch(&r);
        assert!(out.ok);
        assert_eq!(out.attempts, 1);
        assert!(out.hash.starts_with("stub$argon2id-xen$"));
        assert!(out.hash.contains(&r.key));
    }

    #[test]
    fn cuda_is_rejected_by_the_stub() {
        let mut b = StubBackend;
        let mut r = req();
        r.backend = "cuda".into();
        let out = b.run_batch(&r);
        assert!(!out.ok);
        assert!(out.error.contains("cuda backend is not available"));
    }

    #[test]
    fn make_key_pads_to_64() {
        assert_eq!(make_key("", 0).len(), 64);
        assert_eq!(make_key("ab", 0), format!("ab{}", "0".repeat(62)));
        assert_eq!(make_key("ab", 0xff), format!("ab{}ff", "0".repeat(60)));
    }
}
