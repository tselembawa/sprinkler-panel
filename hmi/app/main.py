"""HMI web application (runs on the Raspberry Pi).

Start from the hmi folder:
    uvicorn app.main:app --host 0.0.0.0 --port 8000

The same pages are used on the touchscreen (Chromium kiosk) and from a laptop.
"""

from __future__ import annotations

import contextlib
import csv
import io
import json
import os
import threading
import time
import uuid
from datetime import datetime
from pathlib import Path

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Mount, Route
from starlette.staticfiles import StaticFiles

from . import protocol as P
from .controller_link import CommandError, make_controller
from .db import Database

BASE = Path(__file__).resolve().parent
DATA = Path(os.environ.get("SPRINKLER_DATA", BASE.parent / "data"))
UPLOADS = DATA / "uploads"
UPLOADS.mkdir(parents=True, exist_ok=True)

ALLOWED_IMAGES = {".jpg", ".jpeg", ".png", ".webp", ".gif"}
MAX_IMAGE_BYTES = 15 * 1024 * 1024

db = Database(DATA / "sprinkler.db")
controller = make_controller(start_seq=db.last_event_seq())

I18N = json.loads((BASE / "static" / "i18n.json").read_text(encoding="utf-8"))
LANGUAGES = tuple(I18N["languages"])


def language() -> str:
    lang = db.get_setting("language", I18N["default"])
    return lang if lang in LANGUAGES else I18N["default"]


def tr(key: str, lang: str | None = None) -> str:
    lang = lang or language()
    return I18N["strings"].get(lang, {}).get(key) or I18N["strings"][I18N["default"]].get(key, key)


def event_label(etype: str, lang: str | None = None) -> str:
    return tr(f"ev.type.{etype}", lang) if f"ev.type.{etype}" in I18N["strings"]["sv"] else etype


# --------------------------------------------------------------- background
class Background:
    """Runs the controller tick (simulator only) and copies events to SQLite."""

    def __init__(self):
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []

    def start(self):
        if hasattr(controller, "tick"):
            self._threads.append(threading.Thread(target=self._tick, daemon=True))
        self._threads.append(threading.Thread(target=self._sync, daemon=True))
        for t in self._threads:
            t.start()

    def stop(self):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=2)

    def _tick(self):
        while not self._stop.wait(0.1):
            controller.tick()

    def _sync(self):
        while not self._stop.wait(0.5):
            sync_events()


def sync_events():
    last = db.last_event_seq()
    while True:
        batch = controller.get_events(last, 200)
        if not batch:
            return
        db.add_events(batch)
        last = batch[-1]["seq"]


def push_config_if_needed():
    """At start-up the Pi makes sure the controller runs the stored configuration."""
    points = db.points()
    if controller.config_info()["crc"] != P.config_crc(points):
        res = controller.upload_config(points)
        if not res["ok"]:
            print("WARNING: stored configuration rejected by controller:", res["errors"])


@contextlib.asynccontextmanager
async def lifespan(app):
    push_config_if_needed()
    sync_events()
    bg = Background()
    bg.start()
    try:
        yield
    finally:
        bg.stop()


# ------------------------------------------------------------------ helpers
def err(code: str, status: int = 400, **extra):
    """Errors carry a code; the UI shows the text in the selected language."""
    return JSONResponse({"error": tr(f"errc.{code}") if f"errc.{code}" in I18N["strings"]["sv"] else code,
                         "code": code, **extra}, status_code=status)


def parse_point(body: dict) -> dict:
    def as_int(key, default=None):
        v = body.get(key, default)
        if v in (None, ""):
            raise ValueError(f"Fältet '{key}' saknas")
        return int(v)

    return {
        "id": as_int("id"),
        "line": as_int("line", 1),
        "module": as_int("module"),
        "channel": as_int("channel"),
        "name": str(body.get("name", "")).strip()[:32],
        "lcd1": str(body.get("lcd1", "")).strip().upper(),
        "lcd2": str(body.get("lcd2", "")).strip().upper(),
        "category": str(body.get("category", "B")),
        "device_type": str(body.get("device_type", "GENERIC")),
        "contact": str(body.get("contact", "NC")),
        "delay_s": as_int("delay_s", 0),
        "section": as_int("section", 1),
        "enabled": bool(body.get("enabled", True)),
    }


