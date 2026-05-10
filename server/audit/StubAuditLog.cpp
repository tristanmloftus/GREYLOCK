#include "StubAuditLog.h"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace tf::audit {

void StubAuditLog::record(const AuditEvent& evt) {
    // Format: [AUDIT] ts_ms=<ms> actor=<id>(<kind>) action=<action> outcome=<outcome>
    //                  subject=<id>(<kind>) domain=<domain>
    // details are emitted as compact JSON on the same line.
    //
    // GUARDRAIL: do NOT log passphrase, totp_code, or raw session tokens.
    // The caller is responsible for sanitizing the details field before calling
    // record(); we just print whatever is passed.
    std::string details_str = evt.details.dump();
    std::fprintf(stderr,
        "[AUDIT] ts_ms=%" PRId64 " actor=%s(%s) action=%s outcome=%s "
        "subject=%s(%s) domain=%s details=%s\n",
        evt.ts_ms,
        evt.actor_user_id.c_str(),
        evt.actor_kind.c_str(),
        evt.action.c_str(),
        evt.outcome.c_str(),
        evt.subject_id.c_str(),
        evt.subject_kind.c_str(),
        evt.domain.c_str(),
        details_str.c_str()
    );
}

} // namespace tf::audit
