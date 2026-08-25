//! Hash API CLI (`hash-one` / `hash-batch` / `hash-help`). Uses the hash-batch C ABI
//! (Cargo stub, or `treeminer_hash.cpp` when a miner build links it). No CUDA here.

use std::collections::BTreeMap;

use treeminer_hash::{HashBackend, HashRequest, HashResult};

pub const HASH_USAGE: &str = "\
Hash API commands:
  treeminer hash-one --salt <hex> --key <64-hex> [--backend cpu|cuda] [--difficulty <n>] [--no-xuni] [--json]
  treeminer hash-batch --salt <hex> [--backend cpu|cuda] [--prefix <hex>] [--pattern XEN11] [--batch-size <n>] [--difficulty <n>] [--no-xuni] [--json]
  treeminer hash-help
hash-benchmark stays on xenblocksMiner until CUDA is linked into this host.
";

pub fn hash_request_from_flags(flags: &BTreeMap<String, String>) -> Result<HashRequest, String> {
    let mut request = HashRequest::default();
    if let Some(v) = flags.get("request-id") {
        request.request_id = v.clone();
    }
    if let Some(v) = flags.get("backend") {
        request.backend = v.clone();
    }
    if let Some(v) = flags.get("salt") {
        request.salt_hex = v.clone();
    }
    if let Some(v) = flags.get("key") {
        request.key = v.clone();
    }
    if let Some(v) = flags.get("prefix") {
        request.key_prefix = v.clone();
    }
    if let Some(v) = flags.get("pattern") {
        request.target_pattern = v.clone();
    }
    if let Some(v) = flags.get("difficulty") {
        request.difficulty = parse_u32(v, "difficulty")?;
    }
    if let Some(v) = flags.get("batch-size") {
        request.batch_size = parse_usize(v, "batch-size")?;
    }
    if let Some(v) = flags.get("device") {
        request.device_id = parse_u32(v, "device")? as i32;
    }
    request.allow_xuni = flags.get("no-xuni").map(|s| s != "true").unwrap_or(true);
    request.detailed_timings = flags
        .get("detailed-timings")
        .map(|s| s == "true")
        .unwrap_or(false);
    if let Some(v) = flags.get("first-block-workers") {
        request.first_block_workers = parse_usize(v, "first-block-workers")?;
    }
    if let Some(v) = flags.get("first-block-dynamic-chunk-size") {
        request.first_block_dynamic_chunk_size = parse_usize(v, "first-block-dynamic-chunk-size")?;
    }
    request.first_block_dynamic_chunk_auto = flags
        .get("first-block-dynamic-chunk-auto")
        .map(|s| s == "true")
        .unwrap_or(false);
    request.gpu_first_blocks = flags
        .get("gpu-first-blocks")
        .map(|s| s == "true")
        .unwrap_or(false);
    Ok(request)
}

fn parse_u32(text: &str, name: &str) -> Result<u32, String> {
    text.parse::<u32>()
        .map_err(|_| format!("{name} must be an unsigned integer"))
}

fn parse_usize(text: &str, name: &str) -> Result<usize, String> {
    text.parse::<usize>()
        .map_err(|_| format!("{name} must be an unsigned integer"))
}

pub fn format_hash_result(result: &HashResult, json: bool) -> (String, String, i32) {
    if json {
        let stdout = format_hash_json(result);
        let code = if result.ok { 0 } else { 2 };
        return (stdout, String::new(), code);
    }
    if !result.ok {
        return (
            String::new(),
            format!("Hash API error: {}\n", result.error),
            2,
        );
    }
    let mut stdout = format!(
        "ok=true backend={} attempts={} hashrate={} matches={}\n",
        result.backend,
        result.attempts,
        result.hashrate,
        result.matches.len()
    );
    if !result.hash.is_empty() {
        stdout.push_str(&format!("hash={}\n", result.hash));
    }
    (stdout, String::new(), 0)
}

fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn format_hash_json(result: &HashResult) -> String {
    let matches: String = result
        .matches
        .iter()
        .map(|m| {
            format!(
                "{{\"key\":{},\"hash\":{},\"matched_pattern\":{},\"attempt_index\":{},\"is_superblock\":{}}}",
                json_escape(&m.key),
                json_escape(&m.hash),
                json_escape(&m.matched_pattern),
                m.attempt_index,
                if m.is_superblock { "true" } else { "false" },
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    format!(
        "{{\"ok\":{},\"error\":{},\"backend\":{},\"attempts\":{},\"hashrate\":{},\"hash\":{},\"matches\":[{}]}}\n",
        if result.ok { "true" } else { "false" },
        json_escape(&result.error),
        json_escape(&result.backend),
        result.attempts,
        result.hashrate,
        json_escape(&result.hash),
        matches,
    )
}

pub fn run_hash(backend: &mut impl HashBackend, request: &HashRequest) -> HashResult {
    backend.run_batch(request)
}
