//! PHC-encoded Argon2id string from the parameters the GPU batch actually used.
//! Port of `src/treeminer/PhcAssembler.h` plus unpadded base64 from
//! `src/hashapi/HashApiEncoding.cpp` (PHC-compatible, no `=` padding).

use std::fmt;

const BASE64_CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum HexError {
    OddLength,
    NonHex { ch: char },
}

impl fmt::Display for HexError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::OddLength => f.write_str("hexToBytes: odd-length hex string"),
            Self::NonHex { ch } => write!(f, "hexToBytes: non-hex character {ch:?}"),
        }
    }
}

impl std::error::Error for HexError {}

pub fn hex_to_bytes(hex: &str) -> Result<Vec<u8>, HexError> {
    if hex.len() % 2 != 0 {
        return Err(HexError::OddLength);
    }
    let mut bytes = Vec::with_capacity(hex.len() / 2);
    let chars: Vec<char> = hex.chars().collect();
    for chunk in chars.chunks_exact(2) {
        bytes.push((nibble(chunk[0])? << 4) | nibble(chunk[1])?);
    }
    Ok(bytes)
}

fn nibble(c: char) -> Result<u8, HexError> {
    match c {
        '0'..='9' => Ok(c as u8 - b'0'),
        'a'..='f' => Ok(c as u8 - b'a' + 10),
        'A'..='F' => Ok(c as u8 - b'A' + 10),
        _ => Err(HexError::NonHex { ch: c }),
    }
}

/// Unpadded Base64 matching `hashapi::base64Encode`.
pub fn phc_base64_encode(bytes: &[u8]) -> String {
    let full_groups = bytes.len() / 3;
    let remaining = bytes.len() % 3;
    let out_len = full_groups * 4 + if remaining == 0 { 0 } else { remaining + 1 };
    let mut encoded = String::with_capacity(out_len);

    let mut offset = 0usize;
    while offset + 2 < bytes.len() {
        let value = (u32::from(bytes[offset]) << 16)
            | (u32::from(bytes[offset + 1]) << 8)
            | u32::from(bytes[offset + 2]);
        encoded.push(BASE64_CHARS[((value >> 18) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[((value >> 12) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[((value >> 6) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[(value & 0x3f) as usize] as char);
        offset += 3;
    }

    let leftover = bytes.len() - offset;
    if leftover == 1 {
        let value = u32::from(bytes[offset]) << 16;
        encoded.push(BASE64_CHARS[((value >> 18) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[((value >> 12) & 0x3f) as usize] as char);
    } else if leftover == 2 {
        let value = (u32::from(bytes[offset]) << 16) | (u32::from(bytes[offset + 1]) << 8);
        encoded.push(BASE64_CHARS[((value >> 18) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[((value >> 12) & 0x3f) as usize] as char);
        encoded.push(BASE64_CHARS[((value >> 6) & 0x3f) as usize] as char);
    }
    encoded
}

/// `hexsalt` is the 40-hex-char ETH address without `0x`. `digest_b64` is the unpadded
/// digest as produced on-GPU (`hashapi::base64Encode` format).
pub fn assemble_phc(memory_cost: u32, hexsalt: &str, digest_b64: &str) -> Result<String, HexError> {
    let salt_bytes = hex_to_bytes(hexsalt)?;
    let salt_b64 = phc_base64_encode(&salt_bytes);
    Ok(format!(
        "$argon2id$v=19$m={memory_cost},t=1,p=1${salt_b64}${digest_b64}"
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_to_bytes_rejects_odd_and_non_hex() {
        assert_eq!(hex_to_bytes("abc"), Err(HexError::OddLength));
        assert!(matches!(hex_to_bytes("zz"), Err(HexError::NonHex { .. })));
        assert_eq!(hex_to_bytes("00ff"), Ok(vec![0x00, 0xff]));
        assert_eq!(hex_to_bytes("0A0B"), Ok(vec![0x0a, 0x0b]));
    }

    #[test]
    fn unpadded_base64_matches_hashapi_length_rule() {
        assert_eq!(phc_base64_encode(&[]), "");
        assert_eq!(phc_base64_encode(&[0]), "AA");
        assert_eq!(phc_base64_encode(&[0, 0]), "AAA");
        assert_eq!(phc_base64_encode(&[0, 0, 0]), "AAAA");
        assert!(!phc_base64_encode(&[0]).contains('='));
        assert!(!phc_base64_encode(&[0, 0]).contains('='));
        // 20 zero bytes (ETH address width) → 27 unpadded chars.
        let twenty = [0u8; 20];
        let encoded = phc_base64_encode(&twenty);
        assert_eq!(encoded.len(), 27);
        assert_eq!(encoded, "AAAAAAAAAAAAAAAAAAAAAAAAAAA");
    }

    #[test]
    fn assemble_phc_uses_batch_memory_cost_and_hex_salt() {
        let salt = "0".repeat(40);
        let phc = assemble_phc(1100, &salt, "digestB64").unwrap();
        assert_eq!(
            phc,
            "$argon2id$v=19$m=1100,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAAAAAAA$digestB64"
        );
        let phc = assemble_phc(2100, &salt, "digestB64").unwrap();
        assert!(phc.contains("m=2100"));
        assert!(!phc.contains("m=1100"));
    }

    #[test]
    fn known_nonzero_salt_vector() {
        // 20-byte salt from a typical 0x-stripped address prefix.
        let hex = "aabbccddeeff00112233445566778899aabbccdd";
        let bytes = hex_to_bytes(hex).unwrap();
        assert_eq!(bytes.len(), 20);
        let phc = assemble_phc(100, hex, "xyz").unwrap();
        assert!(phc.starts_with("$argon2id$v=19$m=100,t=1,p=1$"));
        assert!(phc.ends_with("$xyz"));
        let salt_b64 = phc.split('$').nth(4).expect("PHC salt field");
        assert!(!salt_b64.is_empty());
        assert!(
            !salt_b64.contains('='),
            "PHC salt must be unpadded: {salt_b64}"
        );
    }
}
