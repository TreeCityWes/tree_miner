#!/usr/bin/env python3
"""Standalone mock of the XenBlocks verification server.

Replicates the reference server semantics (repos/xenminer/gpage.py, /verify handler at
lines 366-520) EXACTLY — same response strings, same status codes, same check order —
plus fault-injection controls for chaos tests. Python 3 stdlib only.

This is intentionally separate from treeminer/server/ and scripts/run_mock_server.sh,
which model Woody's own marketplace server, not the reference gpage.py protocol.

Usage:
    python mock_server.py [--port 8545] [--difficulty 100000]

Endpoints (protocol, gpage.py-faithful):
    POST /verify                submit a find
    GET  /get_block?key=<key>   confirmation lookup (blocks, then xuni table)
    GET  /difficulty            current difficulty, {"difficulty": "<N>"}
    GET  /difficulty/<account>  same (gpage.py:110)

Control endpoints (test-only, never fault-injected):
    GET  /control               current state + stored counts
    POST /control               set state; see README.md for the full matrix

Known deliberate deviations from gpage.py (documented in README.md):
    - argon2.verify is not run (stdlib-only): submissions are treated as verified unless
      the `verify-fail` fault is armed.
    - The EIP-55 checksum step of the salt check is skipped (needs keccak): any salt
      that base64-decodes to 40 hex chars passes, as does the legacy WEVO... salt.
    - gpage.py truncates hash_to_verify > 150 chars to None *before* its explicit
      length check, so oversize hashes actually produce the "Missing ..." 400, and the
      401 length message at gpage.py:444-448 is unreachable. We replicate the reachable
      behavior.
"""

import argparse
import base64
import json
import re
import socket
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ----------------------------------------------------------------------------- state

LOCK = threading.Lock()

FAULT_MODES = ("normal", "down", "timeout", "empty-body", "500", "insert-fail", "verify-fail")

STATE = {
    "mode": "normal",          # one of FAULT_MODES
    "difficulty": 100000,
    "xuni_window": "auto",     # auto | open | closed
    "xuni_race": False,        # force the legacy gpage.py:497 branch
    "timeout_seconds": 30.0,   # sleep used by the "timeout" fault
    "blocks": {},              # key -> row  (XEN11 table)
    "xuni": {},                # key -> row  (XUNI table)
    "next_block_id": 1,
    "verify_requests": 0,
}


def reset_state(keep_difficulty=False):
    with LOCK:
        difficulty = STATE["difficulty"]
        STATE.update(
            mode="normal",
            xuni_window="auto",
            xuni_race=False,
            timeout_seconds=30.0,
            blocks={},
            xuni={},
            next_block_id=1,
            verify_requests=0,
        )
        if keep_difficulty:
            STATE["difficulty"] = difficulty
        else:
            STATE["difficulty"] = 100000


# --------------------------------------------------------------- gpage.py replications

def is_within_five_minutes_of_hour():
    """gpage.py:36-40 — server-clock gate for XUNI."""
    with LOCK:
        override = STATE["xuni_window"]
    if override == "open":
        return True
    if override == "closed":
        return False
    minutes = datetime.now().minute
    return 0 <= minutes < 5 or 55 <= minutes < 60


def is_hexadecimal(s):
    """gpage.py:256-258."""
    return re.match(r"^[a-fA-F0-9]*$", s) is not None


def is_valid_hash(h):
    """gpage.py:265-267."""
    return bool(re.match("^[a-fA-F0-9]{64}$", h))


def check_salt_format_and_ethereum_address(hash_to_verify):
    """gpage.py:281-328, minus the EIP-55 keccak checksum (see module docstring)."""
    parts = hash_to_verify.split("$")
    if len(parts) != 6:
        return False
    salt = parts[4]
    if re.search(r"WEVOMTAwODIwMjJYRU4", salt):
        return True  # legacy salt
    if re.fullmatch(r"^[A-Za-z0-9+/]{27}$", salt):
        try:
            padded = salt + "=" * (-len(salt) % 4)
            decoded = base64.b64decode(padded).hex()
            if re.fullmatch(r"[0-9a-fA-F]{40}", decoded):
                return True
        except Exception:
            return False
    return False


