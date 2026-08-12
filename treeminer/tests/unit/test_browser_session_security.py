from pathlib import Path
import sys

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from server.auth import AuthService, DEFAULT_ADMIN_KEY, SESSION_COOKIE_NAME
from server.routers.account import router as account_router


class _Repo:
    pass


def test_production_rejects_documented_admin_key():
    with pytest.raises(ValueError, match="non-default admin key"):
        AuthService(_Repo(), admin_key=DEFAULT_ADMIN_KEY, production=True)


def test_production_accepts_operator_admin_key():
    AuthService(_Repo(), admin_key="operator-provided-secret", production=True)


def test_legacy_account_id_login_route_is_removed():
    paths = {(route.path, method) for route in account_router.routes for method in route.methods or ()}
    assert ("/api/auth/login", "POST") not in paths
    assert ("/api/auth/logout", "POST") in paths


def test_wallet_verification_sets_hardened_session_cookie(monkeypatch):
    class _Auth:
        def verify_signature(self, address, signature, nonce):
            return True

        def issue_jwt(self, address, role, account_id):
            return "signed.jwt.value"

    class _Accounts:
        async def get_or_create_by_eth_address(self, address):
            return {"role": "provider", "account_id": "provider-1"}

    server = type("Server", (), {"auth": _Auth(), "accounts": _Accounts()})()
    app = FastAPI()
    app.state.server = server
    app.include_router(account_router)
    address = "0x" + "1" * 40
    with TestClient(app, base_url="https://testserver") as client:
        response = client.post("/api/auth/verify", json={
            "address": address,
            "signature": "0xsig",
            "nonce": "nonce",
        })

    assert response.status_code == 200
    assert "token" not in response.json()
    cookie = response.headers["set-cookie"]
    assert f"{SESSION_COOKIE_NAME}=signed.jwt.value" in cookie
    assert "HttpOnly" in cookie
    assert "Secure" in cookie
    assert "SameSite=strict" in cookie


def test_logout_expires_session_cookie():
    app = FastAPI()
    app.include_router(account_router)
    with TestClient(app, base_url="https://testserver") as client:
        response = client.post("/api/auth/logout")
    assert response.status_code == 204
    cookie = response.headers["set-cookie"]
    assert f"{SESSION_COOKIE_NAME}=" in cookie
    assert "Max-Age=0" in cookie
    assert "HttpOnly" in cookie
    assert "Secure" in cookie
    assert "SameSite=strict" in cookie
