#!/usr/bin/env bash
# Driver for the relay: launch a real instance and drive its full
# approval-envelope lifecycle with plain curl (no client library needed --
# see docs/protocol.md sec2). Run from relay/ (the directory this skill
# lives under .claude/skills/ of).
#
# Usage:
#   .claude/skills/run-relay/driver.sh start   # launch in the background, print connection info
#   .claude/skills/run-relay/driver.sh smoke   # start (if needed) + full drive-through + report
#   .claude/skills/run-relay/driver.sh stop    # stop the background instance
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_DIR="$(cd "$HERE/../../.." && pwd)"
SCRATCH="${RUN_RELAY_SCRATCH:-/tmp/run-relay-scratch}"
PORT="${RUN_RELAY_PORT:-18843}"

PID_FILE="$SCRATCH/relay.pid"
LOG_FILE="$SCRATCH/relay.log"
DATA_DIR="$SCRATCH/relay-data"
ACCOUNT_FILE="$SCRATCH/account.env"

do_start() {
  mkdir -p "$DATA_DIR"
  if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "already running (pid $(cat "$PID_FILE"))" >&2
    return 0
  fi

  (
    cd "$RELAY_DIR"
    FOCUSINK_RELAY_DATA_DIR="$DATA_DIR" \
    FOCUSINK_RELAY_PORT="$PORT" \
    FOCUSINK_RELAY_LOG_LEVEL=INFO \
    python3 -m relay_server.server > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
  )

  for _ in $(seq 1 20); do
    grep -qE "relay listening|TLS not configured" "$LOG_FILE" 2>/dev/null && break
    sleep 0.5
  done
  if ! grep -qE "relay listening|TLS not configured" "$LOG_FILE" 2>/dev/null; then
    echo "relay did not start -- see $LOG_FILE" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi

  if [ ! -f "$ACCOUNT_FILE" ]; then
    OUT=$(cd "$RELAY_DIR" && FOCUSINK_RELAY_DATA_DIR="$DATA_DIR" python3 tools/create_account.py --name "Driver Smoke Household")
    ACCOUNT_ID=$(echo "$OUT" | grep "account_id:" | awk '{print $2}')
    ACCOUNT_TOKEN=$(echo "$OUT" | grep "account_token:" | awk '{print $2}')
    echo "ACCOUNT_ID=$ACCOUNT_ID" > "$ACCOUNT_FILE"
    echo "ACCOUNT_TOKEN=$ACCOUNT_TOKEN" >> "$ACCOUNT_FILE"
  fi
  # shellcheck disable=SC1090
  source "$ACCOUNT_FILE"

  echo "relay running (pid $(cat "$PID_FILE")), no TLS (plaintext, dev-only)"
  echo "  Base:    http://127.0.0.1:$PORT/relay/v1/accounts/$ACCOUNT_ID"
  echo "  Token:   $ACCOUNT_TOKEN"
  echo "  Data:    $DATA_DIR"
  echo "  Log:     $LOG_FILE"
  echo "  Account: $ACCOUNT_FILE (source this file for \$ACCOUNT_ID/\$ACCOUNT_TOKEN)"
}

do_stop() {
  if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    kill -9 "$(cat "$PID_FILE")" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
  echo "stopped"
}

do_smoke() {
  do_start
  # shellcheck disable=SC1090
  source "$ACCOUNT_FILE"
  BASE="http://127.0.0.1:$PORT/relay/v1/accounts/$ACCOUNT_ID"

  echo
  echo "--- submit an approval envelope ---"
  curl -sS -X POST "$BASE/approvals" -H "Authorization: Bearer $ACCOUNT_TOKEN" -H "Content-Type: application/json" \
    -d '{"approval_id":"appr-driver-smoke","device_id":"dev-smoke","job_id":"job-smoke","action":"print","created_at":1}'
  echo

  echo "--- list pending ---"
  curl -sS "$BASE/approvals/pending" -H "Authorization: Bearer $ACCOUNT_TOKEN"
  echo

  echo "--- ack it ---"
  curl -sS -X POST "$BASE/approvals/appr-driver-smoke/ack" -H "Authorization: Bearer $ACCOUNT_TOKEN" \
    -H "Content-Type: application/json" -d '{}'
  echo

  echo "--- list pending again (should be empty) ---"
  curl -sS "$BASE/approvals/pending" -H "Authorization: Bearer $ACCOUNT_TOKEN"
  echo

  echo "--- unauthenticated request (expect 401) ---"
  curl -sS -o /dev/null -w "%{http_code}\n" "$BASE/approvals/pending"

  echo
  echo "smoke test complete."
}

case "${1:-}" in
  start) do_start ;;
  stop) do_stop ;;
  smoke) do_smoke ;;
  *) echo "usage: $0 {start|stop|smoke}" >&2; exit 1 ;;
esac
