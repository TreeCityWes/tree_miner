//! Transport boundary for the submission layer.
//! Port of `src/submit/ITransport.h`. HTTP (reqwest) comes later — not cpr.

use treeminer_protocol::FoundPayload;

/// Outcome of one transport round-trip. `transport_ok == false` means the request never
/// produced an HTTP response (connect error, timeout, DNS failure).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TransportResult {
    pub transport_ok: bool,
    pub http_status: i32,
    pub body: String,
    pub retry_after: Option<String>,
    pub date_header: Option<String>,
    pub error: String,
}

impl TransportResult {
    pub fn ok(status: i32, body: impl Into<String>) -> Self {
        Self {
            transport_ok: true,
            http_status: status,
            body: body.into(),
            ..Self::default()
        }
    }

    pub fn down(error: impl Into<String>) -> Self {
        Self {
            transport_ok: false,
            error: error.into(),
            ..Self::default()
        }
    }
}

/// Adapter behind which an X1 migration replaces HTTP, never the journal or the state machine.
/// Must never panic: every failure comes back as `transport_ok == false`.
pub trait Transport {
    fn submit(&mut self, payload: &FoundPayload) -> TransportResult;
    fn confirm(&mut self, key: &str) -> TransportResult;
    fn difficulty(&mut self) -> TransportResult;
}
