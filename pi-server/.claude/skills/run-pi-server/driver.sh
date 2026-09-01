#!/usr/bin/env bash
# Driver for pi-server: launch a real instance, drive it with real clients
# (curl for IPP, tools/simulate_x4.py for the sync API, Playwright for the
# admin console), and tear it down. Run from pi-server/ (the directory
# this skill lives under .claude/skills/ of).
#
# Usage:
#   .claude/skills/run-pi-server/driver.sh start   # launch in the background, print connection info
#   .claude/skills/run-pi-server/driver.sh smoke   # start (if needed) + full drive-through + report
#   .claude/skills/run-pi-server/driver.sh stop    # stop the background instance
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../../.." && pwd)"
PI_SERVER_DIR="$REPO_ROOT/pi-server"
SCRATCH="${RUN_PI_SERVER_SCRATCH:-/tmp/run-pi-server-scratch}"
VENV="$PI_SERVER_DIR/.venv"
PY="$VENV/bin/python"

IPP_PORT="${RUN_PI_SERVER_IPP_PORT:-16310}"
SYNC_PORT="${RUN_PI_SERVER_SYNC_PORT:-18443}"
ADMIN_PORT="${RUN_PI_SERVER_ADMIN_PORT:-18090}"
ADMIN_PASSWORD="${RUN_PI_SERVER_ADMIN_PASSWORD:-smoketest123}"

PID_FILE="$SCRATCH/server.pid"
LOG_FILE="$SCRATCH/server.log"
DATA_DIR="$SCRATCH/pi-data"

ensure_venv() {
  if [ ! -x "$PY" ]; then
    echo "creating venv + installing pi-server deps (first run only)..." >&2
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$PI_SERVER_DIR/requirements.txt"
  fi
}

do_start() {
  ensure_venv
  mkdir -p "$DATA_DIR"
  if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "already running (pid $(cat "$PID_FILE"))" >&2
    return 0
  fi

  "$PY" "$PI_SERVER_DIR/tools/gen_selfsigned_cert.py" --out-dir "$DATA_DIR/tls" --ip 127.0.0.1 >/dev/null

  (
    cd "$PI_SERVER_DIR"
    FOCUSINK_DATA_DIR="$DATA_DIR" \
    FOCUSINK_TLS_CERT="$DATA_DIR/tls/server.crt" \
    FOCUSINK_TLS_KEY="$DATA_DIR/tls/server.key" \
    FOCUSINK_IPP_PORT="$IPP_PORT" \
    FOCUSINK_SYNC_PORT="$SYNC_PORT" \
    FOCUSINK_ADMIN_PORT="$ADMIN_PORT" \
    FOCUSINK_ADMIN_PASSWORD="$ADMIN_PASSWORD" \
    FOCUSINK_LOG_LEVEL=INFO \
    "$PY" -m focusink_server.server > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
  )

  for _ in $(seq 1 30); do
    grep -q "focusink server ready" "$LOG_FILE" 2>/dev/null && break
    sleep 0.5
  done
  if ! grep -q "focusink server ready" "$LOG_FILE" 2>/dev/null; then
    echo "server did not become ready -- see $LOG_FILE" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi

  echo "pi-server running (pid $(cat "$PID_FILE"))"
  echo "  IPP:   http://127.0.0.1:$IPP_PORT/ipp/print"
  echo "  Sync:  https://127.0.0.1:$SYNC_PORT/api/v1  (CA: $DATA_DIR/tls/server.crt)"
  echo "  Admin: https://127.0.0.1:$ADMIN_PORT/  (user: admin, password: $ADMIN_PASSWORD)"
  echo "  Data:  $DATA_DIR"
  echo "  Log:   $LOG_FILE"
}

do_stop() {
  if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    kill "$(cat "$PID_FILE")"
    sleep 1
    kill -9 "$(cat "$PID_FILE")" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
  echo "stopped"
}

do_smoke() {
  do_start

  echo
  echo "--- submit a real IPP print job ---"
  "$PY" "$HERE/submit_print_job.py" "http://127.0.0.1:$IPP_PORT/ipp/print" "Driver Smoke Test"

  echo
  echo "--- pair a fake X4 device ---"
  (cd "$PI_SERVER_DIR" && \
    FOCUSINK_DATA_DIR="$DATA_DIR" FOCUSINK_TLS_CERT="$DATA_DIR/tls/server.crt" FOCUSINK_TLS_KEY="$DATA_DIR/tls/server.key" \
    FOCUSINK_SYNC_PORT="$SYNC_PORT" \
    "$PY" tools/pair_device.py --name "Driver Smoke X4" --pi-host 127.0.0.1 --out "$SCRATCH/device.json")

  echo
  echo "--- sync (list, download+verify, ack -- via tools/simulate_x4.py) ---"
  "$PY" "$REPO_ROOT/tools/simulate_x4.py" --pairing-file "$SCRATCH/device.json" \
    --ca-cert "$DATA_DIR/tls/server.crt" list-jobs
  JOB_ID=$("$PY" "$REPO_ROOT/tools/simulate_x4.py" --pairing-file "$SCRATCH/device.json" \
    --ca-cert "$DATA_DIR/tls/server.crt" list-jobs | tail -1 | awk '{print $1}')
  "$PY" "$REPO_ROOT/tools/simulate_x4.py" --pairing-file "$SCRATCH/device.json" \
    --ca-cert "$DATA_DIR/tls/server.crt" sync --download-dir "$SCRATCH/inbox"
  ls -la "$SCRATCH/inbox"

  echo
  echo "--- approve the job (keep) ---"
  "$PY" "$REPO_ROOT/tools/simulate_x4.py" --pairing-file "$SCRATCH/device.json" \
    --ca-cert "$DATA_DIR/tls/server.crt" approve "$JOB_ID" keep

  echo
  echo "--- admin console: curl status + Playwright screenshot ---"
  curl -sk -u "admin:$ADMIN_PASSWORD" "https://127.0.0.1:$ADMIN_PORT/api/admin/v1/status"
  echo
  "$PY" -c "import playwright" 2>/dev/null || "$VENV/bin/pip" install -q playwright
  "$PY" "$HERE/screenshot_admin.py" "https://127.0.0.1:$ADMIN_PORT" "$ADMIN_PASSWORD" "$SCRATCH/admin_console.png"

  echo
  echo "smoke test complete. Screenshot: $SCRATCH/admin_console.png"
}

case "${1:-}" in
  start) do_start ;;
  stop) do_stop ;;
  smoke) do_smoke ;;
  *) echo "usage: $0 {start|stop|smoke}" >&2; exit 1 ;;
esac
