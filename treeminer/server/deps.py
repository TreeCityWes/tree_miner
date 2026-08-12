"""Dependency helpers for router modules."""

from typing import Optional

from fastapi import Header, HTTPException
from starlette.requests import Request


def get_server(request: Request):
    return request.app.state.server


def require_auth(role: Optional[str] = None):
    """Build a FastAPI dependency that authenticates the caller.

    Returns the resolved account dict. Raises 401 when credentials are
    missing or invalid, 403 when the account's role is insufficient, and
    503 when the auth service is not initialized yet.

    WHY: several routers used to resolve the caller only *if* credentials
    were supplied and then ran role checks guarded by `if caller ...` —
    which meant an anonymous request skipped authorization entirely.
    Centralizing the check here makes "no credentials" a hard failure on
    every route that depends on it, so that class of bug can't recur.

    An "admin" account satisfies every role requirement.
    """

    async def _dep(
        request: Request,
        x_api_key: str = Header(default=""),
        authorization: str = Header(default=""),
    ) -> dict:
        srv = get_server(request)
        # Fail closed: a missing auth service used to mean "treat caller as
        # anonymous and allow" — now it means nobody is authorized.
        if srv.auth is None:
            raise HTTPException(status_code=503, detail="Authentication service unavailable")
        acct = await srv.auth.resolve_account(x_api_key, authorization)
        if acct is None:
            raise HTTPException(
                status_code=401,
                detail="Missing or invalid credentials. Pass Authorization: Bearer <jwt> or X-API-Key header.",
            )
        if role is not None and acct["role"] not in (role, "admin"):
            raise HTTPException(status_code=403, detail=f"{role.capitalize()} account required")
        return acct

    return _dep
