//! Hand-rolled JSONL for the fallback sink. No serde: this path runs when SQLite is
//! already broken and must not grow extra dependencies. Byte-exact round-trip of
//! FoundPayload fields, matching `src/journal/FallbackSink.cpp`.

use std::collections::BTreeMap;

use treeminer_protocol::{FindKind, FoundPayload};

pub fn serialize(payload: &FoundPayload) -> String {
    let mut out = String::with_capacity(512);
    out.push('{');
    append_field(&mut out, "key", &payload.key, true);
    append_field(&mut out, "hash_to_verify", &payload.hash_to_verify, false);
    append_field(&mut out, "account", &payload.account, false);
    append_field(&mut out, "kind", payload.kind.as_str(), false);
    append_raw_field(
        &mut out,
        "memory_cost",
        &payload.memory_cost.to_string(),
        false,
    );
    append_field(&mut out, "worker", &payload.worker, false);
    append_raw_field(&mut out, "attempts", &payload.attempts.to_string(), false);
    append_raw_field(
        &mut out,
        "hashes_per_second",
        &format_rate(payload.hashes_per_second),
        false,
    );
    append_field(&mut out, "found_at_utc", &payload.found_at_utc, false);
    out.push('}');
    out.push('\n');
    out
}

pub fn parse_payload(line: &str) -> Option<FoundPayload> {
    let fields = parse_flat_object(line)?;
    payload_from_fields(&fields)
}

fn format_rate(v: f64) -> String {
    if !v.is_finite() {
        return "0".to_string();
    }
    // Rust Display for f64 is a shortest round-trip; C++ uses %.17g. Both must parse
    // back to the original bits for the sink's equality tests.
    let s = v.to_string();
    if s.bytes().any(|b| matches!(b, b'n' | b'i' | b'N' | b'I')) {
        "0".to_string()
    } else {
        s
    }
}