# ----------------------------------------------------------------------------- server

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockXenBlocks/1.0"

    # --- plumbing ---

    def log_message(self, fmt, *args):  # quieter default log line
        print("[mock] %s - %s" % (self.address_string(), fmt % args))

    def send_json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b""
            return json.loads(raw.decode("utf-8")) if raw else None
        except Exception:
            return None

    def apply_fault(self):
        """Returns True when a fault consumed the request. /control is exempt."""
        with LOCK:
            mode = STATE["mode"]
            timeout_s = STATE["timeout_seconds"]
        if mode == "down":
            # Abruptly drop the connection: the client sees a reset/EOF, no HTTP response.
            # shutdown() is required — rfile/wfile hold dup'd handles (notably on Windows),
            # so close() alone would leave the OS socket open and the client hanging.
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.connection.close()
            except OSError:
                pass
            return True
        if mode == "timeout":
            # Stall past any sane client timeout, then answer (client has given up).
            time.sleep(timeout_s)
            self.send_json(200, {"message": "too late"})
            return True
        if mode == "empty-body":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if mode == "500":
            self.send_json(500, {"message": "Internal Server Error"})
            return True
        return False  # normal, insert-fail and verify-fail reach the handler

    # --- routing ---

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/control":
            return self.handle_control_get()
        if self.apply_fault():
            return
        if parsed.path == "/difficulty" or parsed.path.startswith("/difficulty/"):
            return self.handle_difficulty()
        if parsed.path == "/get_block":
            return self.handle_get_block(parse_qs(parsed.query))
        self.send_json(404, {"error": "Not found"})

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/control":
            return self.handle_control_post()
        if self.apply_fault():
            return
        if parsed.path == "/verify":
            return self.handle_verify()
        self.send_json(404, {"error": "Not found"})

    # --- protocol endpoints ---

    def handle_difficulty(self):
        # gpage.py:109-117 — difficulty is serialized as a STRING.
        with LOCK:
            difficulty = STATE["difficulty"]
        self.send_json(200, {"difficulty": str(difficulty)})

    def handle_get_block(self, query):
        # gpage.py:331-364.
        key = (query.get("key") or [None])[0]
        if not key:
            return self.send_json(400, {"error": "Please provide a key"})
        if not is_valid_hash(key):
            return self.send_json(400, {"error": "Invalid key provided"})
        with LOCK:
            row = STATE["blocks"].get(key) or STATE["xuni"].get(key)
        if row is None:
            return self.send_json(404, {"error": "Data not found for provided key"})
        self.send_json(200, row)

    def handle_verify(self):
        # gpage.py:366-519, same check order, same strings, same codes.
        with LOCK:
            STATE["verify_requests"] += 1
            difficulty = STATE["difficulty"]
            mode = STATE["mode"]
            xuni_race = STATE["xuni_race"]

        data = self.read_json_body()
        if data is None:
            # Flask's request.json on a bad body -> 400/500 depending on version; the
            # reference deployment answers an HTML 400. A JSON 400 is close enough and
            # classifies identically (unknown 400 -> quarantine).
            return self.send_json(400, {"error": "Bad Request"})

        hash_to_verify = data.get("hash_to_verify")
        hash_to_verify = hash_to_verify if (hash_to_verify and len(hash_to_verify) <= 150) else None
        key = data.get("key")
        key = key if (key and len(key) <= 128) else None
        account = data.get("account")
        if account is not None:
            account = str(account).lower().replace("'", "").replace('"', "")
            account = account if len(account) <= 43 else None

        # gpage.py:390 calls is_hexadecimal(key) with key possibly None -> TypeError ->
        # Flask 500. Replicated.
        if key is None:
            return self.send_json(500, {"message": "Internal Server Error"})
        if not is_hexadecimal(key):
            return self.send_json(400, {"error": "Invalid key format"})

        # gpage.py:394 runs the salt check before the missing-fields check; None hash
        # would TypeError -> 500. Replicated.
        if hash_to_verify is None:
            return self.send_json(500, {"message": "Internal Server Error"})
        if not check_salt_format_and_ethereum_address(hash_to_verify):
            return self.send_json(400, {"error": "Invalid salt format"})

        if not hash_to_verify or not key or not account:
            return self.send_json(400, {"error": "Missing hash_to_verify, key, or account"})

        # gpage.py:404,412-418 — strictly-< difficulty check on the embedded m=.
        m_match = re.search(r"m=(\d+)", hash_to_verify)
        if m_match is None:
            return self.send_json(500, {"message": "Internal Server Error"})
        submitted_difficulty = int(m_match.group(1))
        if submitted_difficulty < int(difficulty):
            error_message = (
                f"Hash does not contain 'm={difficulty}'. Your memory_cost setting in "
                f"your miner will be autoadjusted."
            )
            return self.send_json(401, {"message": error_message})

        # gpage.py:421-442 — target scan over the last 87 chars.
        tail = hash_to_verify[-87:]
        found = "XEN11" in tail
        is_xuni_present = re.search("XUNI[0-9]", tail) is not None
        if is_xuni_present:
            found = True
            window_open_first_check = True if xuni_race else is_within_five_minutes_of_hour()
            if not window_open_first_check:
                return self.send_json(401, {"message": "XUNI Submitted outside of proper time frame."})
        if not found:
            error_message = (
                "Hash does not contain any of the valid targets ['XEN11'] in the last 87 "
                "characters. Adjust target_substr in your miner."
            )
            return self.send_json(401, {"message": error_message})

        # gpage.py:444-448 length check: unreachable (see module docstring), kept for parity.
        if len(hash_to_verify) > 150:
            return self.send_json(
                401,
                {"message": "Length of hash_to_verify should not be greater than 150 characters."},
            )

        # gpage.py:450-457 — argon2.verify; stdlib mock treats it as passed unless the
        # verify-fail fault is armed.
        is_verified = mode != "verify-fail"
        if not is_verified:
            return self.send_json(401, {"message": "Hash verification failed."})

        is_xen11_present = "XEN11" in tail
        window_open_second_check = False if xuni_race else is_within_five_minutes_of_hour()

        # gpage.py:467-515 — insert, duplicate handling, and the unconfirmed-200.
        with LOCK:
            if is_xuni_present and window_open_second_check:
                table = STATE["xuni"]
            elif is_xen11_present:
                table = STATE["blocks"]
            else:
                # gpage.py:497 — the legacy else-branch, reachable only for XUNI-bearing
                # hashes that lost the window between the two checks (xuni_race mode).
                return self.send_json(401, {"message": "XUNI found outside of time window"})

            if key in table:
                # sqlite3.IntegrityError on the UNIQUE key -> gpage.py:510.
                return self.send_json(400, {"message": "Block already exists, continue"})

            if mode == "insert-fail":
                # gpage.py:492-494 + 515 — insert retries exhausted, "success" returned
                # anyway. THE LYING 200: nothing is stored, /get_block will 404.
                pass
            else:
                table[key] = {
                    "block_id": STATE["next_block_id"],
                    "hash_to_verify": hash_to_verify,
                    "key": key,
                    "account": account,
                    "created_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                }
                STATE["next_block_id"] += 1

        return self.send_json(200, {"message": "Hash verified successfully and block saved."})

    # --- control endpoints ---

    def handle_control_get(self):
        with LOCK:
            snapshot = {
                "mode": STATE["mode"],
                "difficulty": STATE["difficulty"],
                "xuni_window": STATE["xuni_window"],
                "xuni_race": STATE["xuni_race"],
                "timeout_seconds": STATE["timeout_seconds"],
                "stored_blocks": len(STATE["blocks"]),
                "stored_xuni": len(STATE["xuni"]),
                "verify_requests": STATE["verify_requests"],
            }
        self.send_json(200, snapshot)

    def handle_control_post(self):
        body = self.read_json_body()
        if body is None or not isinstance(body, dict):
            return self.send_json(400, {"error": "control body must be a JSON object"})
        if body.get("reset"):
            reset_state(keep_difficulty=bool(body.get("keep_difficulty")))
        errors = []
        with LOCK:
            if "mode" in body:
                if body["mode"] in FAULT_MODES:
                    STATE["mode"] = body["mode"]
                else:
                    errors.append("unknown mode %r (valid: %s)" % (body["mode"], ", ".join(FAULT_MODES)))
            if "difficulty" in body:
                try:
                    STATE["difficulty"] = int(body["difficulty"])
                except (TypeError, ValueError):
                    errors.append("difficulty must be an integer")
            if "xuni_window" in body:
                if body["xuni_window"] in ("auto", "open", "closed"):
                    STATE["xuni_window"] = body["xuni_window"]
                else:
                    errors.append("xuni_window must be auto|open|closed")
            if "xuni_race" in body:
                STATE["xuni_race"] = bool(body["xuni_race"])
            if "timeout_seconds" in body:
                try:
                    STATE["timeout_seconds"] = float(body["timeout_seconds"])
                except (TypeError, ValueError):
                    errors.append("timeout_seconds must be a number")
        if errors:
            return self.send_json(400, {"error": "; ".join(errors)})
        return self.handle_control_get()


def main():
    parser = argparse.ArgumentParser(description="Mock XenBlocks server (gpage.py semantics)")
    parser.add_argument("--port", type=int, default=8545)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--difficulty", type=int, default=100000)
    args = parser.parse_args()

    STATE["difficulty"] = args.difficulty
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[mock] XenBlocks mock server on http://{args.host}:{args.port} "
          f"(difficulty={args.difficulty})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
