//! Pure, deterministic classification of XenBlocks `/verify` responses.
//! Port of `src/submit/ResponseClassifier.{h,cpp}`. Truth table: PLAN.md §3.2 / gpage.py.

use crate::types::{Classification, FindKind, FindStatus};

/// Sentinel HTTP status for transport-level failures (connect / timeout / DNS).
/// Upstream cpr also reports `status_code` 0 on transport errors.
pub const TRANSPORT_ERROR: i32 = 0;

pub fn classify(http_status: i32, body: &str, kind: FindKind) -> Classification {
    classify_with_retry_after(http_status, body, kind, None)
}

pub fn classify_with_retry_after(
    http_status: i32,
    body: &str,
    kind: FindKind,
    retry_after: Option<&str>,
) -> Classification {
    if http_status <= 0 {
        return Classification::pending("transport failure; will retry with backoff");
    }

    if body.is_empty() || is_blank(body) {
        return Classification::pending(format!(
            "empty response body (http {http_status}); will retry with backoff"
        ));
    }

    let owned_message = extract_json_message(body);
    let message = owned_message.as_deref().unwrap_or(body);

    if http_status == 200 {
        return Classification {
            next_status: FindStatus::AcceptedUnconfirmed,
            server_difficulty_hint: None,
            needs_lookup_confirmation: true,
            reason: "http 200; awaiting /get_block confirmation".to_string(),
        };
    }

    if http_status == 429 {
        let mut c = Classification::pending("rate limited (429)");
        if let Some(header) = retry_after {
            if let Some(secs) = parse_retry_after_seconds(header) {
                c.reason.push_str(&format!("; retry_after_s={secs}"));
            }
        }
        return c;
    }

    if http_status == 408 || http_status == 425 || http_status >= 500 {
        return Classification::pending(format!(
            "server unhealthy (http {http_status}); will retry with backoff"
        ));
    }

    if http_status == 400 {
        if message.contains("already exists") {
            return Classification {
                next_status: FindStatus::AcceptedUnconfirmed,
                server_difficulty_hint: None,
                needs_lookup_confirmation: true,
                reason: "duplicate key (already exists); confirming via /get_block".to_string(),
            };
        }
        return Classification::quarantined(format!("unrecognized 400: {message}"));
    }

    if http_status == 401 {
        if message.contains("Hash verification failed") {
            return Classification {
                next_status: FindStatus::PermanentlyInvalid,
                server_difficulty_hint: None,
                needs_lookup_confirmation: false,
                reason: "server rejected: hash verification failed".to_string(),
            };
        }

        if message.contains("outside of proper time frame")
            || message.contains("outside of time window")
        {
            if kind == FindKind::Xuni {
                return Classification {
                    next_status: FindStatus::ParkedXuniWindow,
                    server_difficulty_hint: None,
                    needs_lookup_confirmation: false,
                    reason: "XUNI outside server time window; parked for a later window"
                        .to_string(),
                };
            }
            return Classification::quarantined(format!(
                "IMPOSSIBLE: XUNI-window rejection for a XEN11 record — server semantics \
                 changed, investigate: {message}"
            ));
        }

        if let Some(hint) = parse_difficulty_hint(message) {
            return Classification {
                next_status: FindStatus::ParkedDifficulty,
                server_difficulty_hint: Some(hint),
                needs_lookup_confirmation: false,
                reason: format!(
                    "difficulty too low (server currently at m={hint}); parked until difficulty falls"
                ),
            };
        }

        return Classification::quarantined(format!("unrecognized 401: {message}"));
    }

    Classification::quarantined(format!(
        "unrecognized response (http {http_status}): {message}"
    ))
}

pub fn extract_json_field(body: &str, key: &str) -> Option<String> {
    let mut i = 0usize;
    skip_ws(body, &mut i);
    let bytes = body.as_bytes();
    if i >= bytes.len() || bytes[i] != b'{' {
        return None;
    }
    i += 1;
    skip_ws(body, &mut i);
    if i < bytes.len() && bytes[i] == b'}' {
        return None;
    }
    while i < bytes.len() {
        skip_ws(body, &mut i);
        let k = parse_json_string(body, &mut i)?;
        skip_ws(body, &mut i);
        if i >= bytes.len() || bytes[i] != b':' {
            return None;
        }
        i += 1;
        if k == key {
            return capture_scalar(body, &mut i);
        }
        if !skip_json_value(body, &mut i) {
            return None;
        }
        skip_ws(body, &mut i);
        if i < bytes.len() && bytes[i] == b',' {
            i += 1;
            continue;
        }
        break;
    }
    None
}

