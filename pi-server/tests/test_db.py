import sqlite3

from focusink_server.db import Database, SCHEMA, _ensure_column


def _insert_job(
    db: Database,
    title="Doc",
    thumbnail_path: str = "",
    xtc_landscape_path: str = "",
    xtc_landscape_bytes: int = 0,
    xtc_landscape_sha256: str = "",
    xtc_landscape_page_count: int = 0,
) -> str:
    return db.insert_job(
        title=title,
        source="ipp",
        original_path="/tmp/orig.pdf",
        original_mime="application/pdf",
        original_bytes=1234,
        xtc_path="/tmp/out.xtc",
        xtc_bytes=4321,
        xtc_sha256="deadbeef",
        page_count=2,
        thumbnail_path=thumbnail_path,
        xtc_landscape_path=xtc_landscape_path,
        xtc_landscape_bytes=xtc_landscape_bytes,
        xtc_landscape_sha256=xtc_landscape_sha256,
        xtc_landscape_page_count=xtc_landscape_page_count,
    )


def test_insert_and_get_job(db: Database):
    job_id = _insert_job(db)
    row = db.get_job(job_id)
    assert row["title"] == "Doc"
    assert row["status"] == "pending"
    assert row["page_count"] == 2


def test_pending_jobs_hide_delivered(db: Database):
    job_id = _insert_job(db)
    db.register_device("dev-1", "hash", "Test Device", None)

    pending = db.list_pending_jobs_for_device("dev-1")
    assert len(pending) == 1

    db.mark_delivered(job_id, "dev-1")
    pending_after = db.list_pending_jobs_for_device("dev-1")
    assert len(pending_after) == 0

    # A second device hasn't seen it yet.
    pending_other = db.list_pending_jobs_for_device("dev-2")
    assert len(pending_other) == 1