fn append_json_string(out: &mut String, value: &str) {
    let mut buf: Vec<u8> = Vec::with_capacity(value.len() + 2);
    buf.push(b'"');
    for &c in value.as_bytes() {
        match c {
            b'"' => buf.extend_from_slice(br#"\""#),
            b'\\' => buf.extend_from_slice(br#"\\"#),
            b'\x08' => buf.extend_from_slice(br#"\b"#),
            b'\x0c' => buf.extend_from_slice(br#"\f"#),
            b'\n' => buf.extend_from_slice(br#"\n"#),
            b'\r' => buf.extend_from_slice(br#"\r"#),
            b'\t' => buf.extend_from_slice(br#"\t"#),
            c if c < 0x20 => {
                const HEX: &[u8] = b"0123456789abcdef";
                buf.extend_from_slice(br#"\u00"#);
                buf.push(HEX[(c >> 4) as usize]);
                buf.push(HEX[(c & 0x0f) as usize]);
            }
            c => buf.push(c),
        }
    }
    buf.push(b'"');
    out.push_str(std::str::from_utf8(&buf).expect("escaped JSON is UTF-8"));
}

fn append_field(out: &mut String, key: &str, value: &str, first: bool) {
    if !first {
        out.push(',');
    }
    append_json_string(out, key);
    out.push(':');
    append_json_string(out, value);
}

fn append_raw_field(out: &mut String, key: &str, literal: &str, first: bool) {
    if !first {
        out.push(',');
    }
    append_json_string(out, key);
    out.push(':');
    out.push_str(literal);
}

fn skip_ws(s: &str, i: &mut usize) {
    let b = s.as_bytes();
    while *i < b.len() && matches!(b[*i], b' ' | b'\t' | b'\r' | b'\n') {
        *i += 1;
    }
}

fn hex_nibble(c: u8) -> Option<u32> {
    match c {
        b'0'..=b'9' => Some(u32::from(c - b'0')),
        b'a'..=b'f' => Some(u32::from(c - b'a' + 10)),
        b'A'..=b'F' => Some(u32::from(c - b'A' + 10)),
        _ => None,
    }
}

fn parse_json_string(s: &str, i: &mut usize) -> Option<String> {
    let b = s.as_bytes();
    if *i >= b.len() || b[*i] != b'"' {
        return None;
    }
    *i += 1;
    let mut out: Vec<u8> = Vec::new();
    while *i < b.len() {
        let c = b[*i];
        if c == b'"' {
            *i += 1;
            return String::from_utf8(out).ok();
        }
        if c == b'\\' {
            *i += 1;
            if *i >= b.len() {
                return None;
            }
            let esc = b[*i];
            *i += 1;
            match esc {
                b'"' => out.push(b'"'),
                b'\\' => out.push(b'\\'),
                b'/' => out.push(b'/'),
                b'b' => out.push(b'\x08'),
                b'f' => out.push(b'\x0c'),
                b'n' => out.push(b'\n'),
                b'r' => out.push(b'\r'),
                b't' => out.push(b'\t'),
                b'u' => {
                    if *i + 4 > b.len() {
                        return None;
                    }
                    let mut cp = 0u32;
                    for k in 0..4 {
                        cp = (cp << 4) | hex_nibble(b[*i + k])?;
                    }
                    *i += 4;
                    let ch = char::from_u32(cp)?;
                    let mut buf = [0u8; 4];
                    out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                }
                _ => return None,
            }
            continue;
        }
        out.push(c);
        *i += 1;
    }
    None
}

fn parse_flat_object(line: &str) -> Option<BTreeMap<String, String>> {
    let n = line.len();
    let mut i = 0usize;
    skip_ws(line, &mut i);
    let b = line.as_bytes();
    if i >= n || b[i] != b'{' {
        return None;
    }
    i += 1;
    skip_ws(line, &mut i);
    let mut fields = BTreeMap::new();
    if i < n && b[i] == b'}' {
        i += 1;
        skip_ws(line, &mut i);
        return if i == n { Some(fields) } else { None };
    }
    loop {
        skip_ws(line, &mut i);
        let key = parse_json_string(line, &mut i)?;
        skip_ws(line, &mut i);
        if i >= n || line.as_bytes()[i] != b':' {
            return None;
        }
        i += 1;
        skip_ws(line, &mut i);
        if i >= n {
            return None;
        }
        let b = line.as_bytes();
        let value = if b[i] == b'"' {
            parse_json_string(line, &mut i)?
        } else if b[i] == b'{' || b[i] == b'[' {
            return None;
        } else {
            let start = i;
            while i < n && !matches!(b[i], b',' | b'}' | b' ' | b'\t' | b'\r' | b'\n') {
                i += 1;
            }
            if i == start {
                return None;
            }
            line[start..i].to_string()
        };
        fields.insert(key, value);
        skip_ws(line, &mut i);
        let b = line.as_bytes();
        if i < n && b[i] == b',' {
            i += 1;
            continue;
        }
        break;
    }
    let b = line.as_bytes();
    if i >= n || b[i] != b'}' {
        return None;
    }
    i += 1;
    skip_ws(line, &mut i);
    if i == n {
        Some(fields)
    } else {
        None
    }
}

fn parse_u64(text: &str) -> Option<u64> {
    if text.is_empty() || text.starts_with('-') {
        return None;
    }
    text.parse().ok()
}

fn payload_from_fields(fields: &BTreeMap<String, String>) -> Option<FoundPayload> {
    let kind = match fields.get("kind")?.as_str() {
        "XEN11" => FindKind::Xen11,
        "XUNI" => FindKind::Xuni,
        _ => return None,
    };
    let memory_cost = parse_u64(fields.get("memory_cost")?)?;
    if memory_cost > u64::from(u32::MAX) {
        return None;
    }
    Some(FoundPayload {
        key: fields.get("key")?.clone(),
        hash_to_verify: fields.get("hash_to_verify")?.clone(),
        account: fields.get("account")?.clone(),
        kind,
        memory_cost: memory_cost as u32,
        worker: fields.get("worker")?.clone(),
        attempts: parse_u64(fields.get("attempts")?)?,
        hashes_per_second: fields.get("hashes_per_second")?.parse().ok()?,
        found_at_utc: fields.get("found_at_utc")?.clone(),
    })
}
