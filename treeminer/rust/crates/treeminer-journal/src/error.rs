use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JournalError(pub String);

impl JournalError {
    pub fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for JournalError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for JournalError {}

impl From<rusqlite::Error> for JournalError {
    fn from(err: rusqlite::Error) -> Self {
        Self(format!("journal: {err}"))
    }
}
