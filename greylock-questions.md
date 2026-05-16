# Greylock — Open Questions

> Companion to `greylock-spec.md`. Every `[FILL]` in the spec collapses
> to one of the questions below. Each has context, options, Claude's
> recommendation, and a space for the answer.
> Rip through these in one sitting and the spec becomes commitable.

---

## How to use this doc

Each question has four parts:

- **Context** — what's at stake and where it lives in the spec.
- **Options** — the realistic choices.
- **Claude's pick** — the recommendation and why.
- **Decision** — empty field; fill in with `[X]` against an option or
  write a custom answer.

When you answer, copy a one-liner to the Decisions log at the bottom
of the spec.

---

## A. Architecture & infrastructure

### Q1 — Plaid app structure

**Context (spec §1.6, §3, §7.1):** Plaid Limited Production gives 10
items free per app. Each app has its own client_id + secret. Greylock
scopes by `entity_id` internally; the Plaid app identity is
independent.

**Options:**
- (A) One app for everything (10 items shared).
- (B) Two apps — Personal shared + PCC (20 items).
- (C) Three apps — Rory personal + Tristan personal + PCC (30 items).
- (D) Four apps — per user × per domain (40 items, fragments PCC).

**Claude's pick:** **(C)**. PCC is one LLC, its banking should live
under one app owned by PCC, not split per user. Personal is per-user
identity, each principal owns their own. 30 free items is comfortable
headroom; current usage is ~8-9 items total.

**Decision:** ___ C

---

### Q2 — First-run entity seeding

**Context (spec §7.2):** `main.cpp` currently hardcodes "Personal" +
"Business LLC" entities on a fresh install. Collides with the real
multi-entity model.

**Options:**
- (A) Keep the hardcoded seed.
- (B) Remove the seed; first-user goes through an onboarding flow
  that creates one virtual personal entity; subsequent users go
  through invite-only onboarding.

**Claude's pick:** **(B)**. Hardcoded names will be wrong for almost
everyone except the original developer. Onboarding-driven seeding is
the standard pattern.

**Decision:** B

---

### Q3 — Dashboard widget replacement

**Context (spec §5.5, §7.3):** Four shovel-era widgets need replacing:
`ShovelScore`, `ShovelIntelligence`, `Drill_ShovelScore`,
`ConnectionStatus`.

**Claude's proposed starting four:**
1. Net Worth — per-entity breakdown + 90-day sparkline.
2. Cash Flow This Month — income / expenses / net.
3. Recent Activity — last 5 tx across visible entities.
4. Sync & Alerts — per-item sync status + active alerts.

Add in v3+: Upcoming Tasks, Open Decisions awaiting outcome, Pipeline
summary, Overdue Relationships.

**Decision (confirm or override):** confirm

---

### Q4 — Naming

**Context (spec header, §7.4):** "TerminalFinance" was the v1 framing
and is too narrow. Codename has been "Greylock."

**Options:**
- (A) Keep "Greylock" as the product name.
- (B) New name (specify).
- (C) Stay codenamed; never productize the name.

**Claude's pick:** **(A) or (C)**. Slight lean (A) — having a name
makes the project feel real and committable. But (C) is fine; PCC
doesn't need this productized.

**Decision:** Greylock

---

### Q5 — Single vs dual-source ledger

**Context (spec §7.5):** Once Greylock is PCC's general ledger (v2),
do we also maintain an external accounting system (QuickBooks, Wave)
for the accountant?

**Options:**
- (A) Single source in Greylock; export-on-demand for the accountant.
- (B) Dual-write to QuickBooks; Greylock is the operating view,
  QuickBooks is the books-of-record.

**Claude's pick:** **(A)**. Reconciliation tax of dual-write is real
and bites monthly. Single source + clean exports is the cleaner
path. Worth committing now even though it's a v2 question.

**Decision:** single-source, build a quicksbook inside Greylock

---

### Q6 — Obsidian as default editor

**Context (spec §4, §7.6):** Vault format is portable markdown +
frontmatter + wikilinks. Any editor works. But Obsidian gives graph
view, backlinks, daily notes — meaningful UX wins.

**Options:**
- (A) Standardize on Obsidian as the recommended editor; format stays
  portable so vim/VS Code still work.
- (B) Editor-agnostic; no recommendation.

**Claude's pick:** **(A)**. Obsidian's backlinks + graph view make
the vault genuinely useful as a thinking tool. Free for personal use.
File format stays portable so this is recommendation, not lock-in.

**Decision:** A

---

### Q7 — Git remote choice

**Context (spec §4.3, §7.7):** Server hosts the canonical vault repo.

**Options:**
- (A) Plain ssh-git with post-receive hooks. Simplest. No web UI.
- (B) Gitea / Forgejo. Self-hosted GitHub clone with web UI.
- (C) Sourcehut self-hosted. Minimalist.
- (D) Private GitHub repo. Convenient but breaks the "no SaaS for
  sensitive data" principle.

**Claude's pick:** **(A)** for v3 ingestion. Add (B) only if/when a
web UI for diff review becomes useful (probably never; the TUI shows
diffs natively).

