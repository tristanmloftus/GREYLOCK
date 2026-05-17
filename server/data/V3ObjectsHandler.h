#pragma once

// V3ObjectsHandler.h — read-only GET routes for v3/v4 typed objects.
//
// Routes registered (all session-authenticated):
//   GET /decisions               — list (newest first)
//   GET /decisions/:id           — one decision
//   GET /relationships           — list (alphabetical)
//   GET /relationships/:id       — one relationship (id OR slug-matched name)
//   GET /targets                 — list (newest updated first)
//   GET /targets/:id             — one target
//   GET /tasks                   — list (open first, then by due_unix)
//   GET /tasks/:id               — one task
//   GET /events                  — list (starts_at_unix DESC)
//   GET /events/:id              — one event
//   GET /notes                   — list (updated_at_unix DESC)
//   GET /notes/:id               — one note (id OR path)
//   GET /real_estate             — list (label ASC)
//   GET /real_estate/:id         — one property (id OR slug-matched label)
//   GET /pending_extractions     — list (pending first)
//   GET /pending_extractions/:id — one extraction
//   GET /reimbursements          — list (pending first, joined with entity names)
//   GET /reimbursements/:id      — one reimbursement
//
// Requires M007 (reimbursements) + M008 (decisions, tasks, events, notes)
// + M009 (targets, relationships, real_estate, pending_extractions)
// migrations.

namespace httplib { class SSLServer; }
class Database;

namespace tf::data {

void register_v3_objects_handlers(httplib::SSLServer& server,
                                   Database& db);

} // namespace tf::data
