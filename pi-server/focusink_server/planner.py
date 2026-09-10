"""Planner task + Pomodoro config business logic (docs/protocol.md's
planner/Pomodoro endpoints, and this feature's own design doc,
docs/planner.md). Validation and shaping live here, not in sync_api.py's
handlers, following the same split db.py's docstring describes for
approvals: one place, reused by every caller (sync API today, an admin
console CRUD unit tomorrow).
"""

from __future__ import annotations

import re
from typing import Optional

from .db import Database

CATEGORIES = ("Work", "Break", "Chore", "Health", "Social", "School", "Personal", "Other")

DEFAULT_POMODORO_CONFIG = {
    "work_minutes": 25,
    "break_minutes": 5,
    "long_break_minutes": 15,
    "sessions_before_long_break": 4,
    "checkpoint_minutes": 5,
}

_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_TIME_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")


class PlannerError(ValueError):
    """Raised for a malformed/invalid planner request -- callers (sync_api
    handlers) catch this and respond 400."""


def is_valid_date(s: str) -> bool:
    return bool(_DATE_RE.match(s))


def is_valid_time(s: str) -> bool:
    return bool(_TIME_RE.match(s))


def create_task(
    db: Database, *, device_id: str, date: str, title: str, category: str, start_time: str, end_time: str
) -> int:
    if not is_valid_date(date):
        raise PlannerError(f"invalid date {date!r}, expected YYYY-MM-DD")
    if category not in CATEGORIES:
        raise PlannerError(f"unknown category {category!r}, expected one of {CATEGORIES}")
    if not is_valid_time(start_time):
        raise PlannerError(f"invalid start_time {start_time!r}, expected HH:MM")
    if not is_valid_time(end_time):
        raise PlannerError(f"invalid end_time {end_time!r}, expected HH:MM")
    title = title.strip()
    if not title:
        raise PlannerError("title must not be empty")
    return db.insert_planner_task(
        device_id=device_id, date=date, title=title, category=category, start_time=start_time, end_time=end_time
    )


def list_tasks(db: Database, device_id: str, date: str) -> list[dict]:
    rows = db.list_planner_tasks(device_id, date)
    return [
        {
            "id": row["id"],
            "title": row["title"],
            "category": row["category"],
            "start_time": row["start_time"],
            "end_time": row["end_time"],
            "done": bool(row["done"]),
        }
        for row in rows
    ]


def complete_task(db: Database, *, device_id: str, task_id: int, completion_id: str) -> Optional[dict]:
    result = db.complete_task_if_new(device_id=device_id, task_id=task_id, completion_id=completion_id)
    if result is None:
        return None
    applied_now, row = result
    return {
        "completion_id": completion_id,
        "task_id": row["id"],
        "status": "applied" if applied_now else "already_applied",
        "done": bool(row["done"]),
    }


def delete_task(db: Database, task_id: int) -> None:
    db.delete_planner_task(task_id)


def get_pomodoro_config(db: Database, device_id: str) -> dict:
    row = db.get_pomodoro_config(device_id)
    if row is None:
        return dict(DEFAULT_POMODORO_CONFIG)
    return {
        "work_minutes": row["work_minutes"],
        "break_minutes": row["break_minutes"],
        "long_break_minutes": row["long_break_minutes"],
        "sessions_before_long_break": row["sessions_before_long_break"],
        "checkpoint_minutes": row["checkpoint_minutes"],
    }


def set_pomodoro_config(db: Database, device_id: str, **fields: int) -> dict:
    merged = get_pomodoro_config(db, device_id)
    for key, value in fields.items():
        if key not in DEFAULT_POMODORO_CONFIG:
            raise PlannerError(f"unknown pomodoro config field {key!r}")
        merged[key] = int(value)
    db.set_pomodoro_config(device_id, **merged)
    return merged
