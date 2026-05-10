#pragma once

// IAuditLog.h — abstract audit log interface (Phase 3 stub; Phase 4 = real).
//
// Phase 3 ships StubAuditLog (logs to stderr, no persistence).
// Phase 4 swaps in the BLAKE2b-chained writer.

#include "AuditEvent.h"

namespace tf::audit {

class IAuditLog {
public:
    virtual ~IAuditLog() = default;

    // Record an audit event.  Implementations must be thread-safe;
    // handlers run in the cpp-httplib thread pool.
    //
    // GUARDRAIL: implementations MUST NOT log passphrase, totp_code,
    // or raw session token values — sanitize before passing details.
    virtual void record(const AuditEvent& evt) = 0;
};

} // namespace tf::audit