def test_approval_dedup_insert(db: Database):
    job_id = _insert_job(db)
    is_new_1 = db.record_approval_if_new(
        approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    is_new_2 = db.record_approval_if_new(
        approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    assert is_new_1 is True
    assert is_new_2 is False

    row = db.get_approval("appr-1")
    assert row["applied"] == 0

    db.mark_approval_applied("appr-1", detail="printed", cups_job_id=7)
    row = db.get_approval("appr-1")
    assert row["applied"] == 1
    assert row["detail"] == "printed"
    assert row["cups_job_id"] == 7


def test_unapplied_approvals_listed_for_replay(db: Database):
    job_id = _insert_job(db)
    db.record_approval_if_new(
        approval_id="appr-x", device_id="dev-1", job_id=job_id, action="keep", created_at=1, received_via="direct"
    )
    unapplied = db.list_unapplied_approvals()
    assert len(unapplied) == 1
    assert unapplied[0]["approval_id"] == "appr-x"

    db.mark_approval_applied("appr-x", detail="kept")
    assert db.list_unapplied_approvals() == []


def test_insert_job_thumbnail_path_defaults_empty(db: Database):
    job_id = _insert_job(db)
    row = db.get_job(job_id)
    assert row["thumbnail_path"] == ""


def test_insert_job_thumbnail_path_stored(db: Database):
    job_id = _insert_job(db, thumbnail_path="/tmp/thumb.jpg")
    row = db.get_job(job_id)
    assert row["thumbnail_path"] == "/tmp/thumb.jpg"


def test_insert_job_landscape_fields_default_empty(db: Database):
    job_id = _insert_job(db)
    row = db.get_job(job_id)
    assert row["xtc_landscape_path"] == ""
    assert row["xtc_landscape_bytes"] == 0
    assert row["xtc_landscape_sha256"] == ""
    assert row["xtc_landscape_page_count"] == 0


def test_insert_job_landscape_fields_stored(db: Database):
    job_id = _insert_job(
        db,
        xtc_landscape_path="/tmp/out_landscape.xtc",
        xtc_landscape_bytes=5555,
        xtc_landscape_sha256="cafef00d",
        xtc_landscape_page_count=5,
    )
    row = db.get_job(job_id)
    assert row["xtc_landscape_path"] == "/tmp/out_landscape.xtc"
    assert row["xtc_landscape_bytes"] == 5555
    assert row["xtc_landscape_sha256"] == "cafef00d"
    assert row["xtc_landscape_page_count"] == 5


def test_landscape_migration_adds_columns_to_existing_db(tmp_path):
    # Same shape as test_thumbnail_path_migration_adds_column_to_existing_db
    # below, but for the four landscape-strip columns added afterward.
    db_path = tmp_path / "legacy_landscape.db"
    conn = sqlite3.connect(str(db_path))
    conn.executescript(
        """CREATE TABLE jobs (
               job_id TEXT PRIMARY KEY, title TEXT NOT NULL, created_at INTEGER NOT NULL,
               source TEXT NOT NULL DEFAULT 'ipp', original_path TEXT NOT NULL,
               original_mime TEXT NOT NULL, original_bytes INTEGER NOT NULL,
               xtc_path TEXT NOT NULL, xtc_bytes INTEGER NOT NULL, xtc_sha256 TEXT NOT NULL,
               page_count INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'pending',
               thumbnail_path TEXT NOT NULL DEFAULT ''
           );"""
    )
    conn.execute(
        """INSERT INTO jobs (job_id, title, created_at, original_path, original_mime,
           original_bytes, xtc_path, xtc_bytes, xtc_sha256, page_count)
           VALUES ('old-job', 'Old', 1, '/tmp/o.pdf', 'application/pdf', 1, '/tmp/o.xtc', 1, 'x', 1)"""
    )
    conn.commit()
    conn.close()

    db = Database(db_path)
    row = db.get_job("old-job")
    assert row["xtc_landscape_path"] == ""
    assert row["xtc_landscape_bytes"] == 0
    assert row["xtc_landscape_sha256"] == ""
    assert row["xtc_landscape_page_count"] == 0


def test_thumbnail_path_migration_adds_column_to_existing_db(tmp_path):
    # Simulate a jobs.db from before thumbnail_path existed: run the
    # original schema (jobs table minus the column) directly, bypassing
    # Database.__init__'s own migration, then construct a Database against
    # that file and confirm it adds the column rather than erroring.
    db_path = tmp_path / "legacy.db"
    conn = sqlite3.connect(str(db_path))
    conn.executescript(
        """CREATE TABLE jobs (
               job_id TEXT PRIMARY KEY, title TEXT NOT NULL, created_at INTEGER NOT NULL,
               source TEXT NOT NULL DEFAULT 'ipp', original_path TEXT NOT NULL,
               original_mime TEXT NOT NULL, original_bytes INTEGER NOT NULL,
               xtc_path TEXT NOT NULL, xtc_bytes INTEGER NOT NULL, xtc_sha256 TEXT NOT NULL,
               page_count INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'pending'
           );"""
    )
    conn.execute(
        """INSERT INTO jobs (job_id, title, created_at, original_path, original_mime,
           original_bytes, xtc_path, xtc_bytes, xtc_sha256, page_count)
           VALUES ('old-job', 'Old', 1, '/tmp/o.pdf', 'application/pdf', 1, '/tmp/o.xtc', 1, 'x', 1)"""
    )
    conn.commit()
    conn.close()

    db = Database(db_path)  # runs SCHEMA (no-op, table exists) + _ensure_column migration
    row = db.get_job("old-job")
    assert row["thumbnail_path"] == ""  # migration default, not a crash or NULL


def test_ensure_column_is_idempotent(db: Database):
    # thumbnail_path already exists (added by Database.__init__'s own
    # migration) -- calling _ensure_column again must be a no-op, not a
    # "duplicate column" error.
    conn = db._connect()
    _ensure_column(conn, "jobs", "thumbnail_path", "TEXT NOT NULL DEFAULT ''")
    cols = [row[1] for row in conn.execute("PRAGMA table_info(jobs)")]
    assert cols.count("thumbnail_path") == 1


def test_list_recent_approvals_for_device_scopes_by_device(db: Database):
    job_id = _insert_job(db)
    db.record_approval_if_new(
        approval_id="a1", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    db.record_approval_if_new(
        approval_id="a2", device_id="dev-2", job_id=job_id, action="keep", created_at=2, received_via="direct"
    )

    dev1_approvals = db.list_recent_approvals_for_device("dev-1")
    assert len(dev1_approvals) == 1
    assert dev1_approvals[0]["approval_id"] == "a1"
    assert dev1_approvals[0]["job_title"] == "Doc"

    dev2_approvals = db.list_recent_approvals_for_device("dev-2")
    assert len(dev2_approvals) == 1
    assert dev2_approvals[0]["approval_id"] == "a2"

    assert db.list_recent_approvals_for_device("dev-nonexistent") == []


def test_calendar_feeds_ordered_by_position(db: Database):
    first_id = db.add_calendar_feed("https://example.com/a.ics", "A")
    second_id = db.add_calendar_feed("https://example.com/b.ics", "B")
    feeds = db.list_calendar_feeds()
    assert [f["id"] for f in feeds] == [first_id, second_id]
    assert feeds[0]["url"] == "https://example.com/a.ics"
    assert feeds[0]["label"] == "A"


def test_delete_calendar_feed(db: Database):
    feed_id = db.add_calendar_feed("https://example.com/a.ics", "A")
    db.delete_calendar_feed(feed_id)
    assert db.list_calendar_feeds() == []


def test_add_or_update_wifi_network_upserts_by_ssid(db: Database):
    first_id = db.add_or_update_wifi_network("HomeWiFi", "pw1")
    second_id = db.add_or_update_wifi_network("HomeWiFi", "pw2")
    assert first_id == second_id
    networks = db.list_wifi_networks()
    assert len(networks) == 1
    assert networks[0]["password"] == "pw2"


def test_delete_wifi_network(db: Database):
    network_id = db.add_or_update_wifi_network("HomeWiFi", "pw1")
    db.delete_wifi_network(network_id)
    assert db.list_wifi_networks() == []
