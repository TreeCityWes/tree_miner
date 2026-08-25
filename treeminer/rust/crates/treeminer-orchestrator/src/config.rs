//! config.txt `key=value` (trimmed) plus CLI overrides. No Boost.ProgramOptions.
//!
//! Precedence matches `main.cpp`: file supplies defaults, flags win. An unparseable
//! margin value is fatal rather than ignored.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

use treeminer_protocol::{parse_margin_mode, MarginConfig};

pub const DEFAULT_JOURNAL_PATH: &str = "treeminer-journal.db";
pub const DEFAULT_RPC_LINK: &str = "http://xenblocks.io";
pub const DEFAULT_CONFIG_FILE: &str = "config.txt";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ConfigError {
    InvalidMarginMode(String),
    NotANumber { key: String, value: String },
    OutOfRange { key: String, value: String },
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::InvalidMarginMode(mode) => write!(
                f,
                "Invalid difficulty margin mode '{mode}' (expected: off | fixed | auto)."
            ),
            Self::NotANumber { key, value } => {
                write!(f, "Value for {key} ({value}) is not a number.")
            }
            Self::OutOfRange { key, value } => {
                write!(
                    f,
                    "Value for {key} ({value}) is out of range (0-100000000)."
                )
            }
        }
    }
}

impl std::error::Error for ConfigError {}

#[derive(Clone, Debug, PartialEq)]
pub struct HostConfig {
    pub journal_path: String,
    pub rpc_link: String,
    pub miner_addr: String,
    pub worker: String,
    pub margin: MarginConfig,
    pub config_path: String,
}

impl Default for HostConfig {
    fn default() -> Self {
        Self {
            journal_path: DEFAULT_JOURNAL_PATH.into(),
            rpc_link: DEFAULT_RPC_LINK.into(),
            miner_addr: String::new(),
            worker: String::new(),
            margin: MarginConfig::default(),
            config_path: DEFAULT_CONFIG_FILE.into(),
        }
    }
}

pub fn trim_ws(s: &str) -> &str {
    s.trim_matches(|c| c == ' ' || c == '\t' || c == '\r' || c == '\n')
}

/// C++ `ConfigManager::loadConfig`: first `=` splits, both sides trimmed. Missing file is empty.
pub fn load_config_txt(path: impl AsRef<Path>) -> BTreeMap<String, String> {
    let mut map = BTreeMap::new();
    let Ok(file) = File::open(path) else {
        return map;
    };
    for line in BufReader::new(file).lines().map_while(Result::ok) {
        let Some(eq) = line.find('=') else {
            continue;
        };
        let key = trim_ws(&line[..eq]).to_string();
        let value = trim_ws(&line[eq + 1..]).to_string();
        if !key.is_empty() {
            map.insert(key, value);
        }
    }
    map
}

/// CLI flags keyed by C++ Boost names (`journalPath`, `rpcLink`, …).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CliOverrides {
    pub journal_path: Option<String>,
    pub rpc_link: Option<String>,
    pub miner_addr: Option<String>,
    pub worker: Option<String>,
    pub margin_mode: Option<String>,
    pub margin_kib: Option<String>,
    pub margin_max: Option<String>,
    pub config_path: Option<String>,
}

impl CliOverrides {
    pub fn from_flags(flags: &BTreeMap<String, String>) -> Self {
        Self {
            journal_path: flags.get("journalPath").cloned(),
            rpc_link: flags.get("rpcLink").cloned(),
            miner_addr: flags.get("minerAddr").cloned(),
            worker: flags
                .get("worker-id")
                .cloned()
                .or_else(|| flags.get("workerId").cloned()),
            margin_mode: flags.get("difficultyMarginMode").cloned(),
            margin_kib: flags.get("difficultyMargin").cloned(),
            margin_max: flags.get("difficultyMarginMax").cloned(),
            config_path: flags.get("config").cloned(),
        }
    }
}

pub fn resolve_config(
    file: &BTreeMap<String, String>,
    cli: &CliOverrides,
) -> Result<HostConfig, ConfigError> {
    let mut cfg = HostConfig::default();
    if let Some(path) = &cli.config_path {
        cfg.config_path = path.clone();
    }

    pick(
        &mut cfg.journal_path,
        file.get("journal_path"),
        cli.journal_path.as_deref(),
    );
    pick(
        &mut cfg.rpc_link,
        file.get("rpc_link"),
        cli.rpc_link.as_deref(),
    );
    pick(
        &mut cfg.miner_addr,
        file.get("account_address"),
        cli.miner_addr.as_deref(),
    );
    pick(
        &mut cfg.worker,
        file.get("worker_id"),
        cli.worker.as_deref(),
    );

    let mut mode_text = file
        .get("difficulty_margin_mode")
        .cloned()
        .unwrap_or_default();
    if let Some(cli_mode) = &cli.margin_mode {
        mode_text = cli_mode.clone();
    }
    if !mode_text.is_empty() {
        match parse_margin_mode(&mode_text) {
            Some(mode) => cfg.margin.mode = mode,
            None => return Err(ConfigError::InvalidMarginMode(mode_text)),
        }
    }

    read_positive_int(
        file.get("difficulty_margin"),
        cli.margin_kib.as_deref(),
        "difficulty_margin",
        &mut cfg.margin.margin_kib,
    )?;
    read_positive_int(
        file.get("difficulty_margin_max"),
        cli.margin_max.as_deref(),
        "difficulty_margin_max",
        &mut cfg.margin.max_kib,
    )?;

    // Empty override keeps the compatibility default (C++ `main.cpp` journal path).
    if cfg.journal_path.is_empty() {
        cfg.journal_path = DEFAULT_JOURNAL_PATH.into();
    }
    if cfg.rpc_link.is_empty() {
        cfg.rpc_link = DEFAULT_RPC_LINK.into();
    }
    Ok(cfg)
}

fn pick(target: &mut String, file: Option<&String>, cli: Option<&str>) {
    if let Some(v) = file {
        if !v.is_empty() {
            *target = v.clone();
        }
    }
    if let Some(v) = cli {
        if !v.is_empty() {
            *target = v.to_string();
        }
    }
}

fn read_positive_int(
    file: Option<&String>,
    cli: Option<&str>,
    key: &str,
    target: &mut u32,
) -> Result<(), ConfigError> {
    let mut text = file.cloned().unwrap_or_default();
    if let Some(cli) = cli {
        text = cli.to_string();
    }
    if text.is_empty() {
        return Ok(());
    }
    let value: i64 = text.parse().map_err(|_| ConfigError::NotANumber {
        key: key.into(),
        value: text.clone(),
    })?;
    if !(0..=100_000_000).contains(&value) {
        return Err(ConfigError::OutOfRange {
            key: key.into(),
            value: text,
        });
    }
    *target = value as u32;
    Ok(())
}
