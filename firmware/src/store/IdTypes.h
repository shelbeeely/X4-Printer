#pragma once
// Shared id-length constants for JobStore.h / ApprovalOutbox.h. Both job_id
// and approval_id are server/device-generated uuid4 hex strings (32 chars,
// no dashes) — see docs/protocol.md.

#include <cstddef>

namespace store {

constexpr size_t kJobIdLen = 32;
constexpr size_t kApprovalIdLen = 32;
// Planner task IDs are Pi-issued (pi-server's planner_tasks.id is a SQLite
// AUTOINCREMENT integer, not a uuid4 hex string like jobId) -- stored
// on-device as that integer's decimal string form, which comfortably fits
// well within 32 chars. Unlike approvalId/completion_id (device-generated
// idempotency keys, created before any network attempt), a task's
// identity always originates on the Pi, the same way jobId does.
constexpr size_t kTaskIdLen = 32;

}  // namespace store