def save_points(points: list[dict]):
    """Upload to the controller; store on the Pi only if the controller confirms."""
    res = controller.upload_config(points)
    if not res["ok"]:
        return err("CONFIG_REJECTED", 422, errors=res["errors"])
    expected = P.config_crc(points)
    info = controller.config_info()
    if info["crc"] != expected:
        return err("CRC_MISMATCH", 500)
    db.replace_points(points)
    return JSONResponse({"saved": True, "version": info["version"], "crc": f"{info['crc']:08X}"})


# ------------------------------------------------------------------- routes
async def api_meta(request: Request):
    return JSONResponse({
        "categories": P.CATEGORIES,
        "device_types": P.DEVICE_TYPES,
        "latching_device_types": sorted(P.LATCHING_DEVICE_TYPES),
        "contacts": P.CONTACTS,
        "conditions": P.CONDITIONS,
        "max_delay_s": P.MAX_DELAY_S,
        "lcd_line_len": P.LCD_LINE_LEN,
        "limits": P.LIMITS,
        "simulator": hasattr(controller, "sim_set_input"),
    })


async def api_status(request: Request):
    status = controller.get_status()
    status["hmi_config_crc"] = P.config_crc(db.points())
    status["config_in_sync"] = status["hmi_config_crc"] == status["config_crc"]
    status["config_crc"] = f"{status['config_crc']:08X}"
    status["hmi_config_crc"] = f"{status['hmi_config_crc']:08X}"
    return JSONResponse(status)


async def api_points(request: Request):
    runtime = {p["id"]: p for p in controller.get_points()}
    out = []
    for p in db.points():
        p = dict(p)
        p["latching"] = P.is_latching(p["device_type"])
        p["runtime"] = runtime.get(p["id"])
        out.append(p)
    return JSONResponse(out)


async def api_point_save(request: Request):
    try:
        new = parse_point(await request.json())
    except (ValueError, TypeError) as e:
        return err("INVALID", detail=str(e))
    original_id = request.query_params.get("original_id")
    points = [p for p in db.points() if p["id"] != new["id"]]
    if original_id and original_id.isdigit() and int(original_id) != new["id"]:
        points = [p for p in points if p["id"] != int(original_id)]
    points.append(new)
    return save_points(points)


async def api_point_delete(request: Request):
    pid = int(request.path_params["pid"])
    points = [p for p in db.points() if p["id"] != pid]
    resp = save_points(points)
    if resp.status_code == 200:
        for m in db.markers():
            if m["point_id"] == pid:
                db.delete_marker(m["id"])
    return resp


async def api_cmd(request: Request):
    cmd = request.path_params["cmd"]
    body = {}
    with contextlib.suppress(Exception):
        body = await request.json()
    try:
        if cmd == "ack":
            result = controller.cmd_ack()
        elif cmd == "reset":
            result = controller.cmd_reset()
        elif cmd == "disable":
            result = controller.cmd_disable(int(body["point_id"]), bool(body.get("disabled", True)))
        else:
            return err("UNKNOWN_COMMAND", 404)
    except CommandError as e:
        return err(e.code, 403)
    sync_events()
    return JSONResponse(result)


async def api_events(request: Request):
    limit = min(int(request.query_params.get("limit", 200)), 5000)
    text = request.query_params.get("q", "").strip()
    rows = db.events(limit, text)
    lang = language()
    for r in rows:
        r["label"] = event_label(r["type"], lang)
    return JSONResponse(rows)


async def api_events_csv(request: Request):
    rows = db.events(100000, request.query_params.get("q", "").strip())
    lang = language()
    buf = io.StringIO()
    w = csv.writer(buf, delimiter=";")
    w.writerow([tr(k, lang) for k in ("th.nr", "th.time", "th.event")] + ["Type", tr("th.source", lang), "Id",
               tr("th.point", lang), tr("th.from", lang), tr("th.to", lang)])
    for r in rows:
        w.writerow([r["seq"], datetime.fromtimestamp(r["time"]).strftime("%Y-%m-%d %H:%M:%S"),
                    event_label(r["type"], lang), r["type"], r["source_kind"], r["source_id"],
                    r.get("point_name") or "", r["old"] or "", r["new"] or ""])
    name = f"events_{datetime.now():%Y%m%d_%H%M}.csv"
    return Response("﻿" + buf.getvalue(), media_type="text/csv; charset=utf-8",
                    headers={"Content-Disposition": f'attachment; filename="{name}"'})


