#pragma once
// Shared "write durably" helper for every SD-backed store (WifiStore,
// JobStore, ApprovalOutbox) — one implementation of the
// write-to-temp-then-rename pattern (docs/architecture.md, modeled on
// CrossPoint's PersistableStoreBase::writeDocToFile) instead of three
// copies of the same footgun-prone file I/O.
//
// Sequence: write the new content to "<path>.tmp"; if <path> already
// exists, remove it; rename "<path>.tmp" -> <path>. A power loss before the
// rename leaves the old file intact (the write only ever touched .tmp) and
// a fresh boot's load() sees the last-good state; a power loss during or
// after the rename leaves the new file intact (SdFat's rename on a FAT
// volume is effectively a single directory-entry update, not a byte-by-byte
// copy). The one narrow gap — power loss in between "remove old" and
// "rename tmp" — loses the file entirely; every caller here treats a
// missing store file as "start empty" (a fresh WifiStore/JobStore/outbox),
// which is the same state a truly brand-new device boots into, so this
// gap degrades to "forget this store's contents," never to a corrupt or
// torn read.

#include <Print.h>
#include <WString.h>

namespace store {

bool writeFileAtomic(const char* path, const String& content);

}  // namespace store
