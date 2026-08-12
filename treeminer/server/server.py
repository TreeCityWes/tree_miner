"""
server.py - Mock platform server entry point.

Single-process server combining:
 - Embedded MQTT broker (pure-Python async, port 1883)
 - SQLite persistent storage via StorageManager
 - Platform services (account, matcher, watcher, settlement)
 - REST control API (FastAPI on uvicorn, port 8080)

Usage:
    python -m server.server [--mqtt-port 1883] [--api-port 8080] [--db-path data/marketplace.db]
    python server/server.py [--mqtt-port 1883] [--api-port 8080] [--db-path data/marketplace.db]
"""

import argparse
import asyncio
import logging
import os
import re
import sys
from pathlib import Path
from typing import Optional

# Ensure project root is on sys.path so imports work both ways
_project_root = str(Path(__file__).resolve().parent.parent)
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

try:
    from fastapi import FastAPI
    import uvicorn
except ImportError:
    print("ERROR: FastAPI and uvicorn are required. Install with:")
    print("  pip install fastapi uvicorn pydantic")
    sys.exit(1)

from server.account import AccountService
from server.auth import (
    AuthService, DEFAULT_ADMIN_KEY, SESSION_COOKIE_NAME,
    ensure_api_keys_for_defaults,
)
from server.broker import MQTTBroker
from server.chain_simulator import ChainSimulator
from server.dashboard import register_dashboard
from server.matcher import MatchingEngine
from server.monitoring import MonitoringService
from server.pricing import PricingEngine
from server.reputation import ReputationEngine
from server.routers import register_all_routers
from server.settlement import SettlementEngine
from server.storage import StorageManager
from server.wallet_snapshot import WalletSnapshotService
from server.watcher import BlockWatcher
from server.ws import WSManager

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)-10s] %(levelname)-5s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("server")

# ---------------------------------------------------------------------------
# MQTT topic parser
# ---------------------------------------------------------------------------

_TOPIC_RE = re.compile(r"^xenminer/([^/]+)/(\w+)$")


def parse_topic(topic: str):
    """Parse an MQTT topic into (worker_id, message_type), or (None, None) if invalid."""
    m = _TOPIC_RE.match(topic)
    if m:
        return m.group(1), m.group(2)
    return None, None

# ---------------------------------------------------------------------------
# Platform server
# ---------------------------------------------------------------------------

