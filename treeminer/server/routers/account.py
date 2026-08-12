"""Account router — /api/auth/*, /api/accounts/{id}/* endpoints."""

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Response
from starlette.requests import Request

from server.auth import JWT_TTL, SESSION_COOKIE_NAME, SIGN_MESSAGE_TEMPLATE
from server.deps import get_server, require_auth
from server.models import RegisterRequest, DepositRequest, WithdrawRequest, WalletVerifyRequest

router = APIRouter()


@router.get("/api/auth/nonce")
async def auth_nonce(request: Request, address: str = Query(...)):
    srv = get_server(request)
    if not address.startswith("0x") or len(address) != 42:
        raise HTTPException(status_code=400, detail="Invalid Ethereum address")
    nonce = srv.auth.generate_nonce(address)
    return {
        "nonce": nonce,
        "message": SIGN_MESSAGE_TEMPLATE.format(nonce=nonce),
    }


@router.post("/api/auth/verify")
async def auth_verify(request: Request, response: Response, req: WalletVerifyRequest):
    srv = get_server(request)
    if not srv.auth.verify_signature(req.address, req.signature, req.nonce):
        raise HTTPException(status_code=401, detail="Invalid signature or expired nonce")
    acct = await srv.accounts.get_or_create_by_eth_address(req.address)
    if acct is None:
        raise HTTPException(status_code=500, detail="Failed to create account")
    token = srv.auth.issue_jwt(req.address, acct["role"], acct["account_id"])
    response.set_cookie(
        key=SESSION_COOKIE_NAME,
        value=token,
        max_age=JWT_TTL,
        httponly=True,
        secure=True,
        samesite="strict",
        path="/",
    )
    return {
        "address": req.address,
        "account_id": acct["account_id"],
        "role": acct["role"],
    }


@router.post("/api/auth/register")
async def auth_register(request: Request, req: RegisterRequest):
    srv = get_server(request)
    try:
        # Balance is intentionally NOT taken from the request: new accounts
        # always start at the server-side default (0). Funds arrive via the
        # deposit flow, never at registration time.
        acct = await srv.auth.register(
            account_id=req.account_id,
            role=req.role,
            eth_address=req.eth_address,
        )
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    except RuntimeError as e:
        raise HTTPException(status_code=500, detail=str(e))
    return {
        "account_id": acct["account_id"],
        "role": acct["role"],
        "eth_address": acct["eth_address"],
        "balance": acct["balance"],
        "api_key": acct["api_key"],
    }


@router.post("/api/auth/logout", status_code=204)
async def auth_logout(response: Response):
    response.delete_cookie(
        key=SESSION_COOKIE_NAME,
        httponly=True,
        secure=True,
        samesite="strict",
        path="/",
    )


@router.get("/api/auth/me")
async def auth_me(
    request: Request,
    x_api_key: str = Header(default=""),
    authorization: str = Header(default=""),
):
    srv = get_server(request)
    acct = await srv.auth.resolve_account(x_api_key, authorization)
    if acct is None:
        raise HTTPException(status_code=401, detail="Invalid or missing credentials")
    return {
        "account_id": acct["account_id"],
        "role": acct["role"],
        "eth_address": acct.get("eth_address", ""),
        "balance": acct.get("balance", 0.0),
    }


@router.get("/api/accounts/{account_id}/balance")
async def get_balance(
    request: Request,
    account_id: str,
    caller: dict = Depends(require_auth()),
):
    srv = get_server(request)
    if caller["role"] != "admin" and caller["account_id"] != account_id:
        raise HTTPException(status_code=403, detail="You can only view your own balance")
    acct = await srv.accounts.get_account(account_id)
    if acct is None:
        raise HTTPException(status_code=404, detail="Account not found")
    return {
        "account_id": acct["account_id"],
        "role": acct["role"],
        "balance": acct["balance"],
        "eth_address": acct["eth_address"],
    }


@router.post("/api/accounts/{account_id}/deposit")
async def deposit(
    request: Request,
    account_id: str,
    req: DepositRequest,
    caller: dict = Depends(require_auth()),
):
    srv = get_server(request)
    if caller["role"] != "admin" and caller["account_id"] != account_id:
        raise HTTPException(status_code=403, detail="You can only deposit to your own account")
    try:
        acct = await srv.accounts.deposit(account_id, req.amount)
    except KeyError:
        raise HTTPException(status_code=404, detail="Account not found")
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    return {
        "account_id": acct["account_id"],
        "balance": acct["balance"],
    }


@router.post("/api/accounts/{account_id}/withdraw")
async def withdraw(
    request: Request,
    account_id: str,
    req: WithdrawRequest,
    caller: dict = Depends(require_auth()),
):
    srv = get_server(request)
    if caller["role"] != "admin" and caller["account_id"] != account_id:
        raise HTTPException(status_code=403, detail="You can only withdraw from your own account")
    try:
        acct = await srv.accounts.withdraw(account_id, req.amount)
    except KeyError:
        raise HTTPException(status_code=404, detail="Account not found")
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    return {
        "account_id": acct["account_id"],
        "balance": acct["balance"],
        "withdrawn": req.amount,
    }
