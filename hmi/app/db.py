"""SQLite storage on the Raspberry Pi.

The Pi is the source of the configuration (points, graphics) and keeps the full
event history. Points are only written here after the controller has confirmed
the same configuration (matching CRC).
"""

import sqlite3
import threading
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS points (
    id          INTEGER PRIMARY KEY,
    line        INTEGER NOT NULL,
    module      INTEGER NOT NULL,
    channel     INTEGER NOT NULL,
    name        TEXT    NOT NULL DEFAULT '',
    lcd1        TEXT    NOT NULL DEFAULT '',
    lcd2        TEXT    NOT NULL DEFAULT '',
    category    TEXT    NOT NULL,
    device_type TEXT    NOT NULL,
    contact     TEXT    NOT NULL DEFAULT 'NC',
    delay_s     INTEGER NOT NULL DEFAULT 0,
    section     INTEGER NOT NULL DEFAULT 1,
    enabled     INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS pages (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    name      TEXT NOT NULL,
    filename  TEXT,
    parent_id INTEGER
);
CREATE TABLE IF NOT EXISTS markers (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    page_id  INTEGER NOT NULL REFERENCES pages(id) ON DELETE CASCADE,
    x        REAL NOT NULL,
    y        REAL NOT NULL,
    point_id INTEGER
);
CREATE TABLE IF NOT EXISTS events (
    seq         INTEGER PRIMARY KEY,
    time        REAL NOT NULL,
    type        TEXT NOT NULL,
    source_kind TEXT NOT NULL,
    source_id   INTEGER NOT NULL,
    old         TEXT,
    new         TEXT,
    user        INTEGER
);
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""

POINT_COLUMNS = ("id", "line", "module", "channel", "name", "lcd1", "lcd2", "category",
                 "device_type", "contact", "delay_s", "section", "enabled")


class Database:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(self.path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA foreign_keys = ON")
        self._conn.execute("PRAGMA journal_mode = WAL")
        self._conn.executescript(SCHEMA)
        self._conn.commit()

    def _q(self, sql, args=()):
        with self._lock:
            return [dict(r) for r in self._conn.execute(sql, args).fetchall()]

    def _x(self, sql, args=()):
        with self._lock:
            cur = self._conn.execute(sql, args)
            self._conn.commit()
            return cur.lastrowid

    # --- points
    def points(self) -> list[dict]:
        rows = self._q("SELECT * FROM points ORDER BY id")
        for r in rows:
            r["enabled"] = bool(r["enabled"])
        return rows

    def replace_points(self, points: list[dict]):
        """Write the full confirmed point list in one transaction."""
        with self._lock:
            cols = ",".join(POINT_COLUMNS)
            marks = ",".join("?" for _ in POINT_COLUMNS)
            self._conn.execute("BEGIN")
            try:
                self._conn.execute("DELETE FROM points")
                self._conn.executemany(
                    f"INSERT INTO points ({cols}) VALUES ({marks})",
                    [tuple(int(p[c]) if c == "enabled" else p[c] for c in POINT_COLUMNS) for p in points],
                )
                self._conn.execute("COMMIT")
            except Exception:
                self._conn.execute("ROLLBACK")
                raise

    # --- graphics
    def pages(self) -> list[dict]:
        return self._q("SELECT * FROM pages ORDER BY id")

    def page(self, page_id: int) -> dict | None:
        rows = self._q("SELECT * FROM pages WHERE id = ?", (page_id,))
        return rows[0] if rows else None

    def add_page(self, name: str, filename: str | None) -> int:
        return self._x("INSERT INTO pages (name, filename) VALUES (?, ?)", (name, filename))

    def delete_page(self, page_id: int):
        self._x("DELETE FROM pages WHERE id = ?", (page_id,))

    def markers(self, page_id: int | None = None) -> list[dict]:
        if page_id is None:
            return self._q("SELECT * FROM markers ORDER BY id")
        return self._q("SELECT * FROM markers WHERE page_id = ? ORDER BY id", (page_id,))

    def add_marker(self, page_id: int, x: float, y: float, point_id: int) -> int:
        return self._x("INSERT INTO markers (page_id, x, y, point_id) VALUES (?, ?, ?, ?)",
                       (page_id, x, y, point_id))

    def delete_marker(self, marker_id: int):
        self._x("DELETE FROM markers WHERE id = ?", (marker_id,))

    # --- events
    def last_event_seq(self) -> int:
        rows = self._q("SELECT MAX(seq) AS s FROM events")
        return rows[0]["s"] or 0

    def add_events(self, events: list[dict]):
        if not events:
            return
        with self._lock:
            self._conn.executemany(
                "INSERT OR IGNORE INTO events (seq, time, type, source_kind, source_id, old, new, user)"
                " VALUES (:seq, :time, :type, :source_kind, :source_id, :old, :new, :user)",
                events,
            )
            self._conn.commit()

    def events(self, limit: int = 200, text: str = "") -> list[dict]:
        if text:
            like = f"%{text}%"
            return self._q(
                "SELECT e.*, p.name AS point_name FROM events e"
                " LEFT JOIN points p ON e.source_kind = 'POINT' AND p.id = e.source_id"
                " WHERE e.type LIKE ? OR p.name LIKE ? ORDER BY e.seq DESC LIMIT ?",
                (like, like, limit),
            )
        return self._q(
            "SELECT e.*, p.name AS point_name FROM events e"
            " LEFT JOIN points p ON e.source_kind = 'POINT' AND p.id = e.source_id"
            " ORDER BY e.seq DESC LIMIT ?",
            (limit,),
        )

    def clear_events(self):
        """Used when the controller's event sequence restarts (e.g. new controller)."""
        self._x("DELETE FROM events")
