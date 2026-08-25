//! HTTP transport over `std::net` (not cpr, not ureq). Field-for-field `/verify` JSON of
//! `HttpTransport.cpp`. Tests drive drain with a fake; this type is for the `drain` CLI.
//! Default RPC is `http://xenblocks.io` (HTTP). HTTPS is reported as a transport error.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::time::{Duration, Instant};

use treeminer_protocol::FoundPayload;
use treeminer_submit::{Transport, TransportResult};

const SUBMIT_TIMEOUT_MS: u64 = 10_000;
const GET_TIMEOUT_MS: u64 = 5_000;

pub struct HttpTransport {
    rpc: String,
    worker: String,
    submit_timeout: Duration,
    get_timeout: Duration,
}

impl HttpTransport {
    pub fn new(rpc_link: impl Into<String>, worker: impl Into<String>) -> Self {
        let mut rpc = rpc_link.into();
        while rpc.ends_with('/') {
            rpc.pop();
        }
        Self {
            rpc,
            worker: worker.into(),
            submit_timeout: Duration::from_millis(SUBMIT_TIMEOUT_MS),
            get_timeout: Duration::from_millis(GET_TIMEOUT_MS),
        }
    }

    pub fn rpc(&self) -> &str {
        &self.rpc
    }

    fn request(
        &self,
        method: &str,
        rel: &str,
        body: Option<&str>,
        timeout: Duration,
    ) -> TransportResult {
        let target = match parse_http_target(&self.rpc) {
            Ok(t) => t,
            Err(err) => return TransportResult::down(err),
        };
        let path = join_path(&target.path_prefix, rel);
        http_exchange(&target, method, &path, body, timeout)
    }
}

/// Exact `/verify` body: `attempts` and `hashes_per_second` are JSON strings (2 dp).
pub fn verify_json(payload: &FoundPayload, worker: &str) -> String {
    let worker = if worker.is_empty() {
        payload.worker.as_str()
    } else {
        worker
    };
    let hps = if payload.hashes_per_second.is_finite() {
        format!("{:.2}", payload.hashes_per_second)
    } else {
        "0.00".into()
    };
    format!(
        "{{\"hash_to_verify\":{},\"key\":{},\"account\":{},\"attempts\":{},\"hashes_per_second\":{},\"worker\":{}}}",
        json_string(&payload.hash_to_verify),
        json_string(&payload.key),
        json_string(&payload.account),
        json_string(&payload.attempts.to_string()),
        json_string(&hps),
        json_string(worker),
    )
}

fn json_string(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c.is_control() => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

#[derive(Debug)]
struct Target {
    host: String,
    port: u16,
    path_prefix: String,
}

fn parse_http_target(rpc: &str) -> Result<Target, String> {
    let rest = if let Some(r) = rpc.strip_prefix("http://") {
        r
    } else if rpc.starts_with("https://") {
        return Err(
            "https RPC is not supported in this host (C++ default is http://xenblocks.io)".into(),
        );
    } else {
        rpc
    };
    let (authority, path_prefix) = match rest.find('/') {
        Some(i) => (&rest[..i], rest[i..].trim_end_matches('/').to_string()),
        None => (rest, String::new()),
    };
    if authority.is_empty() {
        return Err("RPC URL missing host".into());
    }
    let (host, port) = if let Some(i) = authority.rfind(':') {
        let host = &authority[..i];
        let port: u16 = authority[i + 1..]
            .parse()
            .map_err(|_| format!("invalid RPC port in {authority}"))?;
        if host.is_empty() {
            return Err("RPC URL missing host".into());
        }
        (host.to_string(), port)
    } else {
        (authority.to_string(), 80)
    };
    Ok(Target {
        host,
        port,
        path_prefix,
    })
}

fn join_path(prefix: &str, rel: &str) -> String {
    if prefix.is_empty() {
        format!("/{rel}")
    } else {
        format!("{prefix}/{rel}")
    }
}

fn http_exchange(
    target: &Target,
    method: &str,
    path: &str,
    body: Option<&str>,
    timeout: Duration,
) -> TransportResult {
    let addr = match resolve_addr(&target.host, target.port) {
        Ok(a) => a,
        Err(err) => return TransportResult::down(err),
    };
    let mut stream = match TcpStream::connect_timeout(&addr, timeout) {
        Ok(s) => s,
        Err(err) => return TransportResult::down(format!("connect: {err}")),
    };
    if let Err(err) = stream.set_read_timeout(Some(timeout)) {
        return TransportResult::down(format!("set_read_timeout: {err}"));
    }
    if let Err(err) = stream.set_write_timeout(Some(timeout)) {
        return TransportResult::down(format!("set_write_timeout: {err}"));
    }
    let host_header = if target.port == 80 {
        target.host.clone()
    } else {
        format!("{}:{}", target.host, target.port)
    };
    let mut req =
        format!("{method} {path} HTTP/1.1\r\nHost: {host_header}\r\nConnection: close\r\n");
    if let Some(body) = body {
        req.push_str("Content-Type: application/json\r\n");
        req.push_str(&format!("Content-Length: {}\r\n\r\n", body.len()));
        req.push_str(body);
    } else {
        req.push_str("\r\n");
    }
    if let Err(err) = stream.write_all(req.as_bytes()) {
        return TransportResult::down(format!("write: {err}"));
    }
    let _ = stream.flush();
    read_http_response(&mut stream, timeout)
}

fn resolve_addr(host: &str, port: u16) -> Result<SocketAddr, String> {
    (host, port)
        .to_socket_addrs()
        .map_err(|e| format!("dns: {e}"))?
        .next()
        .ok_or_else(|| format!("dns: no addresses for {host}"))
}

fn read_http_response(stream: &mut TcpStream, timeout: Duration) -> TransportResult {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    let mut tmp = [0u8; 2048];
    loop {
        if Instant::now() > deadline {
            return TransportResult::down("timeout reading HTTP response");
        }
        match stream.read(&mut tmp) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&tmp[..n]);
                if let Some(header_end) = find_header_end(&buf) {
                    if let Some(len) = content_length(&buf[..header_end]) {
                        if buf.len() >= header_end + len {
                            break;
                        }
                    }
                }
            }
            Err(err)
                if err.kind() == std::io::ErrorKind::WouldBlock
                    || err.kind() == std::io::ErrorKind::TimedOut =>
            {
                return TransportResult::down("timeout reading HTTP response");
            }
            Err(err) => return TransportResult::down(format!("read: {err}")),
        }
    }
    parse_response(&buf)
}

