#pragma once

// StubAuditLog.h — no-op audit log for Phase 3.
//
// record() logs the event to stderr in a structured format and returns.
// No persistence.  Phase 4 replaces this with the BLAKE2b-chained writer.

#include "IAuditLog.h"

namespace tf::audit {

class StubAuditLog : public IAuditLog {
public:
    StubAuditLog() = default;
    ~StubAuditLog() override = default;

    // Thread-safe: uses stderr (FILE* writes are atomic enough for a dev stub).
    void record(const AuditEvent& evt) override;
};

} // namespace tf::audit
