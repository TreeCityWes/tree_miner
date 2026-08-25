//! Output matching. Port of `src/hashapi/HashApiMatching.cpp`.

use crate::types::{HashMatch, HashRequest, HashResult};

pub fn is_superblock_hash(hash: &str) -> bool {
    hash.chars().filter(|c| c.is_ascii_uppercase()).count() >= 50
}

pub fn has_xuni_match(hash: &str) -> bool {
    let bytes = hash.as_bytes();
    let mut from = 0usize;
    while from < bytes.len() {
        let rest = &hash[from..];
        match rest.find("XUNI") {
            None => return false,
            Some(rel) => {
                let offset = from + rel;
                let digit_offset = offset + 4;
                if digit_offset < bytes.len() && bytes[digit_offset].is_ascii_digit() {
                    return true;
                }
                from = offset + 1;
            }
        }
    }
    false
}

pub fn append_matches(
    request: &HashRequest,
    result: &mut HashResult,
    key: &str,
    hash: &str,
    attempt_index: usize,
) {
    if hash.contains(&request.target_pattern) {
        result.matches.push(HashMatch {
            key: key.to_string(),
            hash: hash.to_string(),
            matched_pattern: request.target_pattern.clone(),
            attempt_index,
            is_superblock: is_superblock_hash(hash),
        });
    }
    if request.allow_xuni && has_xuni_match(hash) {
        result.matches.push(HashMatch {
            key: key.to_string(),
            hash: hash.to_string(),
            matched_pattern: "XUNI".into(),
            attempt_index,
            is_superblock: false,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn superblock_counts_ascii_uppercase() {
        let hash = "A".repeat(50);
        assert!(is_superblock_hash(&hash));
        assert!(!is_superblock_hash(&"A".repeat(49)));
        assert!(!is_superblock_hash("aaaaaaaaaa"));
    }

    #[test]
    fn xuni_requires_a_digit_after_the_prefix() {
        assert!(has_xuni_match("xxXUNI7tail"));
        assert!(!has_xuni_match("XUNI"));
        assert!(!has_xuni_match("XUNIx"));
        assert!(!has_xuni_match("nope"));
        assert!(has_xuni_match("XUNIXUNI1"));
    }

    #[test]
    fn append_matches_pattern_and_optional_xuni() {
        let mut req = HashRequest {
            target_pattern: "XEN11".into(),
            allow_xuni: true,
            ..HashRequest::default()
        };
        let mut result = HashResult::default();
        append_matches(&req, &mut result, "k", "fooXEN11barXUNI9", 3);
        assert_eq!(result.matches.len(), 2);
        assert_eq!(result.matches[0].matched_pattern, "XEN11");
        assert_eq!(result.matches[0].attempt_index, 3);
        assert_eq!(result.matches[1].matched_pattern, "XUNI");
        req.allow_xuni = false;
        result.matches.clear();
        append_matches(&req, &mut result, "k", "fooXEN11barXUNI9", 0);
        assert_eq!(result.matches.len(), 1);
    }
}
