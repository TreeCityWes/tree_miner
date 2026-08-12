"""Admin router — /api/accounts, /api/settlements, /api/status, /api/workers/{id}/control, etc."""

from fastapi import APIRouter, Depends
from starlette.requests import Request

from server.deps import get_server, require_auth
from server.models import ControlRequest

router = APIRouter()


# The root banner and /api/status expose only aggregate counters and are
# deliberately public (the dashboard polls them anonymously).

@router.get("/")
async def root(request: Request):
    srv = get_server(request)
    return {
        "service": "XenMiner Mock Platform",
        "mqtt_port": srv.mqtt_port,
        "api_port": srv.api_port,
        "connected_workers": len(srv.broker.connected_client_ids),
        "uptime": "running",
    }


@router.get("/api/status")
async def server_status(request: Request):
    srv = get_server(request)
    return {
        "mqtt_clients": srv.broker.connected_client_ids,
        "workers": await srv.storage.workers.count(),
        "active_leases": await srv.storage.leases.count(state="active"),
        "total_blocks": await srv.storage.blocks.count(),
        "self_mined_blocks": await srv.storage.blocks.count_self_mined(),
        "total_settlements": await srv.storage.settlements.count(),
    }


@router.get("/api/accounts")
async def list_accounts(
    request: Request,
    caller: dict = Depends(require_auth("admin")),
):
    srv = get_server(request)
    accounts = await srv.accounts.list_accounts()
    sanitized = {}
    for k, v in accounts.items():
        sanitized[k] = {
            "account_id": v["account_id"],
            "role": v["role"],
            "eth_address": v["eth_address"],
            "balance": v["balance"],
        }
    return sanitized


@router.get("/api/settlements")
async def list_settlements(
    request: Request,
    limit: int = 50,
    offset: int = 0,
    caller: dict = Depends(require_auth("admin")),
):
    srv = get_server(request)
    items = await srv.settlement.list_settlements(limit=limit, offset=offset)
    total = await srv.storage.settlements.count()
    return {"items": items, "total": total, "limit": limit, "offset": offset}


# Worker control publishes MQTT commands (config changes, shutdown) to live
# miners — operator-only. Wallet owners have their own scoped command route
# at /api/wallet/workers/{id}/command.

@router.post("/api/workers/{worker_id}/control")
async def send_control(
    request: Request,
    worker_id: str,
    req: ControlRequest,
    caller: dict = Depends(require_auth("admin")),
):
    srv = get_server(request)
    payload = {"action": req.action, "config": req.config}
    await srv.broker.publish(f"xenminer/{worker_id}/control", payload)
    return {"status": "sent", "worker_id": worker_id, "action": req.action}


@router.post("/api/control/broadcast")
async def broadcast_control(
    request: Request,
    req: ControlRequest,
    caller: dict = Depends(require_auth("admin")),
):
    srv = get_server(request)
    workers = await srv.matcher.get_available_workers()
    sent_to = []
    for w in workers:
        wid = w["worker_id"]
        payload = {"action": req.action, "config": req.config}
        await srv.broker.publish(f"xenminer/{wid}/control", payload)
        sent_to.append(wid)
    return {"status": "sent", "workers": sent_to, "action": req.action}
