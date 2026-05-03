"""Bearer-token auth middleware logic — pure stdlib, no FastAPI."""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.api.middleware.auth import (
    make_bearer_middleware, DEFAULT_PUBLIC_PATHS)


class _FakeURL:
    def __init__(self, path):
        self.path = path


class _FakeRequest:
    def __init__(self, path, header=None, query_token=None):
        self.url = _FakeURL(path)
        self.headers = {"authorization": header} if header else {}
        self.query_params = {"token": query_token} if query_token else {}
        self.method = "GET"
        self.client = None


class _FakeResponse:
    def __init__(self, body):
        self.body = body
        self.status_code = 200


async def _ok(_req):
    return _FakeResponse("ok")


class AuthMiddlewareTests(unittest.IsolatedAsyncioTestCase):
    async def test_no_token_configured_passes_everything(self):
        mw = make_bearer_middleware(token=None)
        r = await mw(_FakeRequest("/api/v1/tracks"), _ok)
        self.assertEqual(r.body, "ok")

    async def test_correct_bearer_header_passes(self):
        mw = make_bearer_middleware("secret")
        r = await mw(
            _FakeRequest("/api/v1/tracks", header="Bearer secret"),
            _ok)
        self.assertEqual(r.body, "ok")

    async def test_query_string_fallback_passes(self):
        mw = make_bearer_middleware("secret")
        r = await mw(
            _FakeRequest("/api/v1/events/stream", query_token="secret"),
            _ok)
        self.assertEqual(r.body, "ok")

    async def test_missing_header_returns_401(self):
        mw = make_bearer_middleware("secret")
        r = await mw(_FakeRequest("/api/v1/tracks"), _ok)
        self.assertEqual(r.status_code, 401)

    async def test_wrong_token_returns_401(self):
        mw = make_bearer_middleware("secret")
        r = await mw(
            _FakeRequest("/api/v1/tracks", header="Bearer wrong"),
            _ok)
        self.assertEqual(r.status_code, 401)

    async def test_health_paths_are_public(self):
        mw = make_bearer_middleware("secret")
        for path in ("/healthz", "/readyz", "/metrics"):
            r = await mw(_FakeRequest(path), _ok)
            self.assertEqual(r.body, "ok",
                msg=f"{path} should be public but was rejected")

    async def test_query_token_rejected_on_non_sse_path(self):
        """?token= must NOT authenticate non-SSE paths — tokens in URLs
        leak via referer/access logs. Only the SSE EventSource paths
        accept the query fallback."""
        mw = make_bearer_middleware("secret")
        r = await mw(_FakeRequest("/api/v1/tracks", query_token="secret"),
                     _ok)
        self.assertEqual(r.status_code, 401)

    async def test_case_insensitive_bearer_keyword(self):
        mw = make_bearer_middleware("secret")
        r = await mw(
            _FakeRequest("/api/v1/tracks", header="bearer secret"),
            _ok)
        self.assertEqual(r.body, "ok")


if __name__ == "__main__":
    unittest.main(verbosity=2)
