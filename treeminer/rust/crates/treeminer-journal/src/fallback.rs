//! Last-resort append-only JSONL sink for finds SQLite refused.
//! Port of `src/journal/FallbackSink.{h,cpp}`. Same disk as the journal — covers SQLite
//! lock/corruption/quota, not total-disk death. `append` never panics; returns false on
//! any I/O failure.

use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};

use treeminer_protocol::FoundPayload;

use crate::jsonl;
use crate::FindJournalApi;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ImportStats {
    pub imported: usize,
    pub malformed: usize,
    pub file_present: bool,
}

pub struct FallbackSink {
    path: PathBuf,
}

impl FallbackSink {
    /// Records the path only. Does not create, open, or stat anything.
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// One JSONL line, fsync'd before return. False on any failure; never panics.
    pub fn append(&self, payload: &FoundPayload) -> bool {
        append_line(&self.path, &jsonl::serialize(payload)).is_ok()
    }

    /// Drain `path` into `journal`. Missing file is a silent no-op. Malformed lines are
    /// skipped. A journal error stops the pass and leaves the file for the next boot.
    /// A clean pass renames to `<path>.imported`.
    pub fn import_into(journal: &impl FindJournalApi, path: impl AsRef<Path>) -> ImportStats {
        import_into_impl(journal, path.as_ref())
    }
}

fn append_line(path: &Path, line: &str) -> std::io::Result<()> {
    let mut created = true;
    let mut opts = OpenOptions::new();
    opts.write(true).append(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    let mut file = match opts.open(path) {
        Ok(f) => f,
        Err(err) if err.kind() == std::io::ErrorKind::AlreadyExists => {
            created = false;
            let mut opts = OpenOptions::new();
            opts.write(true).append(true).create(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                opts.mode(0o600);
            }
            opts.open(path)?
        }
        Err(err) => return Err(err),
    };
    file.write_all(line.as_bytes())?;
    file.sync_all()?;
    #[cfg(unix)]
    if created {
        sync_parent_dir(path);
    }
    #[cfg(not(unix))]
    let _ = created;
    Ok(())
}

#[cfg(unix)]
fn sync_parent_dir(file_path: &Path) {
    let parent = file_path.parent().unwrap_or_else(|| Path::new("."));
    if let Ok(dir) = File::open(parent) {
        let _ = dir.sync_all();
    }
}

fn import_into_impl(journal: &impl FindJournalApi, path: &Path) -> ImportStats {
    let mut stats = ImportStats::default();
    if !path.exists() {
        return stats;
    }
    let file = match File::open(path) {
        Ok(f) => f,
        Err(_) => {
            stats.file_present = true;
            return stats;
        }
    };
    stats.file_present = true;
    let reader = BufReader::new(file);
    let mut clean_pass = true;
    for line_res in reader.lines() {
        let mut line = match line_res {
            Ok(l) => l,
            Err(_) => {
                clean_pass = false;
                break;
            }
        };
        while line.ends_with('\r') || line.ends_with('\n') {
            line.pop();
        }
        if line.trim_start_matches([' ', '\t']).is_empty() {
            continue;
        }
        let Some(payload) = jsonl::parse_payload(&line) else {
            stats.malformed += 1;
            continue;
        };
        match journal.append(&payload) {
            Ok(_) => stats.imported += 1,
            Err(_) => {
                clean_pass = false;
                break;
            }
        }
    }
    if !clean_pass {
        return stats;
    }
    let archived = {
        let mut p = path.as_os_str().to_os_string();
        p.push(".imported");
        PathBuf::from(p)
    };
    let _ = fs::remove_file(&archived);
    let _ = fs::rename(path, &archived);
    stats
}
