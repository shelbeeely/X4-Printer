#pragma once
// Shared id-length constants for JobStore.h / ApprovalOutbox.h. Both job_id
// and approval_id are server/device-generated uuid4 hex strings (32 chars,
// no dashes) — see docs/protocol.md.

#include <cstddef>

namespace store {

constexpr size_t kJobIdLen = 32;
constexpr size_t kApprovalIdLen = 32;
// Planner task IDs are device-generated (esp_random()-based, same
// convention as approvalId in ApprovalOutbox.h), not server-issued like
// jobId.
constexpr size_t kTaskIdLen = 32;

}  // namespace store
