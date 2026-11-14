"""The web API refuses changes to the installation without the technician key (review 2026-11-14).

No extra packages: the Starlette app is called directly as an ASGI application.
"""

import asyncio
import importlib
import json
import sys

import pytest


def call(app, method, path, body=None, upload=None, client="192.168.1.50"):
    raw = json.dumps(body).encode() if body is not None else b""
    ctype = b"application/json"
    if upload is not None:                      # (filename, bytes) as a multipart form field "file"
        raw = (b"--XX\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + upload[0].encode()
               + b"\"\r\nContent-Type: application/octet-stream\r\n\r\n" + upload[1] + b"\r\n--XX--\r\n")
        ctype = b"multipart/form-data; boundary=XX"
    scope = {
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1", "method": method,
        "path": path, "raw_path": path.encode(), "query_string": b"", "root_path": "",
        "scheme": "http", "server": ("testserver", 80), "client": (client, 5000),
        "headers": [(b"content-type", ctype), (b"content-length", str(len(raw)).encode())],
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
    yield lambda method, path, body=None, **kw: call(main.app, method, path, body, **kw)
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


def test_lamptest_modules_and_who_did_it(api):
    assert api("POST", "/api/sim/key", {"tech": True})[0] == 200
    assert api("POST", "/api/points", POINT)[0] == 200
    assert api("POST", "/api/cmd/lamptest", {}, client="127.0.0.1")[0] == 200
    assert api("GET", "/api/status")[1]["lamptest"] is True
    assert api("POST", "/api/cmd/ack", {})[0] == 200                       # from the network
    ev = api("GET", "/api/events")[1]
    assert [e["user"] for e in ev if e["type"] == "LAMPTEST"] == [2]       # the panel's screen
    assert [e["user"] for e in ev if e["type"] == "ACK"][0] == 3           # a browser on the network

    mods = api("GET", "/api/modules")[1]
    assert [(m["line"], m["module"], m["state"], m["points"]) for m in mods] == [(1, 2, "RUN", 1)]
    assert mods[0]["inputs"][0] == "CLOSED" and mods[0]["inputs"][1] == "NOT_USED"
    api("POST", "/api/sim/modfault", {"line": 1, "module": 2, "what": "OUT1", "kind": "OPEN"})
    st = api("GET", "/api/status")[1]
    assert st["module_faults"] == [{"line": 1, "module": 2, "what": "OUT1", "kind": "OPEN"}]
    assert st["counts"]["FAULT"] == 1 and not st["relays"]["K3_FAULT_OK"] and st["buzzer"]
    assert api("GET", "/api/modules")[1][0]["faults"][0]["what"] == "OUT1"
    api("POST", "/api/sim/modfault", {"line": 1, "module": 2, "what": "OUT1", "kind": None})
    st = api("GET", "/api/status")[1]
    assert st["module_faults"] == [] and st["relays"]["K3_FAULT_OK"] and not st["buzzer"]


def test_backup_and_restore(api):
    import io
    import zipfile

    assert api("POST", "/api/sim/key", {"tech": True})[0] == 200
    assert api("POST", "/api/points", POINT)[0] == 200
    assert api("POST", "/api/points", dict(POINT, id=2, channel=2, name="Pressostat", category="A",
                                            device_type="PRESSOSTAT", contact="NO"))[0] == 200
    status, blob = api("GET", "/api/backup")
    assert status == 200 and blob[:2] == b"PK"
    z = zipfile.ZipFile(io.BytesIO(blob))
    assert {"backup.json", "events.csv"} <= set(z.namelist())
    assert [p["id"] for p in json.loads(z.read("backup.json"))["points"]] == [1, 2]

    assert api("DELETE", "/api/points/2")[0] == 200
    assert api("DELETE", "/api/points/1")[0] == 200
    assert api("GET", "/api/points")[1] == []

    assert api("POST", "/api/sim/key", {"tech": False})[0] == 200
    assert api("POST", "/api/restore", upload=("b.zip", blob))[0] == 403           # needs the key
    assert api("POST", "/api/sim/key", {"tech": True})[0] == 200
    assert api("POST", "/api/restore", upload=("b.zip", b"not a zip"))[1]["code"] == "BAD_BACKUP"
    status, body = api("POST", "/api/restore", upload=("b.zip", blob))
    assert status == 200 and body["points"] == 2
    pts = api("GET", "/api/points")[1]
    assert [(p["id"], p["name"], p["category"]) for p in pts] == [(1, "Ventil", "B"), (2, "Pressostat", "A")]
    assert api("GET", "/api/status")[1]["config_in_sync"] is True