**Decision:** A

---

### Q8 — Local embedding model

**Context (spec §4.4, §7.8):** Open-source sentence-transformers-class
model running on Tristan's Mac via MLX. Benchmark on a sample of
vault content before locking in.

**Options to evaluate (concrete candidates):**
- BGE-large or BGE-M3 (general-purpose, strong English retrieval).
- Nomic Embed v1.5 (long context, well-tested).
- GTE-large (alternative, similar tier).
- Whatever Anthropic ships an open-weights version of (if/when).

**Claude's pick:** **Benchmark BGE-M3 first.** Run a small eval on 50
hand-picked retrieval pairs from your existing notes (once you have
notes); whichever wins, lock it in. Switching later is cheap (just
re-embed everything).

**Decision:** Hold off on this

---

### Q9 — Embedding storage location

**Context (spec §2.6, §7.9):** Where do embedding vectors live?

**Options:**
- (A) In the main SQLCipher DB as BLOB columns. Simplest, one DB to
  back up.
- (B) Separate sqlite-vec / similar vector store. Faster at scale.

**Claude's pick:** **(A) through v5**. At our data size (years of
vault + tens of thousands of tx + thousands of objects), the main DB
is fine. Revisit at v6 if retrieval latency becomes annoying.

**Decision:** A

---

### Q10 — API privacy tiering & provider terms

**Context (spec §4.4, §7.10):** Three tiers proposed: A (ask freely),
B (ask with redaction), C (local only). Which scopes go where?

**Proposed assignment:**

| Scope | Tier | Rationale |
|-------|------|-----------|
| PCC strategy notes, deal memos, target analysis | A | Strategic value of frontier reasoning outweighs sensitivity |
| Public research, drafting, summarization | A | Low sensitivity |
| Cash-flow analysis, P&L questions | B | Redact account numbers + counterparty names |
| Specific Transaction-level queries | B | Numbers go but identifiers don't |
| Raw account balances, mask-revealed | C | Never to API |
| Personal-scope vault notes (rory/ or tristan/) | **[FILL — your call]** | Most sensitive corpus per principal |
| Reimbursements + cross-entity flagging | B | |

**Also [FILL]:** Lock in writing that Anthropic API + OpenAI API
developer terms don't train on submitted data. Both currently default
to no-training-on-API-data but confirm the agreement type and
opt-out status before going live.

**Decision (per scope):** hold off on this

---

### Q11 — Backup target

**Context (spec §5.4, §7.11):** Where does the encrypted nightly
snapshot go?

**Options:**
- (A) Tailnet-reachable NAS at Rory's place or Tristan's parents'
  house.
- (B) Encrypted backup to S3/B2/Backblaze with client-side encryption
  (server holds no key).
- (C) Both — primary tailnet mirror + offsite encrypted cloud.

**Claude's pick:** **(C)**. Tailnet mirror for fast recovery; cloud
for catastrophic loss (house fire, hardware theft). Cloud cost at
your data sizes is <$5/mo.

**Decision:** A

---

### Q12 — Server migration path off the Mac

**Context (spec §5.1, §7.12):** Tristan's Mac is fine for v1-v3 but
its mobility limits uptime. When/where to migrate?

**Options:**
- (A) Stay on Tristan's Mac through v5; migrate only when uptime pain
  becomes acute.
- (B) Mac mini at home base ($600), tailnet-joined, always on. Move
  when v4 ships.
- (C) Intel NUC or similar small Linux box. Off macOS, more
  flexibility on RAM/disk.

**Claude's pick:** **(A) → (B) when v4 ships.** Mac mini is the
cheapest path to 24/7 with zero new OS to learn. (C) is the right
answer if/when you want to run a heavier local model later.

**Decision:** tristans mac for now raspberry pi5 with 16gb of ram running local llm and server in parallel with mac essentiall combininb ght two.

---

## B. Ontology & data model

### Q13 — First non-finance object type to wire

**Context (spec §2.1, conversation):** Which object type goes in
*first* after the v1-v2 finance set, and what does that say about
what Greylock is becoming?

**Options (in roughly v3-v5 priority order):**
- (A) Decision. Backbone of "where we were / are / going."
- (B) Note (vault ingestion). Makes everything else possible.
- (C) Task. Operational; quick win.
- (D) Target. PCC pipeline value, but waits on (B).
- (E) Relationship. The object type that turns Greylock from a
  business system into a life system.
- (F) Event. Foundation for meeting-driven workflows.

**Claude's pick:** **(B) Note first** (it's the input pipe; nothing
downstream works without it), **(A) Decision second** (highest
strategic value once the vault ingests), **(E) Relationship third**
(it's the object type that signals you're building a life system, not
just a business tool).

**The deeper question:** which of these you prioritize tells us what
Greylock is to you. If Relationship comes before Target, this is a
life system that happens to do PCC. If Target comes first, it's a
PCC system that happens to know your personal life. Both are
legitimate. Worth deciding deliberately.

**Decision:** B

---

### Q14 — Top-level navigation keymap

**Context (spec §7.1, §8.1):** Tab currently does two things (widget
focus on Dashboard, view-switch elsewhere). Discoverability problem.

