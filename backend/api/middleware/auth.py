"""
Bearer-token auth middleware for the FastAPI app.

When `API_BEARER_TOKEN` is set in config, every request to /api/v1/*
must include `Authorization: Bearer <token>`. Without it the SSE stream
is open to anyone on the LAN — explicitly unsafe for a contested net.

Health endpoints (`/healthz`, `/readyz`, `/metrics`) are intentionally
not gated so a watchdog / Prometheus scraper can hit them without
shipping the secret.

Implementation note: we accept BOTH the standard `Authorization: Bearer
<token>` header AND a `?token=` query string fallback. The query
fallback is for the SSE EventSource API in the operator UI, which
can't set custom headers without a polyfill — pragmatic v1.
"""
from __future__ import annotations

import logging
from typing import Iterable, Optional

logger = logging.getLogger(__name__)

DEFAULT_PUBLIC_PATHS = ("/healthz", "/readyz", "/metrics", "/health",
                        "/docs", "/openapi.json", "/redoc")

# `?token=` query-string fallback only accepted on these paths. SSE/
# EventSource can't set custom headers without a polyfill, so we allow
# the token in the URL there. Everywhere else we require the
# Authorization header so tokens don't leak via referer/server-access
# logs/browser history.
DEFAULT_QUERY_TOKEN_PATHS = ("/api/v1/events/stream", "/api/v1/stream")


def make_bearer_middleware(token: Optional[str],
                           public_paths: Iterable[str] = DEFAULT_PUBLIC_PATHS,
                           query_token_paths: Iterable[str]
                               = DEFAULT_QUERY_TOKEN_PATHS):
    """Return a Starlette ASGI middleware function.

    If `token` is None or empty, the middleware is a no-op (auth
    disabled — the v0 / lab default). The startup banner logs which
    mode is active so the operator can't accidentally ship without it."""
    public = tuple(public_paths)
    query_ok = tuple(query_token_paths)

    async def middleware(request, call_next):
        if not token:
            return await call_next(request)
        path = request.url.path
        if path in public or any(path.startswith(p + "/") for p in public):
            return await call_next(request)
        auth = request.headers.get("authorization", "")
        provided = None
        if auth.lower().startswith("bearer "):
            provided = auth[7:].strip()
        # ?token= fallback only on whitelisted SSE paths
        if provided is None and (path in query_ok
                or any(path.startswith(p + "/") for p in query_ok)):
            provided = request.query_params.get("token")
        if provided != token:
            logger.warning("Unauthorized %s %s from %s",
                           request.method, path,
                           request.client.host if request.client else "?")
            try:
                from fastapi.responses import JSONResponse
                return JSONResponse(
                    status_code=401,
                    content={"error": "unauthorized",
                             "detail": "missing or invalid bearer token"})
            except ImportError:
                # Stdlib fallback so the test suite can exercise this
                # logic without FastAPI installed. Production always
                # has FastAPI (it's a hard dep of the API package).
                class _Resp:
                    def __init__(self):
                        self.status_code = 401
                        self.body = b'{"error":"unauthorized"}'
                return _Resp()
        return await call_next(request)

    return middleware
