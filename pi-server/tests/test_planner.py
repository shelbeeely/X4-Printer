import pytest

from focusink_server import planner
from focusink_server.db import Database


def test_create_task_rejects_unknown_category(db: Database):
    with pytest.raises(planner.PlannerError):
        planner.create_task(
            db, device_id="dev1", date="2026-09-04", title="X", category="Not-A-Category",
            start_time="09:00", end_time="09:15",
        )


def test_create_task_rejects_bad_time_format(db: Database):
    with pytest.raises(planner.PlannerError):
        planner.create_task(
            db, device_id="dev1", date="2026-09-04", title="X", category="Work",
            start_time="9am", end_time="09:15",
        )


def test_create_task_rejects_bad_date_format(db: Database):
    with pytest.raises(planner.PlannerError):
        planner.create_task(
            db, device_id="dev1", date="09/04/2026", title="X", category="Work",
            start_time="09:00", end_time="09:15",
        )


def test_create_task_rejects_empty_title(db: Database):
    with pytest.raises(planner.PlannerError):
        planner.create_task(
            db, device_id="dev1", date="2026-09-04", title="   ", category="Work",
            start_time="09:00", end_time="09:15",
        )


def test_list_tasks_shape_and_scoping(db: Database):
    planner.create_task(
        db, device_id="dev1", date="2026-09-04", title="Standup", category="Work",
        start_time="09:00", end_time="09:15",
    )
    planner.create_task(
        db, device_id="dev1", date="2026-09-05", title="Other day", category="Other",
        start_time="09:00", end_time="09:15",
    )
    tasks = planner.list_tasks(db, "dev1", "2026-09-04")
    assert len(tasks) == 1
    task = tasks[0]
    assert set(task.keys()) == {"id", "title", "category", "start_time", "end_time", "done"}
    assert task["title"] == "Standup"
    assert task["category"] == "Work"
    assert task["done"] is False


def test_complete_task_idempotent_and_not_found(db: Database):
    task_id = planner.create_task(
        db, device_id="dev1", date="2026-09-04", title="Standup", category="Work",
        start_time="09:00", end_time="09:15",
    )
    first = planner.complete_task(db, device_id="dev1", task_id=task_id, completion_id="c1")
    assert first["status"] == "applied"
    assert first["done"] is True

    second = planner.complete_task(db, device_id="dev1", task_id=task_id, completion_id="c1")
    assert second["status"] == "already_applied"
    assert second["done"] is True

    assert planner.complete_task(db, device_id="dev1", task_id=999999, completion_id="c2") is None


def test_get_pomodoro_config_defaults(db: Database):
    assert planner.get_pomodoro_config(db, "dev1") == planner.DEFAULT_POMODORO_CONFIG


def test_set_pomodoro_config_partial_update_merges_onto_defaults(db: Database):
    result = planner.set_pomodoro_config(db, "dev1", work_minutes=30)
    assert result["work_minutes"] == 30
    assert result["break_minutes"] == planner.DEFAULT_POMODORO_CONFIG["break_minutes"]
    assert result["checkpoint_minutes"] == planner.DEFAULT_POMODORO_CONFIG["checkpoint_minutes"]

    # A second partial update merges onto the *stored* config, not the defaults.
    result2 = planner.set_pomodoro_config(db, "dev1", break_minutes=10)
    assert result2["work_minutes"] == 30
    assert result2["break_minutes"] == 10