pub fn extract_json_message(body: &str) -> Option<String> {
    extract_json_field(body, "message").or_else(|| extract_json_field(body, "error"))
}

pub fn parse_difficulty_hint(message: &str) -> Option<u32> {
    let mut search_from = 0usize;
    while let Some(rel) = message[search_from..].find("m=") {
        let pos = search_from + rel;
        let mut d = pos + 2;
        let bytes = message.as_bytes();
        if d >= bytes.len() || !bytes[d].is_ascii_digit() {
            search_from = pos + 1;
            continue;
        }
        let mut value: u64 = 0;
        while d < bytes.len() && bytes[d].is_ascii_digit() {
            value = value * 10 + u64::from(bytes[d] - b'0');
            if value > u64::from(u32::MAX) {
                return None;
            }
            d += 1;
        }
        return Some(value as u32);
    }
    None
}

pub fn parse_retry_after_seconds(header_value: &str) -> Option<i64> {
    let mut i = 0usize;
    skip_ws(header_value, &mut i);
    let bytes = header_value.as_bytes();
    if i >= bytes.len() || !bytes[i].is_ascii_digit() {
        return None;
    }
    let mut value: i64 = 0;
    while i < bytes.len() && bytes[i].is_ascii_digit() {
        if value > 100_000_000 {
            return None;
        }
        value = value * 10 + i64::from(bytes[i] - b'0');
        i += 1;
    }
    skip_ws(header_value, &mut i);
    if i != bytes.len() {
        return None;
    }
    Some(value)
}

fn is_blank(s: &str) -> bool {
    s.chars().all(char::is_whitespace)
}

fn skip_ws(s: &str, i: &mut usize) {
    let bytes = s.as_bytes();
    while *i < bytes.len() && bytes[*i].is_ascii_whitespace() {
        *i += 1;
    }
}

fn parse_json_string(s: &str, i: &mut usize) -> Option<String> {
    let bytes = s.as_bytes();
    if *i >= bytes.len() || bytes[*i] != b'"' {
        return None;
    }
    *i += 1;
    let mut out = String::new();
    while *i < bytes.len() {
        let c = bytes[*i];
        if c == b'"' {
            *i += 1;
            return Some(out);
        }
        if c == b'\\' {
            *i += 1;
            if *i >= bytes.len() {
                return None;
            }
            let e = bytes[*i];
            match e {
                b'"' => out.push('"'),
                b'\\' => out.push('\\'),
                b'/' => out.push('/'),
                b'b' => out.push('\u{0008}'),
                b'f' => out.push('\u{000c}'),
                b'n' => out.push('\n'),
                b'r' => out.push('\r'),
                b't' => out.push('\t'),
                b'u' => {
                    if *i + 4 >= bytes.len() {
                        return None;
                    }
                    let mut code: u32 = 0;
                    for k in 1..=4 {
                        let h = bytes[*i + k];
                        code <<= 4;
                        code |= match h {
                            b'0'..=b'9' => u32::from(h - b'0'),
                            b'a'..=b'f' => u32::from(h - b'a' + 10),
                            b'A'..=b'F' => u32::from(h - b'A' + 10),
                            _ => return None,
                        };
                    }
                    if code < 0x80 {
                        out.push(char::from(code as u8));
                    } else {
                        out.push('?');
                    }
                    *i += 4;
                }
                _ => return None,
            }
            *i += 1;
        } else {
            out.push(char::from(c));
            *i += 1;
        }
    }
    None
}

fn skip_json_value(s: &str, i: &mut usize) -> bool {
    skip_ws(s, i);
    let bytes = s.as_bytes();
    if *i >= bytes.len() {
        return false;
    }
    let c = bytes[*i];
    if c == b'"' {
        return parse_json_string(s, i).is_some();
    }
    if c == b'{' || c == b'[' {
        let open = c;
        let close = if c == b'{' { b'}' } else { b']' };
        let mut depth = 0i32;
        while *i < bytes.len() {
            let d = bytes[*i];
            if d == b'"' {
                if parse_json_string(s, i).is_none() {
                    return false;
                }
                continue;
            }
            if d == open {
                depth += 1;
            }
            if d == close {
                depth -= 1;
                if depth == 0 {
                    *i += 1;
                    return true;
                }
            }
            *i += 1;
        }
        return false;
    }
    while *i < bytes.len()
        && bytes[*i] != b','
        && bytes[*i] != b'}'
        && bytes[*i] != b']'
        && !bytes[*i].is_ascii_whitespace()
    {
        *i += 1;
    }
    true
}