**Claude's proposal:** vim-style `g` + letter for top-level:
- `g d` Dashboard, `g a` Accounts, `g t` Transactions,
  `g c` Categories, `g b` Budget, `g k` Tasks, `g D` Decisions,
  `g e` Events, `g n` Notes, `g p` Proposals, `g T` Targets,
  `g R` Relationships, `g r` Real Estate, `g f` Forecasts.
- Tab / Shift-Tab = next/prev focus within current view.
- `:` palette stays as fallback.
- `1`-`9` switch entity.

**Decision (adopt or override; list any letter-collisions to
resolve):** adopt

---

### Q15 — Entity list at launch

**Context (spec §8.2):** What entities seed when you and Tristan come
online for real?

**Claude's proposal:**
- `#me-rory` Rory (virtual personal)
- `#me-tristan` Tristan (virtual personal)
- `#pcc` Platinum Creek Capital LLC (Delaware, shared)

Future (not at launch, when needed):
- `#pcc-subco-1` first acquisition's SubCo LLC
- `#re-rory-ga` Rory's rental holding LLC if/when GA properties
  move into one
- `#re-tristan-...` Tristan's real estate vehicle if applicable

**Decision (additions, corrections, renames):** claude proposal

---

## C. Vault & ingestion UX

### Q16 — Vault auto-commit vs manual commit

**Context (spec §8.13):** When the principal saves a markdown file,
does the TUI auto-commit and push, or wait for explicit
`:vault commit "<msg>"`?

**Options:**
- (A) Auto-commit on every save, batched (e.g. every 60s or on
  TUI-exit). Best UX, no forgotten work.
- (B) Manual commit. Principals write meaningful commit messages
  themselves.
- (C) Hybrid: auto-commit drafts to a personal branch
  (`rory/wip`); manual commit-and-merge to `main` when ready.

**Claude's pick:** **(C)**. Best of both. The personal branch
auto-saves so nothing is ever lost; the `main` branch only gets
meaningful, message-attributed commits that the ingestion pipeline
processes.

**Decision:** c

---

### Q17 — Proposals inbox UX

**Context (spec §8.14):** When the model proposes typed objects from
a note, how does the principal review and apply them?

**Claude's proposed UX:**
- `g p` opens an inbox table sorted by source note's update time.
- Columns: Source Note, Proposed Type, Title/Summary, Created.
- `Enter` opens detail showing the source note excerpt + the proposed
  object payload.
- Actions: `a` apply as-is, `e` edit then apply, `r` reject with
  optional reason, `s` snooze.
- Bulk: `v` visual select, `a` apply selected.
- "Inbox zero" empty state when caught up.

**Decision (confirm or override):** confirm

---

### Q18 — Cross-principal commentary on each other's notes

**Context (spec §10 parking lot):** Tristan reads Rory's deal memo
and has thoughts. How does that interaction work?

**Options:**
- (A) Comment threads on notes (Obsidian-style). More plumbing.
- (B) Reply-notes that wikilink back to the original. Same primitive
  as everything else.
- (C) Just leave it to Slack/text; Greylock doesn't model
  conversation.

**Claude's pick:** **(B)**. Keeps the primitive count low. Reply-notes
get full search, full embedding, full ontology. Comment-on-note as a
separate primitive is the kind of feature that grows tendrils into
every other surface.

**Decision:** b

---

## D. Decisions log

When you answer any of the above, copy a one-liner to the spec's
§11 Decisions log with date + decision + who made the call. Keeps the
audit trail clean.

| Date | Question | Decision | Made by |
|------|----------|----------|---------|
|      |          |          |         |
|      |          |          |         |
|      |          |          |         |

---

## E. Questions for Rory specifically (no recommendation)

These aren't architecture; they're framing questions where Claude
shouldn't have a default opinion.

### Q19 — What's the first object type you'd add that isn't on the list?

The current ontology covers finance, deals, decisions, people, events,
notes, references. Tasks. Relationships.

What's the next one you'd add — the one that isn't here yet but
that you know matters? (Health? Reading? Workouts? Locations? A
specific PCC-internal object type? Something else?)

Whatever you put here tells us what Greylock is becoming for you.

**Answer:** the ontology should figure that out

---

### Q20 — What's the minimum-viable v4 (intelligence layer Phase 1) you'd actually use?

The full Phase 1 catalog from §4.5 is generous. The honest version:
which 2-3 things in that list would you use *every day* if they
shipped tomorrow? Build those first; the rest can wait.

**Answer:** all of it

---

### Q21 — How do you want to handle Tristan's MBA-disrupted server uptime?

Honest framing: if Tristan's Mac is the server and Tristan is at
school, on the move, lid closed — the server is offline for stretches.
What's your tolerance, and which migration trigger do you want?

- "Mac is fine for a year, we'll deal" → stay on the Mac.
- "I want to be able to use Greylock from anywhere any time" → buy a
  Mac mini now.
- "Different answer" → spell it out.

**Answer:** were in development, ultimately we will have a 24/7 server with backups and redundancy in place. 


s
