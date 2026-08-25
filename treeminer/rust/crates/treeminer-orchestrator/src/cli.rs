//! Host CLI. Hash commands first (as `main.cpp`), then recover / capture / drain.
//! No Boost.ProgramOptions, no Crow, no TUI.

use std::collections::BTreeMap;

use treeminer_hash::{FfiBackend, HashBackend};
use treeminer_submit::StepResult;

use crate::config::{load_config_txt, resolve_config, CliOverrides, DEFAULT_CONFIG_FILE};
use crate::hash_cli::{format_hash_result, hash_request_from_flags, HASH_USAGE};
use crate::host::{format_recovery, CaptureInput, Host};
use crate::http::HttpTransport;

pub const HOST_USAGE: &str = "\
TreeMiner orchestrator — journal-first host (CUDA mine loop stays in xenblocksMiner)

Commands:
  hash-one / hash-batch / hash-help
  recover   [--journalPath <file>] [--config <config.txt>]
  capture   --salt <hex> --key <64-hex> --digest <b64> --difficulty <m>
  drain     [--steps <n>] [--journalPath <file>] [--rpcLink <url>]
  --help

Flags (config.txt supplies defaults; CLI wins):
  --journalPath --difficultyMarginMode --difficultyMargin --difficultyMarginMax
  --minerAddr --rpcLink --worker-id --config
";