class PlatformServer:
    """Single-process mock platform server combining MQTT broker, REST API, and services."""

    def __init__(self, mqtt_port: int = 1883, api_port: int = 8080, db_path: str = "data/marketplace.db",
                 enable_chain: bool = True, block_marker: str = "", jwt_secret: str = "",
                 admin_key: str = DEFAULT_ADMIN_KEY, production: bool = False,
                 api_host: str = "127.0.0.1"):
        self.mqtt_port = mqtt_port
        self.api_port = api_port
        # Loopback by default: this is a mock platform with test credentials,
        # so external exposure must be an explicit operator decision
        # (--host 0.0.0.0 or XENMINER_API_HOST).
        self.api_host = api_host
        self.db_path = db_path
        self._block_marker = block_marker
        self._jwt_secret = jwt_secret
        self._admin_key = admin_key
        self._production = production

        # Broker (created immediately, no async needed)
        self.broker = MQTTBroker(port=mqtt_port)

        # Storage + services are initialized async in start()
        self.storage: Optional[StorageManager] = None
        self.accounts: Optional[AccountService] = None
        self.matcher: Optional[MatchingEngine] = None
        self.watcher: Optional[BlockWatcher] = None
        self.settlement: Optional[SettlementEngine] = None
        self.pricing: Optional[PricingEngine] = None
        self.reputation: Optional[ReputationEngine] = None
        self.auth: Optional[AuthService] = None
        self.chain: Optional[ChainSimulator] = None
        self._enable_chain = enable_chain
        self.ws_manager: Optional[WSManager] = None
        self.monitoring: Optional[MonitoringService] = None
        self.wallet_snapshot: Optional[WalletSnapshotService] = None

        # FastAPI app
        self.app = FastAPI(title="XenMiner Mock Platform", version="0.3.0")
        self._install_session_cookie_auth()
        self._register_routes()
        register_dashboard(self.app)

        # Embed chain simulator routes on the same app
        if self._enable_chain:
            self.chain = ChainSimulator(block_marker=self._block_marker)
            self.chain.register_routes(self.app)
            logger.info("Chain simulator embedded on platform server")

        # Register MQTT message handler
        self.broker.on_message(self._on_mqtt_message)

    async def _init_services(self):
        """Initialize storage and wire up services (must be called in async context)."""
        db_dir = os.path.dirname(self.db_path)
        if db_dir:
            os.makedirs(db_dir, exist_ok=True)

        self.storage = StorageManager(self.db_path)
        await self.storage.initialize()

        self.accounts = AccountService(self.storage.accounts)
        await self.accounts.setup_defaults()

        self.matcher = MatchingEngine(
            self.broker, self.accounts,
            self.storage.workers, self.storage.leases,
        )
        self.watcher = BlockWatcher(self.storage.blocks, self.storage.leases, chain=self.chain,
                                     worker_repo=self.storage.workers)
        self.settlement = SettlementEngine(self.accounts, self.storage.settlements)
        await self.settlement.setup_defaults()
        self.pricing = PricingEngine(self.storage.workers)
        self.reputation = ReputationEngine(
            self.storage.workers, self.storage.leases, self.storage.blocks,
        )
        self.auth = AuthService(
            self.storage.accounts,
            admin_key=self._admin_key,
            jwt_secret=self._jwt_secret,
            production=self._production,
        )
        await ensure_api_keys_for_defaults(self.auth)

        self.ws_manager = WSManager(self.storage.workers, self.storage.blocks)
        self.monitoring = MonitoringService(
            self.storage.workers, self.storage.blocks, self.storage.snapshots,
        )
        self.wallet_snapshot = WalletSnapshotService(self.storage, interval_sec=3600)

        logger.info("Services initialized (db=%s)", self.db_path)

    # -------------------------------------------------------------------
    # MQTT message dispatcher
    # -------------------------------------------------------------------

    async def _on_mqtt_message(self, topic: str, payload: dict, client_id: str):
        worker_id, msg_type = parse_topic(topic)
        if worker_id is None:
            return

        if msg_type == "register":
            await self.matcher.register_worker(payload)
        elif msg_type == "heartbeat":
            await self.matcher.update_heartbeat(payload)
            if self.ws_manager:
                await self.ws_manager.broadcast("heartbeat", {
                    "worker_id": payload.get("worker_id", ""),
                    "hashrate": payload.get("hashrate", 0.0),
                    "active_gpus": payload.get("active_gpus", 0),
                })
        elif msg_type == "status":
            await self.matcher.update_worker_state(payload)
        elif msg_type == "block":
            await self.watcher.handle_block_found(payload, worker_id)
            if self.ws_manager:
                await self.ws_manager.broadcast("block", {
                    "worker_id": worker_id,
                    "hash": payload.get("hash", ""),
                    "lease_id": payload.get("lease_id", ""),
                })
        else:
            logger.debug("Unhandled topic: %s", topic)

    # -------------------------------------------------------------------
    # Lease watchdog
    # -------------------------------------------------------------------

    async def _lease_watchdog(self):
        """Periodically check for expired leases and settle them."""
        while True:
            try:
                expired = await self.matcher.check_expired_leases()
                for lease in expired:
                    await self.settlement.settle_lease(lease)
            except Exception:
                logger.exception("Error in lease watchdog")
            await asyncio.sleep(5)

    async def _monitoring_loop(self):
        """Periodic monitoring: snapshot hashrates, check health, cleanup."""
        tick = 0
        while True:
            try:
                if tick % 30 == 0 and self.monitoring:
                    await self.monitoring.record_hashrate_snapshot()
                if tick % 60 == 0 and self.monitoring and self.ws_manager:
                    unhealthy = await self.monitoring.check_worker_health()
                    if unhealthy:
                        await self.ws_manager.broadcast("health", unhealthy)
                if tick % 3600 == 0 and self.monitoring:
                    await self.monitoring.cleanup_old_snapshots()
            except Exception:
                logger.exception("Error in monitoring loop")
            await asyncio.sleep(1)
            tick += 1

    # -------------------------------------------------------------------
    # Route registration (delegated to modular routers)
    # -------------------------------------------------------------------

    def _install_session_cookie_auth(self):
        """Expose the HttpOnly browser session to existing bearer-aware handlers."""
        @self.app.middleware("http")
        async def session_cookie_auth(request, call_next):
            token = request.cookies.get(SESSION_COOKIE_NAME, "")
            has_authorization = any(
                name.lower() == b"authorization"
                for name, _ in request.scope.get("headers", [])
            )
            if token and not has_authorization:
                request.scope["headers"].append(
                    (b"authorization", f"Bearer {token}".encode("latin-1"))
                )
            return await call_next(request)

    def _register_routes(self):
        self.app.state.server = self
        register_all_routers(self.app)

    # -------------------------------------------------------------------
    # Run
    # -------------------------------------------------------------------

    async def start(self):
        """Start storage, broker, watchdog, and API server."""
        await self._init_services()

        await self.broker.start()
        logger.info("MQTT broker started on port %d", self.mqtt_port)

        self._watchdog_task = asyncio.create_task(self._lease_watchdog())
        self._monitoring_task = asyncio.create_task(self._monitoring_loop())
        await self.wallet_snapshot.start()

        config = uvicorn.Config(
            self.app,
            host=self.api_host,
            port=self.api_port,
            log_level="info",
        )
        self._uvicorn_server = uvicorn.Server(config)
        logger.info("REST API starting on %s:%d", self.api_host, self.api_port)
        if self.api_host in ("127.0.0.1", "localhost", "::1"):
            logger.info(
                "REST API bound to loopback only; pass --host 0.0.0.0 "
                "(or set XENMINER_API_HOST) to allow external access"
            )
        else:
            logger.warning(
                "REST API is externally reachable on %s:%d — ensure a "
                "non-default admin key is configured", self.api_host, self.api_port,
            )
        await self._uvicorn_server.serve()

    async def stop(self):
        """Stop all services, broker, and the API server."""
        tasks = []
        if hasattr(self, "_watchdog_task") and self._watchdog_task is not None:
            self._watchdog_task.cancel()
            tasks.append(self._watchdog_task)
        if hasattr(self, "_monitoring_task") and self._monitoring_task is not None:
            self._monitoring_task.cancel()
            tasks.append(self._monitoring_task)
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        if self.wallet_snapshot:
            await self.wallet_snapshot.stop()
        await self.broker.stop()
        if self.storage:
            await self.storage.close()
        if hasattr(self, "_uvicorn_server") and self._uvicorn_server is not None:
            self._uvicorn_server.should_exit = True