fn find_header_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n").map(|i| i + 4)
}

fn content_length(headers: &[u8]) -> Option<usize> {
    let text = std::str::from_utf8(headers).ok()?;
    for line in text.split("\r\n") {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        if name.eq_ignore_ascii_case("content-length") {
            return value.trim().parse().ok();
        }
    }
    None
}

fn parse_response(raw: &[u8]) -> TransportResult {
    let Some(header_end) = find_header_end(raw) else {
        return TransportResult::down("malformed HTTP response (no header terminator)");
    };
    let headers = match std::str::from_utf8(&raw[..header_end]) {
        Ok(s) => s,
        Err(_) => return TransportResult::down("HTTP headers are not UTF-8"),
    };
    let mut lines = headers.split("\r\n");
    let status_line = lines.next().unwrap_or("");
    let http_status = parse_status(status_line).unwrap_or(0);
    if http_status <= 0 {
        return TransportResult::down("malformed HTTP status line");
    }
    let mut retry_after = None;
    let mut date_header = None;
    for line in lines {
        if line.is_empty() {
            continue;
        }
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        let value = value.trim().to_string();
        if name.eq_ignore_ascii_case("retry-after") {
            retry_after = Some(value);
        } else if name.eq_ignore_ascii_case("date") {
            date_header = Some(value);
        }
    }
    let body = String::from_utf8_lossy(&raw[header_end..]).into_owned();
    TransportResult {
        transport_ok: true,
        http_status,
        body,
        retry_after,
        date_header,
        error: String::new(),
    }
}

fn parse_status(line: &str) -> Option<i32> {
    let mut parts = line.split_whitespace();
    let _http = parts.next()?;
    parts.next()?.parse().ok()
}

impl Transport for HttpTransport {
    fn submit(&mut self, payload: &FoundPayload) -> TransportResult {
        let body = verify_json(payload, &self.worker);
        self.request("POST", "verify", Some(&body), self.submit_timeout)
    }

    fn confirm(&mut self, key: &str) -> TransportResult {
        self.request(
            "GET",
            &format!("get_block?key={key}"),
            None,
            self.get_timeout,
        )
    }

    fn difficulty(&mut self) -> TransportResult {
        self.request("GET", "difficulty", None, self.get_timeout)
    }
}

#[cfg(test)]
mod parse_tests {
    use super::*;

    #[test]
    fn parse_default_rpc() {
        let t = parse_http_target("http://xenblocks.io").unwrap();
        assert_eq!(t.host, "xenblocks.io");
        assert_eq!(t.port, 80);
        assert!(t.path_prefix.is_empty());
    }

    #[test]
    fn parse_port_and_prefix() {
        let t = parse_http_target("http://127.0.0.1:8080/api").unwrap();
        assert_eq!(t.host, "127.0.0.1");
        assert_eq!(t.port, 8080);
        assert_eq!(t.path_prefix, "/api");
        assert_eq!(join_path(&t.path_prefix, "verify"), "/api/verify");
    }

    #[test]
    fn https_is_transport_error() {
        let err = parse_http_target("https://xenblocks.io").unwrap_err();
        assert!(err.contains("https"));
    }
}