const BOOL_FLAGS: &[&str] = &[
    "help",
    "h",
    "json",
    "no-xuni",
    "detailed-timings",
    "auto-batch-size",
    "first-block-dynamic-chunk-auto",
    "gpu-first-blocks",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Command {
    Help,
    HashOne,
    HashBatch,
    HashHelp,
    HashBenchmark,
    Recover,
    Capture,
    Drain,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RunOutput {
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

impl RunOutput {
    fn ok(stdout: impl Into<String>) -> Self {
        Self {
            code: 0,
            stdout: stdout.into(),
            stderr: String::new(),
        }
    }
    fn err(code: i32, stderr: impl Into<String>) -> Self {
        Self {
            code,
            stdout: String::new(),
            stderr: stderr.into(),
        }
    }
}

#[derive(Clone, Debug)]
struct Parsed {
    command: Command,
    flags: BTreeMap<String, String>,
}

pub fn run<I, S>(args: I) -> RunOutput
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    let args: Vec<String> = args.into_iter().map(|s| s.as_ref().to_string()).collect();
    let parsed = match parse_args(&args) {
        Ok(p) => p,
        Err(msg) => return RunOutput::err(1, format!("{msg}\n")),
    };
    if parsed.flags.contains_key("help") || parsed.flags.contains_key("h") {
        if matches!(
            parsed.command,
            Command::HashOne | Command::HashBatch | Command::HashHelp | Command::HashBenchmark
        ) {
            return RunOutput::ok(HASH_USAGE);
        }
        return RunOutput::ok(HOST_USAGE);
    }
    match parsed.command {
        Command::Help => RunOutput::ok(HOST_USAGE),
        Command::HashHelp => RunOutput::ok(HASH_USAGE),
        Command::HashBenchmark => RunOutput::err(
            2,
            "hash-benchmark stays on xenblocksMiner until CUDA is linked into this host.\n",
        ),
        Command::HashOne | Command::HashBatch => run_hash_command(parsed.command, &parsed.flags),
        Command::Recover => run_recover(&parsed.flags),
        Command::Capture => run_capture(&parsed.flags),
        Command::Drain => run_drain(&parsed.flags),
    }
}

fn parse_args(args: &[String]) -> Result<Parsed, String> {
    let rest = if args.is_empty() { &[] } else { &args[1..] };
    if rest.is_empty() {
        return Ok(Parsed {
            command: Command::Help,
            flags: BTreeMap::new(),
        });
    }
    let mut i = 0usize;
    let command = if !rest[0].starts_with('-') {
        let cmd = parse_command(&rest[0])?;
        i = 1;
        cmd
    } else {
        Command::Help
    };
    let flags = parse_flags(&rest[i..]);
    Ok(Parsed { command, flags })
}

fn parse_command(text: &str) -> Result<Command, String> {
    match text {
        "hash-one" => Ok(Command::HashOne),
        "hash-batch" => Ok(Command::HashBatch),
        "hash-help" => Ok(Command::HashHelp),
        "hash-benchmark" => Ok(Command::HashBenchmark),
        "recover" => Ok(Command::Recover),
        "capture" => Ok(Command::Capture),
        "drain" => Ok(Command::Drain),
        "help" => Ok(Command::Help),
        other => Err(format!(
            "unknown command '{other}'. Try --help or hash-help."
        )),
    }
}

fn parse_flags(args: &[String]) -> BTreeMap<String, String> {
    let mut flags = BTreeMap::new();
    let mut i = 0usize;
    while i < args.len() {
        let arg = &args[i];
        if !arg.starts_with("--") && arg != "-h" {
            i += 1;
            continue;
        }
        let (key, inline) = if arg == "-h" {
            ("h".to_string(), None)
        } else if let Some(eq) = arg.find('=') {
            (arg[2..eq].to_string(), Some(arg[eq + 1..].to_string()))
        } else {
            (arg[2..].to_string(), None)
        };
        if let Some(value) = inline {
            flags.insert(key, value);
            i += 1;
            continue;
        }
        if BOOL_FLAGS.contains(&key.as_str()) {
            flags.insert(key, "true".into());
            i += 1;
            continue;
        }
        if i + 1 < args.len() && !args[i + 1].starts_with('-') {
            flags.insert(key, args[i + 1].clone());
            i += 2;
        } else {
            flags.insert(key, "true".into());
            i += 1;
        }
    }
    flags
}

fn load_resolved(flags: &BTreeMap<String, String>) -> Result<crate::HostConfig, String> {
    let cli = CliOverrides::from_flags(flags);
    let path = cli
        .config_path
        .clone()
        .unwrap_or_else(|| DEFAULT_CONFIG_FILE.into());
    let file = load_config_txt(&path);
    resolve_config(&file, &cli).map_err(|e| e.to_string())
}

fn run_hash_command(command: Command, flags: &BTreeMap<String, String>) -> RunOutput {
    let mut request = match hash_request_from_flags(flags) {
        Ok(r) => r,
        Err(e) => return RunOutput::err(2, format!("Hash API error: {e}\n")),
    };
    if command == Command::HashOne {
        request.batch_size = 1;
    }
    let json = flags.get("json").map(|s| s == "true").unwrap_or(false);
    let mut backend = FfiBackend;
    let result = backend.run_batch(&request);
    let (stdout, stderr, code) = format_hash_result(&result, json);
    RunOutput {
        code,
        stdout,
        stderr,
    }
}

fn run_recover(flags: &BTreeMap<String, String>) -> RunOutput {
    let cfg = match load_resolved(flags) {
        Ok(c) => c,
        Err(e) => return RunOutput::err(1, format!("{e}\n")),
    };
    match Host::open(&cfg.journal_path) {
        Ok(host) => RunOutput::ok(format_recovery(
            &host.abs_path,
            &host.import,
            &host.recovery,
        )),
        Err(e) => RunOutput::err(
            1,
            format!("JOURNAL cannot open {} | {e}\n", cfg.journal_path),
        ),
    }
}

fn run_capture(flags: &BTreeMap<String, String>) -> RunOutput {
    let cfg = match load_resolved(flags) {
        Ok(c) => c,
        Err(e) => return RunOutput::err(1, format!("{e}\n")),
    };
    let salt = flags.get("salt").cloned().unwrap_or_default();
    let key = flags.get("key").cloned().unwrap_or_default();
    let digest = flags.get("digest").cloned().unwrap_or_default();
    if salt.is_empty() || key.is_empty() || digest.is_empty() {
        return RunOutput::err(1, "capture requires --salt, --key, and --digest\n");
    }
    let memory_cost = flags
        .get("difficulty")
        .or_else(|| flags.get("m"))
        .and_then(|s| s.parse().ok())
        .unwrap_or(8);
    let attempts = flags
        .get("attempts")
        .and_then(|s| s.parse().ok())
        .unwrap_or(1);
    let hashrate = flags
        .get("hashrate")
        .and_then(|s| s.parse().ok())
        .unwrap_or(0.0);
    let worker = if cfg.worker.is_empty() {
        flags.get("worker-id").cloned().unwrap_or_default()
    } else {
        cfg.worker.clone()
    };
    let host = match Host::open(&cfg.journal_path) {
        Ok(h) => h,
        Err(e) => {
            return RunOutput::err(
                1,
                format!("JOURNAL cannot open {} | {e}\n", cfg.journal_path),
            )
        }
    };
    let input = CaptureInput {
        hexsalt: salt,
        key,
        hashed_pure: digest,
        memory_cost,
        attempts,
        hashes_per_second: hashrate,
        worker,
    };
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0);
    match host.capture(input, now_ms) {
        Ok(outcome) => match outcome {
            crate::CaptureOutcome::Journaled { id } => {
                RunOutput::ok(format!("captured id={id} journaled\n"))
            }
            crate::CaptureOutcome::Fallback => RunOutput {
                code: 0,
                stdout: "captured fallback\n".into(),
                stderr: format!(
                    "JOURNAL write failed; find captured in fallback sink | {}\n",
                    host.fallback.path().display()
                ),
            },
            crate::CaptureOutcome::Fatal { reason } => RunOutput::err(1, format!("{reason}\n")),
        },
        Err(e) => RunOutput::err(1, format!("{e}\n")),
    }
}

fn run_drain(flags: &BTreeMap<String, String>) -> RunOutput {
    let cfg = match load_resolved(flags) {
        Ok(c) => c,
        Err(e) => return RunOutput::err(1, format!("{e}\n")),
    };
    let steps: usize = flags
        .get("steps")
        .and_then(|s| s.parse().ok())
        .unwrap_or(1)
        .max(1);
    let host = match Host::open(&cfg.journal_path) {
        Ok(h) => h,
        Err(e) => {
            return RunOutput::err(
                1,
                format!("JOURNAL cannot open {} | {e}\n", cfg.journal_path),
            )
        }
    };
    let mut out = format_recovery(&host.abs_path, &host.import, &host.recovery);
    let transport = HttpTransport::new(&cfg.rpc_link, &cfg.worker);
    let mut mgr = host.make_manager(transport, cfg.margin);
    for i in 0..steps {
        let step = mgr.run_once();
        out.push_str(&format!("drain step {} {:?}\n", i + 1, step_name(step)));
        if mgr.is_fatal() {
            out.push_str("drain fatal\n");
            return RunOutput {
                code: 1,
                stdout: out,
                stderr: "submission drain halted\n".into(),
            };
        }
    }
    RunOutput::ok(out)
}

fn step_name(step: StepResult) -> &'static str {
    match step {
        StepResult::Idle => "Idle",
        StepResult::Probed => "Probed",
        StepResult::Submitted => "Submitted",
        StepResult::BreakerBlocked => "BreakerBlocked",
        StepResult::ConfirmRetried => "ConfirmRetried",
    }
}