fn capture_scalar(s: &str, i: &mut usize) -> Option<String> {
    skip_ws(s, i);
    let bytes = s.as_bytes();
    if *i >= bytes.len() {
        return None;
    }
    if bytes[*i] == b'"' {
        return parse_json_string(s, i);
    }
    if bytes[*i] == b'{' || bytes[*i] == b'[' {
        skip_json_value(s, i);
        return None;
    }
    let start = *i;
    while *i < bytes.len()
        && bytes[*i] != b','
        && bytes[*i] != b'}'
        && bytes[*i] != b']'
        && !bytes[*i].is_ascii_whitespace()
    {
        *i += 1;
    }
    Some(s[start..*i].to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn success_200_is_only_accepted_unconfirmed() {
        let body = r#"{"message": "Hash verified successfully and block saved."}"#;
        let c = classify(200, body, FindKind::Xen11);
        assert_eq!(c.next_status, FindStatus::AcceptedUnconfirmed);
        assert!(c.needs_lookup_confirmation);
        assert!(c.server_difficulty_hint.is_none());
        let c = classify(200, body, FindKind::Xuni);
        assert_eq!(c.next_status, FindStatus::AcceptedUnconfirmed);
        assert!(c.needs_lookup_confirmation);
    }

    #[test]
    fn duplicate_400_acks_via_lookup() {
        let c = classify(
            400,
            r#"{"message": "Block already exists, continue"}"#,
            FindKind::Xen11,
        );
        assert_eq!(c.next_status, FindStatus::AcceptedUnconfirmed);
        assert!(c.needs_lookup_confirmation);
        assert!(c.reason.contains("duplicate"));

        let c = classify(400, "Block already exists, continue", FindKind::Xuni);
        assert_eq!(c.next_status, FindStatus::AcceptedUnconfirmed);
        assert!(c.needs_lookup_confirmation);
    }

    #[test]
    fn difficulty_401_parks_and_surfaces_hint() {
        let c = classify(
            401,
            r#"{"message": "Hash does not contain 'm=104000'. Your memory_cost setting in your miner will be autoadjusted."}"#,
            FindKind::Xen11,
        );
        assert_eq!(c.next_status, FindStatus::ParkedDifficulty);
        assert_eq!(c.server_difficulty_hint, Some(104000));
        assert!(!c.needs_lookup_confirmation);

        let c = classify(
            401,
            "Hash does not contain 'm=99000'. Your memory_cost setting in your miner will be autoadjusted.",
            FindKind::Xen11,
        );
        assert_eq!(c.next_status, FindStatus::ParkedDifficulty);
        assert_eq!(c.server_difficulty_hint, Some(99000));
    }

    #[test]
    fn xuni_window_current_string_parks_xuni() {
        let c = classify(
            401,
            r#"{"message": "XUNI Submitted outside of proper time frame."}"#,
            FindKind::Xuni,
        );
        assert_eq!(c.next_status, FindStatus::ParkedXuniWindow);
    }

    #[test]
    fn xuni_window_legacy_string_parks_xuni() {
        let c = classify(
            401,
            r#"{"message": "XUNI found outside of time window"}"#,
            FindKind::Xuni,
        );
        assert_eq!(c.next_status, FindStatus::ParkedXuniWindow);
    }

    #[test]
    fn xuni_window_for_xen11_is_quarantined_loudly() {
        for body in [
            r#"{"message": "XUNI Submitted outside of proper time frame."}"#,
            r#"{"message": "XUNI found outside of time window"}"#,
        ] {
            let c = classify(401, body, FindKind::Xen11);
            assert_eq!(c.next_status, FindStatus::Quarantined);
            assert!(c.reason.contains("IMPOSSIBLE"));
        }
    }

    #[test]
    fn verification_failure_is_permanently_invalid() {
        let body = r#"{"message": "Hash verification failed."}"#;
        assert_eq!(
            classify(401, body, FindKind::Xen11).next_status,
            FindStatus::PermanentlyInvalid
        );
        assert_eq!(
            classify(401, body, FindKind::Xuni).next_status,
            FindStatus::PermanentlyInvalid
        );
    }

    #[test]
    fn rate_limit_429_honors_retry_after() {
        let body = r#"{"message": "slow down"}"#;
        assert_eq!(
            classify(429, body, FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        let c = classify_with_retry_after(429, body, FindKind::Xen11, Some("30"));
        assert_eq!(c.next_status, FindStatus::Pending);
        assert!(c.reason.contains("retry_after_s=30"));
        let c = classify_with_retry_after(
            429,
            body,
            FindKind::Xen11,
            Some("Wed, 21 Oct 2015 07:28:00 GMT"),
        );
        assert_eq!(c.next_status, FindStatus::Pending);
        assert!(!c.reason.contains("retry_after_s="));
    }

    #[test]
    fn transport_class_failures_stay_pending() {
        assert_eq!(
            classify(TRANSPORT_ERROR, "", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(408, "Request Timeout", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(425, "Too Early", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(500, "<html>Internal Server Error</html>", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(502, "Bad Gateway", FindKind::Xuni).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(
                503,
                r#"{"message": "Service Unavailable"}"#,
                FindKind::Xen11
            )
            .next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(200, "", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
        assert_eq!(
            classify(200, "   \n", FindKind::Xen11).next_status,
            FindStatus::Pending
        );
    }

    #[test]
    fn unknown_responses_quarantine_never_drop() {
        assert_eq!(
            classify(400, r#"{"error": "Invalid key format"}"#, FindKind::Xen11).next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(400, r#"{"error": "Invalid salt format"}"#, FindKind::Xen11).next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(
                400,
                r#"{"error": "Missing hash_to_verify, key, or account"}"#,
                FindKind::Xen11
            )
            .next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(
                401,
                r#"{"message": "Hash does not contain any of the valid targets ['XEN11'] in the last 87 characters. Adjust target_substr in your miner."}"#,
                FindKind::Xen11
            )
            .next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(403, "Forbidden", FindKind::Xen11).next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(404, "Not Found", FindKind::Xen11).next_status,
            FindStatus::Quarantined
        );
        assert_eq!(
            classify(301, "Moved Permanently", FindKind::Xen11).next_status,
            FindStatus::Quarantined
        );
    }

    #[test]
    fn json_message_field_wins_over_incidental_body_text() {
        let c = classify(
            401,
            r#"{"debug": "Hash verification failed.", "message": "totally new response"}"#,
            FindKind::Xen11,
        );
        assert_eq!(c.next_status, FindStatus::Quarantined);

        let c = classify(
            401,
            r#"{"message": "Hash does not contain \u0027m=123456\u0027."}"#,
            FindKind::Xen11,
        );
        assert_eq!(c.next_status, FindStatus::ParkedDifficulty);
        assert_eq!(c.server_difficulty_hint, Some(123456));
    }

    #[test]
    fn extract_json_field_and_parse_difficulty_hint_helpers() {
        assert_eq!(
            extract_json_field(r#"{"difficulty": "104000"}"#, "difficulty").as_deref(),
            Some("104000")
        );
        assert_eq!(
            extract_json_field(r#"{"a": 1, "difficulty": 98000}"#, "difficulty").as_deref(),
            Some("98000")
        );
        assert!(extract_json_field("not json", "difficulty").is_none());
        assert!(extract_json_field("{}", "difficulty").is_none());

        assert_eq!(parse_difficulty_hint("m=1234 tail"), Some(1234));
        assert!(parse_difficulty_hint("no hint here").is_none());
        assert!(parse_difficulty_hint("m=99999999999999").is_none());
        assert_eq!(parse_difficulty_hint("memory m=x then m=42"), Some(42));

        assert_eq!(parse_retry_after_seconds("120"), Some(120));
        assert_eq!(parse_retry_after_seconds(" 5 "), Some(5));
        assert!(parse_retry_after_seconds("Wed, 21 Oct 2015 07:28:00 GMT").is_none());
    }

    #[test]
    fn classification_is_deterministic() {
        let body = r#"{"message": "Hash does not contain 'm=104000'."}"#;
        let a = classify(401, body, FindKind::Xen11);
        let b = classify(401, body, FindKind::Xen11);
        assert_eq!(a.next_status, b.next_status);
        assert_eq!(a.server_difficulty_hint, b.server_difficulty_hint);
        assert_eq!(a.reason, b.reason);
    }
}