def main():
    """CLI entry point for the mock platform server."""
    parser = argparse.ArgumentParser(description="XenMiner Mock Platform Server")
    parser.add_argument("--mqtt-port", type=int, default=1883, help="MQTT broker port (default: 1883)")
    parser.add_argument("--api-port", type=int, default=8080, help="REST API port (default: 8080)")
    parser.add_argument("--host", default=os.environ.get("XENMINER_API_HOST", "127.0.0.1"),
                        help="REST API bind address (default: 127.0.0.1, loopback only; "
                             "use 0.0.0.0 or XENMINER_API_HOST to expose externally)")
    parser.add_argument("--db-path", default="data/marketplace.db", help="SQLite database path (default: data/marketplace.db)")
    parser.add_argument("--no-chain", action="store_true", help="Disable embedded chain simulator")
    parser.add_argument("--block-marker", default="", help="Override block detection marker for testing (default: XEN11)")
    parser.add_argument("--jwt-secret", default="", help="Secret key for JWT signing (auto-generated if not set)")
    parser.add_argument("--admin-key", default=os.environ.get("XENMINER_ADMIN_KEY", DEFAULT_ADMIN_KEY),
                        help="Admin API key (or set XENMINER_ADMIN_KEY)")
    parser.add_argument("--production", action="store_true",
                        help="Enable production safety checks; requires a non-default admin key")
    args = parser.parse_args()

    server = PlatformServer(
        mqtt_port=args.mqtt_port, api_port=args.api_port,
        db_path=args.db_path, enable_chain=not args.no_chain,
        block_marker=args.block_marker, jwt_secret=args.jwt_secret,
        admin_key=args.admin_key, production=args.production,
        api_host=args.host,
    )

    logger.info("=" * 60)
    logger.info("  XenMiner Mock Platform Server")
    logger.info("  MQTT broker: localhost:%d", args.mqtt_port)
    logger.info("  REST API:    http://%s:%d (%s)", args.host, args.api_port,
                "loopback only" if args.host in ("127.0.0.1", "localhost", "::1")
                else "EXTERNALLY REACHABLE")
    logger.info("  Dashboard:   http://localhost:%d/dashboard", args.api_port)
    logger.info("  Database:    %s", args.db_path)
    logger.info("  Chain sim:   %s", "enabled" if not args.no_chain else "disabled")
    if args.block_marker:
        logger.info("  Block marker: %s (test override)", args.block_marker)
    logger.info("=" * 60)

    try:
        asyncio.run(server.start())
    except KeyboardInterrupt:
        logger.info("Shutting down...")


if __name__ == "__main__":
    main()
