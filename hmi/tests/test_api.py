"""The web API refuses changes to the installation without the technician key (review 2026-11-14).

No extra packages: the Starlette app is called directly as an ASGI application.
"""

import asyncio
import importlib
import json
import sys

import pytest


def call(app, method, path, body=None):
    raw = json.dumps(body).encode() if body is not None else b""
    scope = {
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1", "method": method,
        "path": path, "raw_path": path.encode(), "query_string": b"", "root_path": "",
        "scheme": "http", "server": ("testserver", 80), "client": ("192.168.1.50", 5000),
        "headers": [(b"content-type", b"application/json"), (b"content-length", str(len(raw)).encode())],
    }
    sent = []

    async def receive():
        return {"type": "http.request", "body": raw, "more_body": False}

    async def send(message):
        sent.append(message)

    asyncio.run(app(scope, receive, send))
    status = next(m["status"] for m in sent if m["type"] == "http.response.start")
    data = b"".join(m.get("body", b"") for m in sent if m["type"] == "http.response.body")
    try:
        return status, json.loads(data)
    except ValueError:
        return status, data


@pytest.fixture
def api(tmp_path, monkeypatch):
    monkeypatch.setenv("SPRINKLER_DATA", str(tmp_path))
    monkeypatch.setenv("SPRINKLER_CONTROLLER", "sim")
    for name in [m for m in sys.modules if m == "app.main"]:
        del sys.modules[name]
    main = importlib.import_module("app.main")
    yield lambda method, path, body=None: call(main.app, method, path, body)
    del sys.modules["app.main"]


POINT = {"id": 1, "line": 1, "module": 2, "channel": 1, "name": "Ventil", "lcd1": "VENTIL", "lcd2": "",
         "category": "B", "device_type": "SERVICEVENTIL", "contact": "NC", "delay_s": 0, "section": 1}


def test_points_cannot_be_changed_without_the_key(api):
    status, body = api("POST", "/api/points", POINT)
    assert status == 403 and body["code"] == "ACCESS_DENIED"
    assert api("GET", "/api/points")[1] == []

    assert api("POST", "/api/sim/key", {"tech": True})[0] == 200
    assert api("POST", "/api/points", POINT)[0] == 200
    assert [p["id"] for p in api("GET", "/api/points")[1]] == [1]

    assert api("POST", "/api/sim/key", {"tech": False})[0] == 200
    assert api("DELETE", "/api/points/1")[0] == 403
    assert api("POST", "/api/pages/1/markers", {"x": 0.5, "y": 0.5, "point_id": 1})[0] == 403
    assert api("DELETE", "/api/markers/1")[0] == 403
    assert api("DELETE", "/api/pages/1")[0] == 403
    assert [p["id"] for p in api("GET", "/api/points")[1]] == [1]


def test_everyone_may_look_and_acknowledge(api):
    assert api("GET", "/api/status")[0] == 200
    assert api("GET", "/api/events")[0] == 200
    assert api("POST", "/api/cmd/ack", {})[0] == 200
    assert api("POST", "/api/cmd/reset", {})[0] == 403          # needs the key (controller says so)
