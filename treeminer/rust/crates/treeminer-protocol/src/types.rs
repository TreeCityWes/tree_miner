//! Shared journal/submitter contract types.
//! Port of `src/treeminer/Types.{h,cpp}`. Terminal states: Acked, Dead, PermanentlyInvalid.

use std::fmt;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum FindKind {
    Xen11,
    Xuni,
}

impl FindKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Xen11 => "XEN11",
            Self::Xuni => "XUNI",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "XEN11" => Some(Self::Xen11),
            "XUNI" => Some(Self::Xuni),
            _ => None,
        }
    }
}

impl fmt::Display for FindKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Full lifecycle of a find. `Submitting` is in-process only; never persisted.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum FindStatus {
    Pending,
    Submitting,
    AcceptedUnconfirmed,
    Acked,
    ParkedDifficulty,
    ParkedXuniWindow,
    Quarantined,
    Dead,
    PermanentlyInvalid,
}

pub const FIND_STATUS_TERMINAL: &[FindStatus] = &[
    FindStatus::Acked,
    FindStatus::Dead,
    FindStatus::PermanentlyInvalid,
];

impl FindStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Pending => "Pending",
            Self::Submitting => "Submitting",
            Self::AcceptedUnconfirmed => "AcceptedUnconfirmed",
            Self::Acked => "Acked",
            Self::ParkedDifficulty => "ParkedDifficulty",
            Self::ParkedXuniWindow => "ParkedXuniWindow",
            Self::Quarantined => "Quarantined",
            Self::Dead => "Dead",
            Self::PermanentlyInvalid => "PermanentlyInvalid",
        }
    }

    pub fn is_terminal(self) -> bool {
        matches!(self, Self::Acked | Self::Dead | Self::PermanentlyInvalid)
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "Pending" => Some(Self::Pending),
            "Submitting" => Some(Self::Submitting),
            "AcceptedUnconfirmed" => Some(Self::AcceptedUnconfirmed),
            "Acked" => Some(Self::Acked),
            "ParkedDifficulty" => Some(Self::ParkedDifficulty),
            "ParkedXuniWindow" => Some(Self::ParkedXuniWindow),
            "Quarantined" => Some(Self::Quarantined),
            "Dead" => Some(Self::Dead),
            "PermanentlyInvalid" => Some(Self::PermanentlyInvalid),
            _ => None,
        }
    }
}

impl fmt::Display for FindStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Immutable capture of a find at discovery time. Never recompute `hash_to_verify`.
#[derive(Clone, Debug, PartialEq)]
pub struct FoundPayload {
    pub key: String,
    pub hash_to_verify: String,
    pub account: String,
    pub kind: FindKind,
    pub memory_cost: u32,
    pub worker: String,
    pub attempts: u64,
    pub hashes_per_second: f64,
    pub found_at_utc: String,
}

impl Default for FoundPayload {
    fn default() -> Self {
        Self {
            key: String::new(),
            hash_to_verify: String::new(),
            account: String::new(),
            kind: FindKind::Xen11,
            memory_cost: 0,
            worker: String::new(),
            attempts: 0,
            hashes_per_second: 0.0,
            found_at_utc: String::new(),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct FindRecord {
    pub id: i64,
    pub payload: FoundPayload,
    pub status: FindStatus,
    pub status_reason: String,
    pub attempt_count: i32,
    pub next_attempt_at: Option<String>,
    pub last_attempt_at: Option<String>,
    pub last_http_status: Option<i32>,
    pub last_response: String,
    pub confirmed_at: Option<String>,
    pub xuni_windows_tried: i32,
}

impl Default for FindRecord {
    fn default() -> Self {
        Self {
            id: -1,
            payload: FoundPayload::default(),
            status: FindStatus::Pending,
            status_reason: String::new(),
            attempt_count: 0,
            next_attempt_at: None,
            last_attempt_at: None,
            last_http_status: None,
            last_response: String::new(),
            confirmed_at: None,
            xuni_windows_tried: 0,
        }
    }
}

/// Outcome of classifying one `/verify` response. Pure data.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Classification {
    pub next_status: FindStatus,
    pub server_difficulty_hint: Option<u32>,
    pub needs_lookup_confirmation: bool,
    pub reason: String,
}

impl Classification {
    pub fn pending(reason: impl Into<String>) -> Self {
        Self {
            next_status: FindStatus::Pending,
            server_difficulty_hint: None,
            needs_lookup_confirmation: false,
            reason: reason.into(),
        }
    }

    pub fn quarantined(reason: impl Into<String>) -> Self {
        Self {
            next_status: FindStatus::Quarantined,
            server_difficulty_hint: None,
            needs_lookup_confirmation: false,
            reason: reason.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_and_kind_round_trip_cpp_strings() {
        for status in [
            FindStatus::Pending,
            FindStatus::Submitting,
            FindStatus::AcceptedUnconfirmed,
            FindStatus::Acked,
            FindStatus::ParkedDifficulty,
            FindStatus::ParkedXuniWindow,
            FindStatus::Quarantined,
            FindStatus::Dead,
            FindStatus::PermanentlyInvalid,
        ] {
            assert_eq!(FindStatus::parse(status.as_str()), Some(status));
        }
        assert_eq!(FindKind::parse("XEN11"), Some(FindKind::Xen11));
        assert_eq!(FindKind::parse("XUNI"), Some(FindKind::Xuni));
        assert_eq!(FindKind::Xen11.to_string(), "XEN11");
        assert!(FindStatus::Acked.is_terminal());
        assert!(!FindStatus::Pending.is_terminal());
    }
}
