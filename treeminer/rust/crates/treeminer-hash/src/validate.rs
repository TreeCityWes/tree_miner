//! Request validation. Port of `src/hashapi/HashApiValidation.cpp`.

use crate::types::{
    HashRequest, HASH_API_KEY_LENGTH, MAX_CPU_BATCH_SIZE, MAX_TARGET_PATTERN_LENGTH,
};

pub fn is_hex_string(value: &str) -> bool {
    value.bytes().all(|ch| ch.is_ascii_hexdigit())
}

pub fn normalize_hex(value: &str) -> String {
    let rest = if value.len() >= 2
        && value.as_bytes()[0] == b'0'
        && (value.as_bytes()[1] == b'x' || value.as_bytes()[1] == b'X')
    {
        &value[2..]
    } else {
        value
    };
    rest.chars().map(|c| c.to_ascii_lowercase()).collect()
}

pub fn validate_request(request: &HashRequest) -> Vec<String> {
    let mut errors = Vec::new();

    if request.algorithm != "argon2id-xen" {
        errors.push(format!("unsupported algorithm: {}", request.algorithm));
    }

    if request.backend != "cpu" && request.backend != "reference" && request.backend != "cuda" {
        errors.push(format!("unsupported backend: {}", request.backend));
    }

    let salt = normalize_hex(&request.salt_hex);
    if salt.is_empty() {
        errors.push("salt_hex is required".into());
    } else {
        if salt.len() % 2 != 0 {
            errors.push("salt_hex must contain an even number of hex characters".into());
        }
        if salt.len() < 16 {
            errors.push("salt_hex must be at least 16 hex characters".into());
        }
        if !is_hex_string(&salt) {
            errors.push("salt_hex must contain only hex characters".into());
        }
    }

    let prefix = normalize_hex(&request.key_prefix);
    if !prefix.is_empty() {
        if prefix.len() > HASH_API_KEY_LENGTH {
            errors.push("key_prefix cannot exceed 64 hex characters".into());
        }
        if !is_hex_string(&prefix) {
            errors.push("key_prefix must contain only hex characters".into());
        }
    }

    let key = normalize_hex(&request.key);
    if !key.is_empty() {
        if key.len() != HASH_API_KEY_LENGTH {
            errors.push("key must contain exactly 64 hex characters".into());
        }
        if !is_hex_string(&key) {
            errors.push("key must contain only hex characters".into());
        }
        if !prefix.is_empty() && !key.starts_with(&prefix) {
            errors.push("key must start with key_prefix when both are provided".into());
        }
    }

    if request.target_pattern.is_empty() {
        errors.push("target_pattern is required".into());
    }
    if request.target_pattern.len() > MAX_TARGET_PATTERN_LENGTH {
        errors.push("target_pattern is too long".into());
    }

    if request.difficulty == 0 {
        errors.push("difficulty must be greater than zero".into());
    }

    if request.batch_size == 0 {
        errors.push("batch_size must be greater than zero".into());
    }
    if request.backend == "cpu" || request.backend == "reference" {
        if request.batch_size > MAX_CPU_BATCH_SIZE {
            errors.push("cpu batch_size exceeds safe limit".into());
        }
    }

    if request.device_id < 0 {
        errors.push("device_id must be non-negative".into());
    }

    if request.gpu_first_blocks && request.backend != "cuda" {
        errors.push("gpu_first_blocks requires backend=cuda".into());
    }

    errors
}

pub fn is_valid_request(request: &HashRequest) -> bool {
    validate_request(request).is_empty()
}

pub fn join_errors(errors: &[String]) -> String {
    errors.join("; ")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base() -> HashRequest {
        HashRequest {
            salt_hex: "aabbccddeeff0011".into(),
            ..HashRequest::default()
        }
    }

    #[test]
    fn accepts_a_minimal_valid_cpu_request() {
        assert!(is_valid_request(&base()));
        assert!(validate_request(&base()).is_empty());
    }

    #[test]
    fn strips_0x_and_lowercases_hex() {
        assert_eq!(normalize_hex("0xABcd"), "abcd");
        assert_eq!(normalize_hex("0XFF"), "ff");
        assert_eq!(normalize_hex("Ee"), "ee");
    }

    #[test]
    fn rejects_unsupported_algorithm_and_backend() {
        let mut r = base();
        r.algorithm = "sha256".into();
        r.backend = "opencl".into();
        let e = join_errors(&validate_request(&r));
        assert!(e.contains("unsupported algorithm: sha256"));
        assert!(e.contains("unsupported backend: opencl"));
    }

    #[test]
    fn salt_rules() {
        let mut r = base();
        r.salt_hex.clear();
        assert!(join_errors(&validate_request(&r)).contains("salt_hex is required"));
        r.salt_hex = "abc".into();
        let e = join_errors(&validate_request(&r));
        assert!(e.contains("even number of hex characters"));
        assert!(e.contains("at least 16 hex characters"));
        r.salt_hex = "zzzzzzzzzzzzzzzz".into();
        assert!(join_errors(&validate_request(&r)).contains("only hex characters"));
    }

    #[test]
    fn key_and_prefix_rules() {
        let mut r = base();
        r.key_prefix = "1".repeat(65);
        assert!(join_errors(&validate_request(&r)).contains("key_prefix cannot exceed 64"));
        r.key_prefix = "gg".into();
        assert!(join_errors(&validate_request(&r)).contains("key_prefix must contain only hex"));
        r.key_prefix = "aa".into();
        r.key = "bb".into();
        let e = join_errors(&validate_request(&r));
        assert!(e.contains("key must contain exactly 64 hex characters"));
        r.key = "b".repeat(64);
        assert!(join_errors(&validate_request(&r)).contains("key must start with key_prefix"));
        r.key = format!("aa{}", "c".repeat(62));
        assert!(is_valid_request(&r));
    }

    #[test]
    fn pattern_difficulty_batch_device_gpu_flag() {
        let mut r = base();
        r.target_pattern.clear();
        assert!(join_errors(&validate_request(&r)).contains("target_pattern is required"));
        r.target_pattern = "x".repeat(129);
        assert!(join_errors(&validate_request(&r)).contains("target_pattern is too long"));
        r = base();
        r.difficulty = 0;
        assert!(join_errors(&validate_request(&r)).contains("difficulty must be greater than zero"));
        r = base();
        r.batch_size = 0;
        assert!(join_errors(&validate_request(&r)).contains("batch_size must be greater than zero"));
        r = base();
        r.batch_size = MAX_CPU_BATCH_SIZE + 1;
        assert!(join_errors(&validate_request(&r)).contains("cpu batch_size exceeds safe limit"));
        r.backend = "cuda".into();
        assert!(!join_errors(&validate_request(&r)).contains("cpu batch_size"));
        r = base();
        r.device_id = -1;
        assert!(join_errors(&validate_request(&r)).contains("device_id must be non-negative"));
        r = base();
        r.gpu_first_blocks = true;
        assert!(
            join_errors(&validate_request(&r)).contains("gpu_first_blocks requires backend=cuda")
        );
        r.backend = "cuda".into();
        assert!(is_valid_request(&r));
    }
}