async def api_pages(request: Request):
    if request.method == "GET":
        pages = db.pages()
        for pg in pages:
            pg["url"] = f"/uploads/{pg['filename']}" if pg["filename"] else None
            pg["markers"] = db.markers(pg["id"])
        return JSONResponse(pages)

    form = await request.form()
    name = str(form.get("name", "")).strip() or "Ny bild"
    upload = form.get("file")
    if upload is None or not getattr(upload, "filename", ""):
        return err("NO_IMAGE")
    ext = Path(upload.filename).suffix.lower()
    if ext not in ALLOWED_IMAGES:
        return err("BAD_TYPE")
    data = await upload.read()
    if len(data) > MAX_IMAGE_BYTES:
        return err("TOO_BIG")
    filename = f"{uuid.uuid4().hex}{ext}"
    (UPLOADS / filename).write_bytes(data)
    pid = db.add_page(name, filename)
    return JSONResponse({"id": pid, "name": name, "url": f"/uploads/{filename}"})


async def api_page_delete(request: Request):
    page = db.page(int(request.path_params["pid"]))
    if not page:
        return err("NOT_FOUND", 404)
    db.delete_page(page["id"])
    if page["filename"]:
        with contextlib.suppress(FileNotFoundError):
            (UPLOADS / page["filename"]).unlink()
    return JSONResponse({"deleted": page["id"]})


async def api_markers(request: Request):
    page_id = int(request.path_params["pid"])
    if not db.page(page_id):
        return err("NOT_FOUND", 404)
    body = await request.json()
    x, y = float(body["x"]), float(body["y"])
    if not (0 <= x <= 1 and 0 <= y <= 1):
        return err("INVALID")
    pid = int(body["point_id"])
    if pid not in {p["id"] for p in db.points()}:
        return err("NOT_FOUND")
    mid = db.add_marker(page_id, x, y, pid)
    return JSONResponse({"id": mid})


async def api_marker_delete(request: Request):
    db.delete_marker(int(request.path_params["mid"]))
    return JSONResponse({"deleted": True})


# --- settings
async def api_settings(request: Request):
    if request.method == "GET":
        return JSONResponse({"language": language(), "languages": I18N["languages"],
                             "time": controller.get_status()["time"]})
    body = await request.json()
    if not controller.get_status()["key_tech"]:
        return err("ACCESS_DENIED", 403)
    lang = str(body.get("language", ""))
    if lang not in LANGUAGES:
        return err("BAD_LANGUAGE")
    db.set_setting("language", lang)
    return JSONResponse({"language": lang})


async def api_time(request: Request):
    body = await request.json()
    try:
        result = controller.set_time(float(body["time"]))
    except (KeyError, TypeError, ValueError):
        return err("BAD_TIME")
    except CommandError as e:
        return err(e.code, 403 if e.code == "ACCESS_DENIED" else 400)
    sync_events()
    return JSONResponse(result)


# --- simulator only
async def api_sim(request: Request):
    if not hasattr(controller, "sim_set_input"):
        return err("NOT_FOUND", 404)
    what = request.path_params["what"]
    body = await request.json()
    try:
        if what == "input":
            controller.sim_set_input(int(body["line"]), int(body["module"]), int(body["channel"]),
                                     str(body["condition"]))
        elif what == "key":
            controller.sim_set_key(bool(body["tech"]))
        elif what == "module":
            controller.sim_set_module_lost(int(body["line"]), int(body["module"]), bool(body["lost"]))
        else:
            return err("NOT_FOUND", 404)
    except CommandError as e:
        return err(e.code)
    sync_events()
    return JSONResponse({"ok": True})


routes = [
    Route("/api/meta", api_meta),
    Route("/api/status", api_status),
    Route("/api/points", api_points, methods=["GET"]),
    Route("/api/points", api_point_save, methods=["POST"]),
    Route("/api/points/{pid:int}", api_point_delete, methods=["DELETE"]),
    Route("/api/cmd/{cmd}", api_cmd, methods=["POST"]),
    Route("/api/events", api_events),
    Route("/api/events.csv", api_events_csv),
    Route("/api/pages", api_pages, methods=["GET", "POST"]),
    Route("/api/pages/{pid:int}", api_page_delete, methods=["DELETE"]),
    Route("/api/pages/{pid:int}/markers", api_markers, methods=["POST"]),
    Route("/api/markers/{mid:int}", api_marker_delete, methods=["DELETE"]),
    Route("/api/settings", api_settings, methods=["GET", "POST"]),
    Route("/api/time", api_time, methods=["POST"]),
    Route("/api/sim/{what}", api_sim, methods=["POST"]),
    Mount("/uploads", StaticFiles(directory=UPLOADS), name="uploads"),
    Mount("/", StaticFiles(directory=BASE / "static", html=True), name="static"),
]

app = Starlette(routes=routes, lifespan=lifespan)
