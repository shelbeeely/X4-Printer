"""End-to-end test of the Planner/Pomodoro sync surface (docs/protocol.md
§1.8/§1.9, docs/planner.md): tasks authored on the Pi (via planner.py,
the same module the admin console's routes call) are fetched by a real
X4Client (tools/simulate_x4.py) through the real SyncApiServer, and a
completion sync-back round-trips idempotently -- the same
"authored centrally, pulled by the device" pattern jobs/calendars already
use, verified the same way test_end_to_end.py verifies the print pipeline:
real (not mocked) server instances on ephemeral localhost ports.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "pi-server"))

from simulate_x4 import X4Client  # noqa: E402

from focusink_server import planner  # noqa: E402


def _make_client(pi_stack) -> X4Client:
    return X4Client(
        base_url=pi_stack["sync_base_url"],
        device_id=pi_stack["device_id"],
        device_token=pi_stack["device_token"],
        verify=False,  # plain HTTP in-process server, see conftest.py's pi_stack docstring
    )


def test_list_planner_tasks_matches_what_was_authored(pi_stack):
    db = pi_stack["db"]
    device_id = pi_stack["device_id"]
    planner.create_task(
        db, device_id=device_id, date="2026-09-04", title="Standup", category="Work",
        start_time="09:00", end_time="09:15",
    )
    planner.create_task(
        db, device_id=device_id, date="2026-09-04", title="Lunch", category="Break",
        start_time="12:00", end_time="13:00",
    )
    # A different day's task must not leak into this day's fetch.
    planner.create_task(
        db, device_id=device_id, date="2026-09-05", title="Other day", category="Other",
        start_time="10:00", end_time="10:30",
    )

    client = _make_client(pi_stack)
    tasks = client.list_planner_tasks("2026-09-04")

    assert [t["title"] for t in tasks] == ["Standup", "Lunch"]
    assert tasks[0]["category"] == "Work"
    assert tasks[0]["done"] is False


def test_pomodoro_config_defaults_when_unset(pi_stack):
    client = _make_client(pi_stack)
    config = client.get_pomodoro_config()
    assert config == {
        "work_minutes": 25,
        "break_minutes": 5,
        "long_break_minutes": 15,
        "sessions_before_long_break": 4,
        "checkpoint_minutes": 5,
    }


def test_complete_task_round_trip_is_idempotent(pi_stack):
    db = pi_stack["db"]
    device_id = pi_stack["device_id"]
    task_id = planner.create_task(
        db, device_id=device_id, date="2026-09-04", title="Standup", category="Work",
        start_time="09:00", end_time="09:15",
    )
    client = _make_client(pi_stack)

    # Apply.
    first = client.complete_planner_task(task_id, completion_id="c1")
    assert first["status"] == "applied"
    assert first["done"] is True

    tasks = client.list_planner_tasks("2026-09-04")
    assert tasks[0]["done"] is True

    # Retry with the SAME completion_id (device rebooted mid-sync, retried
    # the POST) -- must be a no-op, not a second/duplicate application.
    second = client.complete_planner_task(task_id, completion_id="c1")
    assert second["status"] == "already_applied"
    assert second["done"] is True

    count = db.query_one(
        "SELECT COUNT(*) AS n FROM task_completions WHERE task_id = ?", (task_id,)
    )
    assert count["n"] == 1


def test_complete_unknown_task_404s(pi_stack):
    import urllib.error

    client = _make_client(pi_stack)
    try:
        client.complete_planner_task(999999, completion_id="c1")
        assert False, "expected an HTTPError"
    except urllib.error.HTTPError as exc:
        assert exc.code == 404
