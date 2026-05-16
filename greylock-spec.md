# Greylock — System Spec

> **Codename:** Greylock (working title; supersedes "TerminalFinance").
> **Owners:** Rory (vision, UX, ontology design) · Tristan (engineering,
> data, infrastructure).
> **Status:** v1 banking surface in flight (135 real txns flowing, balances
> bug pending). Ontology + vault + workflow + intelligence layers in design.
> **Audience:** Claude Code (implementation), the two principals, and any
> future engineering hire who needs to ramp in one read.
> Open questions consolidated in `greylock-questions.md`.

---

## 0. The vision in one paragraph

Greylock is a **private, household-scope application for the
Loftus principals (Rory + Tristan) and the entities they own or control
(Platinum Creek Capital and any future SPVs, trusts, rental LLCs). It is
the internal operating substrate of PCC and the principals' personal
lives — the system of record, the system of action, and the system of
memory.

**The ontology is the foundation.** Every person, entity, account,
transaction, asset, liability, contract, target, document, decision,
task, relationship, and event is a typed object on a single graph,
bitemporal and lineage-tracked. The other three pillars exist to
populate, mutate, or read from that graph: the **Vault** is the
human-writable input pipe (Obsidian-style markdown that gets distilled
into typed objects), the **Workflow Engine** is the mutation surface
(every change is a typed, audited Action), and the **Intelligence
Layer** is the reasoning loop (local retrieval over the graph and the
vault, frontier LLM for generation, every output traceable to source).

All of it runs on Tristan's mac on a Tailscale-only tailnet. The
ontology compounds: each new node creates potential edges to every
existing node, and the system's usefulness grows roughly with the
square of what's modeled. v1 ships the finance slice. v2-v∞ extend
across acquisitions, decisions, real estate, relationships, forecasts,
and the intelligence layer.

The shorthand: *Palantir for the graph, Obsidian for input, ServiceNow
for write, frontier LLMs for reasoning — all of it private, all of it
ours, JARVIS not Ultron.*

---

## 1. Mental model — one foundation, three services

### 1.1 The Ontology (the foundation)

The ontology is the typed graph that makes Greylock distinctively
**yours**. The other three pillars are commodity infrastructure —
markdown storage, mutation logging, RAG over a corpus — that anyone
could build. The ontology is the moat because it's the typed model of
*your specific life, your specific entities, your specific decisions,
your specific relationships, your specific deal pipeline*. Every node
you add deepens it. Every edge multiplies the queryable surface.

It owns:

- **Typed objects** with declared schemas (full catalog in §2.1).
- **Typed relationships** between objects, bitemporal.
- **Bitemporal storage** — valid time (when the fact was true) and
  system time (when we learned it). Enables "what did we believe our
  net worth was on 2026-03-01?" queries.
- **Lineage** — every derived value records its sources. Nothing is
  a black box.
- **Functions** — derived properties computed on demand
  (`Account.balance_as_of(date)`, `Entity.cash_position()`,
  `Target.fit_score()`, `Relationship.last_meaningful_contact()`).
- **Access control** — per-object, per-property, per-user.

Critical principle: **the ontology never mutates from inference
alone**. The intelligence layer can propose new objects, new edges,
new properties — but every change goes through the workflow engine
as an explicit Action that a principal applied. This protects the
graph from silent corruption (§2.7).

### 1.2 The Vault (the input pipe to the ontology)

The vault is an Obsidian-compatible markdown directory under git,
hosted on Tristan's mac. Every word the principals type that isn't a
keystroke into a TUI form lands here.

Why a vault as a separate pillar instead of just typing into ontology
forms:

- **Prose is how humans think.** You don't think in `Decision`
  objects; you think in sentences. The vault captures the *thinking*;
  the ontology captures the *outcome*.
- **The vault is the corpus.** Years of daily logs, meeting notes,
  research, half-formed ideas — this is what makes the intelligence
  layer functionally "know you."
- **Portability.** If Greylock dies tomorrow, the vault is still a
  directory of markdown files. The DB is the derivative; the vault
  is the source.

The vault feeds the ontology via the ingestion pipeline (§4.2). Every
markdown save → git commit → server-side hook fires `IngestNote` →
the model proposes typed objects extracted from the prose → the
principal confirms in the TUI → the ontology gets the new objects.

Full vault spec in §4.

### 1.3 The Workflow Engine (the write surface for the ontology)

The workflow engine is the **only legal way to mutate the ontology**.
Direct table writes are forbidden by convention.

- **Actions** — typed, audited, versioned mutations (`CreateTarget`,
  `LinkAccount`, `RecategorizeTransaction`, `LogDecision`,
  `IngestNote`, `ConfirmExtraction`). Every Action is
  user-attributed, timestamped, and reversible where possible.
- **Automations** — *event → guard → action* rules. Examples: tx
  matches merchant pattern → recategorize; PCC operating balance
  drops below threshold → fire Alert.
- **Tasks** — work items with assignee, due date, status.
- **Alerts** — surfaced in status bar, dedicated tab, severity-tiered.
- **Recurring jobs** — Plaid syncs, balance snapshots, monthly close,
  reimbursement chase, forecast recompute, vault re-index,
  embedding refresh.

### 1.4 The Intelligence Layer (the reasoning loop)

The intelligence layer reads from the ontology (structured), the
vault (semantic), and the action log (behavioral, temporal), and
proposes back through the workflow engine. It is RAG, not a custom-
trained model:

- **Locally**: embeddings, retrieval, ontology queries, context
  assembly. Runs on Tristan's mac. Fast, private.
- **Frontier APIs (Anthropic, OpenAI)**: the reasoning step. Given
  the retrieved context + the question, the API model generates the
  answer. Subject to a redaction layer and a privacy tiering policy
  (§4.4) — not everything goes to the API, and what does is
  scrubbed of identifiers when scope requires.

What it does:

- **Ambient assistance** — categorization suggestions in
  Transactions, "duplicate Target detected", "this Decision
  contradicts one from six months ago."
- **Conversational** — `:ask <question>` palette command. Answers
  with citations to source notes / objects / transactions.
- **Extraction** — proposes typed ontology objects from vault notes.
- **Pattern surfacing** — semantic queries over the principal's
  entire history.

What it is not — and this is structural, not rhetorical:

- It is not sentient and does not become sentient with more data.
- It does not autonomously mutate ontology state. Every output is a
  proposal; a principal applies it.
- It is not authoritative without lineage. Every answer cites its
  sources; if it can't cite, it doesn't claim.

The mental model is **JARVIS, not Ultron**. JARVIS is Tony Stark's
extended cognition — sees what he can't hold in his head, runs
calculations he can't do in real time, surfaces what he forgot,
drafts what he then approves. Tony stays the operator, the decider,
the genius. JARVIS multiplies him. That is what Greylock + frontier
LLMs is. Ultron is autonomous and aligned-wrong; the architecture
above forecloses that mode by design.

### 1.5 Why a TUI

- **Security choice.** No public web surface means no DNS, no TLS
  cert visible on the open internet, no browser threat model.
- The backend lives on a Tailscale tailnet. The TUI is the only
  authorized client.
- **Speed.** Keyboard-first, no mouse, instant.
- **Cost**: discoverability is worse than a GUI. Mitigated via
  command palette (`:`), help overlay (`?`), and an opinionated
  default keymap (§8).

### 1.6 Why self-hosted on Tristan's mac

- **Data sensitivity.** PCC's books, the acquisition pipeline,
  years of personal writing. No SaaS gets a copy.
- **No vendor risk** for the substrate. Anthropic and OpenAI are
  vendors for the *reasoning step* only; the corpus stays local.
- **Composability.** The ontology is ours to extend.
- **The mac is the trust anchor.** Backend, DB, git remote, vault,
  doc blob store, embedding model, retrieval — all on one box,
  tailnet in, tailnet out. See §5 for full architecture and
  migration path off the mac when uptime demands it.

---

## 2. The ontology

### 2.1 Object types

Each object has a stable string `id`, a `type`, a `created_at`,
`updated_at`, `created_by`, and `valid_from` / `valid_to` for
bitemporal facts. Properties below are the **core** set — extensions
welcome.

#### Person
The two principals plus relevant counterparties (Cade, future
employees, attorneys, accountants, target-company sellers, family).
- `name`, `display_name`, `email[]`, `phone[]`, `role` (principal,
  counterparty, vendor, advisor, family, other), `notes`.

#### Entity
A legal person — PCC, future SPVs, the GA rental LLCs if any, any
trusts. Personal "entities" exist as virtual containers (`#me-rory`,
`#me-tristan`) so the ontology is uniform.
- `name`, `kind` (llc, c-corp, s-corp, trust, person-virtual, other),
  `domicile`, `ein`, `formation_date`, `status`, `notes`.

#### Membership
A `Person` ↔ `Entity` edge with role + economic share.
- `person_id`, `entity_id`, `role`, `equity_pct`, `voting_pct`,
  `effective_from`, `effective_to`.

#### Relationship
A richer model of a Person-to-Person or Person-to-Entity connection
over time. This is the object type that turns Greylock from a
business system into a life system. Without it, you have a list of
people. With it, you have a queryable model of who matters, why,
and what state your relationships are in.
- `from_person_id`, `to_person_or_entity_id`,
- `kind` (family, friend, mentor, advisor, peer, employee, vendor,
  counterparty, romantic, acquaintance, other),
- `strength` (computed function: recency × frequency × significance),
- `last_contacted_at`,
- `last_meaningful_interaction_at` (manually flagged, not every text),
- `cadence_target` (target check-in frequency in days; nullable),
- `what_theyre_working_on` (free text, updated as you learn),
- `what_i_owe_them` (open commitments: intros, follow-ups, reads),
- `what_they_owe_me` (the inverse),
- `notes`.

The strength function + `cadence_target` + `last_contacted_at`
together let the intelligence layer surface things like "you said
you'd check in with Cade weekly; it's been 23 days" and
"you haven't had a meaningful interaction with [advisor] in 6
months and they're on your board."

#### Account
A financial account. Bank, credit card, brokerage, loan, retirement,
crypto custody.
- `entity_id`, `institution`, `nickname`, `account_type` (checking,
  savings, credit, brokerage, ira, 401k, loan, crypto, other),
  `mask` (last-4), `currency`, `plaid_item_id` (nullable),
  `status`, `created_at`.

#### AccountBalance
A bitemporal balance snapshot. Many per account, one per sync.
- `account_id`, `balance_cents`, `available_cents`, `as_of`,
  `recorded_at`, `source` (plaid, manual, computed).

#### Transaction
A money movement. 4-axis attribution (entity, account, category,
merchant).
- `account_id`, `posted_at`, `authorized_at`, `amount_cents`,
  `currency`, `merchant_id`, `category_id`, `description`,
  `external_id`, `pending`, `notes`, `reimbursement_id`,
  `split_parent_id`, `entity_id`.

#### Merchant
- `canonical_name`, `aliases[]`, `default_category_id`, `mcc`,
  `tags[]`, `notes`.

#### Category
Hierarchical. Two levels (Group → Category).
- `parent_id`, `name`, `kind` (income, expense, transfer, investment),
  `entity_scope`, `default_for_merchant_pattern`.

#### Asset
Anything we own that isn't a financial Account.
- `entity_id`, `kind` (real_estate, vehicle, equipment,
  private_equity, collectible, other), `name`, `acquired_at`,
  `cost_basis_cents`, `current_value_cents`, `valuation_source`,
  `notes`.

#### Liability
- `entity_id`, `kind` (mortgage, va_loan, auto_loan, commercial,
  credit_line, intercompany, other), `principal_cents`, `rate_pct`,
  `term_months`, `payment_cents`, `origination_date`,
  `payoff_curve_json`.

#### Document
- `entity_id`, `kind`, `title`, `executed_on`, `parties[]`,
  `storage_uri`, `hash_sha256`, `tags[]`, `notes`.

#### Target *(PCC acquisition pipeline)*
- `company_name`, `category` (one of the six PCC categories),
  `geography`, `revenue_est_cents`, `ebitda_est_cents`,
  `multiple_target`, `owner_age_est`, `successor_status`, `stage`
  (sourced, contacted, ldi, loi, dd, closed, dead), `fit_score`,
  `last_touch_at`, `next_action`, `owner_principal_id`, `source`,
  `notes`.

#### Decision
First-class object. **The backbone of "where we were, where we are,
where we are going."**
- `title`, `decided_at`, `decider_id[]`, `category`, `entity_scope`,
  `rationale`, `alternatives_considered`, `expected_outcome`,
  `confidence_pct`, `linked_object_ids[]`, `outcome_logged_at`,
  `outcome_text`, `outcome_score` (−2..+2), `source_note_id`.

#### Note
The vault's representation in the ontology.
- `path`, `title`, `kind` (daily, meeting, decision_draft, research,
  target_brief, person_brief, deal_memo, journal, reference, other),
  `created_at`, `updated_at`, `author_id`, `entity_scope`, `tags[]`,
  `wikilinks[]`, `mentioned_object_ids[]`, `commit_sha`,
  `word_count`, `last_indexed_at`, `embedding_id`,
  `body_hash_sha256`.

#### Task
- `title`, `description`, `assignee_id`, `due_at`, `status`,
  `priority`, `parent_id`, `created_by`, `completed_at`,
  `source_note_id`.

#### Event
A point-in-time happening.
- `kind` (meeting, call, site_visit, board, milestone, other),
  `occurred_at`, `duration_min`, `attendee_ids[]`,
  `linked_object_ids[]`, `summary`, `notes`, `transcript_uri`,
  `source_note_id`.

#### Reimbursement
- `from_entity_id`, `to_entity_id`, `transaction_id`, `amount_cents`,
  `status`, `created_at`, `resolved_at`, `resolution_tx_id`, `notes`.

#### Reference *(external information, scoped through the graph)*
External information enters the graph **only when it touches an
existing node**. A news article about copper M&A isn't ingested in
the abstract; it's a `Reference` linked to the specific Targets it
names.
- `kind` (article, filing, social_post, paper, podcast, other),
  `title`, `source_url`, `author`, `published_at`, `retrieved_at`,
  `excerpt`, `linked_object_ids[]` (what in the graph this is
  about), `principal_note` (why you saved it), `tags[]`.

#### Forecast *(v6)*
- `kind`, `entity_scope`, `horizon_months`, `assumptions_json`,
  `created_at`, `series_json`, `superseded_by`.

### 2.2 Relationships (edge types)

Every edge is bitemporal.

| Edge | From → To | Notes |
|------|-----------|-------|
| `owns` | Person → Entity | Equity stake |
| `controls` | Entity → Entity | Holdco → subsidiary |
| `member_of` | Person → Entity | Generalized membership |
| `connected_to` | Person → Person/Entity | A Relationship object |
| `holds_account` | Entity → Account | Legal owner |
| `posted_to` | Transaction → Account | |
| `categorized_as` | Transaction → Category | Editable, versioned |
| `at_merchant` | Transaction → Merchant | |
| `documented_by` | any → Document | |
| `signed_by` | Document → Person | |
| `targeted_by` | Target → Person | Who runs the relationship |
| `decided_by` | Decision → Person | Who made the call |
| `references` | Decision/Note → any | What it was about |
| `assigned_to` | Task → Person | |
| `attends` | Person → Event | Many-to-many |
| `discusses` | Event/Note → any | |
| `reimburses` | Reimbursement → Transaction | |
| `secures` | Asset → Liability | Collateral |
| `cites` | Reference → any | External info → graph node |
| `derived_from` | any → any | Lineage; auto-populated |
| `extracted_from` | any → Note | What the vault produced |
| `embedded_as` | Note/object → Embedding | RAG lineage |
| `proposed_by` | any → Inference | Provenance for AI-proposed objects |

### 2.3 Bitemporal model

Two time axes, always:

- **Valid time** (`valid_from`, `valid_to`) — when the fact is/was
  true in the real world.
- **System time** (`recorded_at`, `superseded_at`) — when we knew it.

For Notes specifically, system time also corresponds to the git
commit log — every save is a commit. So Note bitemporality gets a
free third dimension: `commit_sha` gives byte-exact recovery of any
prior state.

### 2.4 Lineage & provenance

Every derived value emits a `derived_from` edge set to its inputs.
Drilling into any number must always be able to answer "where did
this come from?"

Sources:
- `source = plaid` + `external_id`.
- `source = manual` + actor.
- `source = vault` + `source_note_id` + `commit_sha`.
- `source = intelligence` + model id + prompt hash + retrieved refs.

### 2.5 Access control

1. **Authentication.** Passphrase + TOTP, 8h session, token in OS
   keystore.
2. **Entity membership.** Determines which entities you can see.
3. **Property-level overrides.** Default: none.

For the vault and intelligence layer:
- Each Note has an `entity_scope`. Personal = author-only.
  PCC = all PCC members.
- Intelligence retrieval is scoped to what the asker can see.
- Embeddings are partitioned by scope at the storage layer.

### 2.6 Schema sketch (SQLCipher DDL fragments)

Illustrative; Tristan flesh out.

```sql
CREATE TABLE objects (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  created_by TEXT NOT NULL REFERENCES objects(id),
  deleted_at INTEGER
);

CREATE TABLE entity_props (
  id TEXT NOT NULL REFERENCES objects(id),
  name TEXT NOT NULL,
  kind TEXT NOT NULL,
  domicile TEXT,
  ein TEXT,
  formation_date INTEGER,
  status TEXT NOT NULL,
  valid_from INTEGER NOT NULL,
  valid_to INTEGER,
  recorded_at INTEGER NOT NULL,
  superseded_at INTEGER,
  recorded_by TEXT NOT NULL REFERENCES objects(id),
  PRIMARY KEY (id, recorded_at)
);

CREATE TABLE edges (
  id TEXT PRIMARY KEY,
  from_id TEXT NOT NULL REFERENCES objects(id),
  to_id TEXT NOT NULL REFERENCES objects(id),
  edge_type TEXT NOT NULL,
  props_json TEXT,
  valid_from INTEGER NOT NULL,
  valid_to INTEGER,
  recorded_at INTEGER NOT NULL,
  superseded_at INTEGER,
  recorded_by TEXT NOT NULL REFERENCES objects(id)
);

CREATE INDEX idx_edges_from ON edges(from_id, edge_type)
  WHERE superseded_at IS NULL;
CREATE INDEX idx_edges_to ON edges(to_id, edge_type)
  WHERE superseded_at IS NULL;

CREATE TABLE actions (
  id TEXT PRIMARY KEY,
  action_type TEXT NOT NULL,
  actor_id TEXT NOT NULL REFERENCES objects(id),
  payload_json TEXT NOT NULL,
  result_json TEXT,
  status TEXT NOT NULL,
  applied_at INTEGER,
  rolled_back_at INTEGER,
  proposed_by_inference_id TEXT REFERENCES inferences(id)
);

-- Note metadata (body stays in markdown file on disk)
CREATE TABLE notes (
  id TEXT PRIMARY KEY REFERENCES objects(id),
  path TEXT UNIQUE NOT NULL,
  title TEXT,
  kind TEXT NOT NULL,
  author_id TEXT NOT NULL REFERENCES objects(id),
  entity_scope TEXT NOT NULL,
  body_hash_sha256 TEXT NOT NULL,
  commit_sha TEXT NOT NULL,
  word_count INTEGER,
  last_indexed_at INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE note_tags (
  note_id TEXT NOT NULL REFERENCES notes(id),
  tag TEXT NOT NULL,
  PRIMARY KEY (note_id, tag)
);

CREATE TABLE note_links (
  note_id TEXT NOT NULL REFERENCES notes(id),
  to_object_id TEXT REFERENCES objects(id),
  raw_target TEXT NOT NULL,
  PRIMARY KEY (note_id, raw_target)
);

-- Intelligence layer
CREATE TABLE embeddings (
  id TEXT PRIMARY KEY,
  source_kind TEXT NOT NULL,
  source_id TEXT NOT NULL,
  chunk_index INTEGER NOT NULL,
  chunk_text TEXT NOT NULL,
  vector BLOB NOT NULL,
  model_id TEXT NOT NULL,
  entity_scope TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE INDEX idx_embeddings_scope ON embeddings(entity_scope, source_kind);

CREATE TABLE inferences (
  id TEXT PRIMARY KEY,
  asker_id TEXT NOT NULL REFERENCES objects(id),
  prompt TEXT NOT NULL,
  prompt_hash TEXT NOT NULL,
  retrieved_refs_json TEXT NOT NULL,
  model_id TEXT NOT NULL,
  model_location TEXT NOT NULL,        -- 'local' | 'api:anthropic' | 'api:openai'
  redaction_tier TEXT NOT NULL,        -- 'none' | 'masked' | 'local_only'
  response TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  feedback_score INTEGER
);

-- AI-proposed objects that haven't been confirmed yet
CREATE TABLE pending_extractions (
  id TEXT PRIMARY KEY,
  source_note_id TEXT NOT NULL REFERENCES notes(id),
  inference_id TEXT NOT NULL REFERENCES inferences(id),
  proposed_type TEXT NOT NULL,
  proposed_payload_json TEXT NOT NULL,
  status TEXT NOT NULL,                -- 'pending' | 'confirmed' | 'rejected' | 'edited'
  resolved_by TEXT REFERENCES objects(id),
  resolved_at INTEGER,
  resulting_object_id TEXT REFERENCES objects(id)
);
```

### 2.7 The self-learning loop

The ontology gets denser, smarter, and more useful over time through
**three distinct mechanisms** — none of which are "the model trains
itself." Calling that out explicitly because the alternative (silent
autonomous mutation) is how every serious ontology system gets
poisoned.

#### Mechanism 1: Graph densification (humans-in-the-loop)

Every new node creates potential edges to every existing node. When
a Note is ingested, the model proposes typed objects extracted from
its prose. When a Transaction posts, the model proposes a category
based on history. When a Target is added, the model proposes
similar Targets it's seen, or relevant Documents and References.

**These are proposals, not commits.** The TUI surfaces them in an
inbox (`:proposals` or `g p` — [FILL] §8.13). The principal
confirms, edits, or rejects. The confirmation itself is an Action,
logged. The graph grows; the principal stays in the loop.

This is the standard pattern across every production ontology
system, including Palantir's. The model is the eyes; the human is
the trigger.

#### Mechanism 2: Extraction model improves with feedback

Every confirmation, edit, and rejection of a proposed extraction is
logged in `pending_extractions`. Over time:

- Confirmed extractions become high-quality few-shot examples
  included in subsequent extraction prompts.
- Edits become signal: "the model proposed kind=meeting; the
  principal changed it to kind=call." Future extractions of similar
  notes incorporate this.
- Rejections become negative examples ("don't propose this kind of
  object from this kind of note").

This is in-context learning, not training. No model fine-tuning is
required for it to work. (Fine-tuning a local model on confirmation
history is a v6+ option, but optional.)

#### Mechanism 3: Retrieval gets smarter as the corpus grows

The intelligence layer's RAG performance is a function of corpus
size and quality. Every Note you add, every Transaction that posts,
every Decision you log, every Reference you save — all of it
becomes retrievable context for future queries. The same model
answers better questions because it sees more relevant data.

This requires zero engineering. You just keep writing into the
vault and using the system. It compounds automatically.

#### The "world" rule: external info enters through internal nodes

"Linking every relationship of the personal, the business, the
world" — the first two are unambiguously the goal. The third needs
a boundary.

**The world is infinite. The graph is what makes it usable.**
External information enters Greylock only when it touches a node
the graph already cares about. An article about midwest M&A doesn't
get ingested in the abstract; it gets ingested as a `Reference`
linked to the three specific Targets it names, with the relevant
excerpt and source URL, and a note from the principal on why it
matters.

When a new Target is added, ingestion automation *can* (with
consent) go scrape SEC filings, local newspaper archives, LinkedIn
profiles of leadership — and link those as References to that
Target. When a Person is added, the same. But the trigger is
always "we already care about this node"; the system doesn't
maintain a personal copy of the internet.

The rule is **lens, not vacuum**. The graph defines what's
relevant; external ingestion is filtered by graph membership.

---

## 3. The workflow engine

### 3.1 Actions

Every mutation is an `Action`. Direct table writes outside the
Action layer are forbidden.

**v1 (banking):** `LinkAccount`, `SyncAccount`, `UpdateBalance`,
`RecordTransaction`, `RecategorizeTransaction`, `SetMerchant`,
`RenameAccount`, `HideAccount`, `UnlinkAccount`.

**v2 (ledger):** `CreateMerchant`, `MergeMerchants`,
`CreateCategory`, `MoveCategory`, `SplitTransaction`,
`ReassignEntity`, `FlagReimbursable`, `ResolveReimbursement`.

**v3 (workflow + vault):** `LogDecision`, `LogDecisionOutcome`,
`CreateTask`, `AssignTask`, `CompleteTask`, `DropTask`,
`RecordEvent`, `LinkDocument`, `IngestNote(path, commit_sha)`,
`ResolveWikilink`, `ProposeExtractionsFromNote`,
`ConfirmExtraction(extraction_id, edits?)`,
`RejectExtraction(extraction_id, reason?)`.

**v4 (acquisition + intelligence):** `CreateTarget`,
`UpdateTargetStage`, `ScoreTarget`, `RecordTargetTouch`,
`LinkTargetToDecision`, `IndexNote`, `IndexObject`,
`AskIntelligence(prompt, scope, redaction_tier)`,
`RecordFeedback(inference_id, score)`.

**v5+ (relationships + references):** `CreateRelationship`,
`UpdateRelationship`, `LogRelationshipTouch`, `IngestReference`,
`AutoEnrichTarget(target_id)` — opt-in external ingestion.

### 3.2 Automations

Form: *event → guard → action*. Stored as data; v1 hardcoded; v3
user-editable.

Starter set:

- On `RecordTransaction` where merchant matches rule → recategorize.
- On `AccountBalance` where `balance_cents < threshold` and entity
  = PCC, account = checking → alert (warn).
- On `RecordTransaction` where `amount_cents > $10,000` → alert
  (info).
- On `LogDecision` where category = acquisition → create Task
  ("Update Target record").
- On vault commit → `IngestNote` → `IndexNote` → if note kind in
  {decision_draft, meeting, deal_memo, person_brief, target_brief}
  → `ProposeExtractionsFromNote` (creates pending_extractions).
- On `Relationship.last_contacted_at` older than
  `cadence_target` → alert.
- Weekly: `ComputeForecasts(kind=cash_flow, horizon=6)`.
- Daily: re-embed any Note whose `body_hash_sha256` changed.

### 3.3 Tasks, 3.4 Alerts, 3.5 Recurring jobs

Plaid sync every 6h, balance snapshot daily, reimbursement chase
weekly, forecast recompute weekly, vault re-index on every commit,
embedding refresh nightly. Backup model in §5.3.

---

## 4. The Vault and the Intelligence layer

### 4.1 Vault directory structure

Vault lives at `/srv/greylock/vault/` on Tristan's mac, checked out
from `/srv/greylock/vault.git`.

```
vault/
├── daily/
│   ├── 2026-05-16.md
│   └── ...
├── meetings/
│   ├── 2026-05-12-pcc-sync.md
│   └── ...
├── decisions/
│   ├── 2026-05-04-downgrade-services-arm.md
│   └── ...
├── targets/
│   ├── acme-copper.md
│   └── ...
├── people/
│   ├── cade-manasco.md
│   └── ...
├── entities/
│   └── pcc.md
├── research/
│   └── ai-infra-capex-2026.md
├── references/                  # external articles, filings, etc.
│   └── 2026-05-10-wsj-copper-tariffs.md
├── personal/
│   ├── rory/
│   └── tristan/
└── _attachments/
```

Personal subdirectories are access-controlled: only the named
principal's session can read or index them.

### 4.2 Note file format

YAML frontmatter + markdown body.

```markdown
---
id: 01HXYZ...                       # ULID, stable across renames
title: Downgrade PCC services arm to cash-flow operation
kind: decision_draft
author: rory
entity_scope: pcc
created_at: 2026-05-04T14:32:00Z
tags: [strategy, services-arm, thesis]
links:
  - "[[pcc]]"
  - "[[tristan]]"
  - "[[2026-04-19-three-tier-capital]]"
status: confirmed
decided_at: 2026-05-04
deciders: [rory, tristan]
---

# Downgrade PCC services arm to cash-flow operation

After the working session today, the centerpiece moves...
```

Extraction pipeline on save (`IngestNote` action):

1. Parse frontmatter. Validate required fields by `kind`.
2. Compute `body_hash_sha256`.
3. Extract inline `#tag`s and `[[wikilinks]]`.
4. Attempt resolution of each wikilink:
   a. Match a known Note → resolve to that Note.
   b. Match a Person/Entity/Target by name → resolve.
   c. Otherwise → unresolved, surface for disambiguation.
5. For `kind = decision_draft` with `status = confirmed`, create
   or update the corresponding `Decision` object.
6. For `kind = meeting`, propose an `Event` for confirmation.
7. Mark the note for re-embedding.
8. Fire `ProposeExtractionsFromNote` to surface other typed
   objects implied by the prose (Tasks, Relationships, References).

### 4.3 Git as substrate

The vault is a git repo. Every change is a commit.

- **Free bitemporality for notes.** `git checkout <sha> -- path`.
- **Diff-able decisions.** `git log -p` on a decision file shows
  exactly how the thinking evolved.
- **Multi-device editing.** Each principal clones; commits push to
  the mac which is the canonical remote.
- **Disaster recovery.** Second offsite mirror of the bare repo is
  a full backup of the human-written corpus.
- **Audit trail.** Every commit is attributed.

Operational:

- Self-hosted git daemon ([FILL] §7.7 — recommended: plain ssh-git
  + post-receive hook).
- Server-side post-receive hook fires `IngestNote` per changed
  file.
- TUI surfaces "uncommitted changes in vault" in status bar.
- DB is **not** in git (binary, sensitive). Separate encrypted
  backup schedule.
- `main` is canonical; both principals push directly; PR overhead
  not worth it for a two-person team.

### 4.4 The Intelligence layer — architecture

**Local for everything except reasoning. API for reasoning.**

The architecture matches the hardware reality (Tristan's mac, §5).
Apple silicon handles embeddings, retrieval, and orchestration
easily. It does *not* handle frontier-quality LLM inference well —
a 7-14B local model is slow, RAM-hungry, and meaningfully worse in
output than Claude or GPT-4. So the reasoning step calls out to
the API; everything else stays on the mac.

**Component 1: Embeddings (local).**

- Sentence-transformers-class model, MLX-optimized for Apple
  silicon ([FILL] §7.8 — candidates: BGE, Nomic, or similar).
- Embed: Note chunks, significant ontology objects (Decisions,
  Targets, Events, Persons, Entities), Transaction descriptions.
- Re-embed on change; nightly catch-up cron.
- Stored in `embeddings`, partitioned by `entity_scope`.

**Component 2: Retrieval (local).**

Two passes:
- **Structured pass.** Parse the question for entity names, date
  ranges, category names. Pull matching ontology rows.
- **Semantic pass.** Embed the query, top-k against
  scope-allowed embeddings.

Merge, rank, assemble context.

**Component 3: Generation (API, with privacy tiering).**

Three privacy tiers, configurable per scope:

- **Tier A — Ask freely.** Default for low-sensitivity scopes
  (strategy, public research, drafting). Retrieved context goes
  to the API model verbatim. Use for: PCC strategy questions,
  Target analysis, deal memo drafts, research synthesis,
  summarization.
- **Tier B — Ask with redaction.** Retrieved context is passed
  through a redaction layer that masks account numbers, balance
  amounts, counterparty PII, and any property flagged
  `redact_in_api` in the ontology. Use for: financial pattern
  analysis, cash-flow questions, anything that mixes sensitive
  numbers with strategy.
- **Tier C — Local only.** Never goes to the API. Use for: raw
  account data, certain personal scopes, anything the principals
  classify as never-leaves-the-tailnet.

Tier choice:
- v4: per-query confirmation prompt before any API call.
- v5+: per-scope default with override.
- [FILL] §7.10: which scopes are auto-tier-A, which require
  per-query consent.

Model choice:
- Default: Claude (via Anthropic API) for reasoning.
- Optional: GPT (via OpenAI API) — toggleable per query if the
  principal wants a second opinion or one model is down.
- Always: log the model used in `inferences.model_id` for
  reproducibility and comparison.

Privacy contract with the API providers:
- Anthropic API and OpenAI API (developer API, not the consumer
  products) do not train on submitted data by default — relevant
  terms to verify and lock in writing before going live ([FILL]
  §7.10).
- Every API call logged with prompt hash + redacted context + model
  + response.

### 4.5 What the intelligence layer surfaces

**Phase 1 (v3-v4):**
- "What did I commit to in the May 12 PCC sync?" — retrieval over
  meeting notes + linked Decisions.
- "Summarize last week's daily notes" — Tier A.
- "Suggest a category for this transaction" — local embedding
  match against historical categorized tx; no API needed.
- "Propose extractions from this note" — runs after each ingestion.

**Phase 2 (v4-v5):**
- "Show me Targets that look like the ones we passed on" — local
  embedding similarity over Target objects.
- "Draft a deal memo for [[acme-copper]] using the template and
  what we know" — Tier A or B depending on whether financials
  are in scope.
- "Which Relationships are overdue per their cadence_target?" —
  pure structured query, no LLM needed.

**Phase 3 (v6, the second-brain mode):**
- "I keep coming back to defense tech — what's the actual
  through-line in what I've written about it?" — semantic pattern
  surfacing across Notes.
- "When did I last contradict myself on the services arm?" —
  semantic + temporal pattern detection on Decisions.
- "Of the principles I've committed to in the capital framework,
  which have I actually violated in deployed decisions?" —
  cross-reference Decision rationale against Transaction patterns.
- "What relationships am I letting decay?" — Relationship objects
  with stale `last_contacted_at` vs `cadence_target`.

### 4.6 Failure modes to avoid

- **Hallucination presented as fact.** Every output cites sources.
  If it can't cite, it doesn't claim.
- **Auto-action from inference.** Suggestions go through workflow;
  humans apply them. No automation rule ever mutates ontology
  state from inference alone.
- **Cross-scope leakage.** Storage-layer partitioning + retrieval
  filtering on `entity_scope`. Unit tests assert no
  Rory→Tristan or PCC→personal retrieval is ever possible.
- **API exposure of raw data.** Redaction layer between retrieval
  and any API call. Default-deny on anything not explicitly
  classified as redactable.

### 4.7 The right mental model: JARVIS, not Ultron

The system is not sentient and will not become sentient. It is a
retrieval-augmented model running over the principals' corpus. It
has no experiences, no preferences, no continuity of self across
queries.

What it *does* — and this is genuinely valuable, and worth the
build — is **function as the principals' extended cognition**:

- It will have read every word they've written.
- It will know every move they've made financially.
- It will remember every decision they've logged with rationale.
- It will spot patterns in their behavior they can't see in
  themselves.
- It will catch contradictions between what they said in March
  and what they're doing in October.
- It will surface the things they forgot they care about.

The principals stay the operators. They make the decisions, take
the actions, write the values into the corpus, confirm the
extractions, and apply the proposals. The system multiplies them
without replacing them.

**Alignment in this design is not a training problem; it's a
corpus problem.** The system is aligned to the principals because
the principals wrote the corpus. Every value committed in the
vault, every decision logged with rationale, every principle
stated in research — that's the alignment signal. The better the
corpus, the more "them" the system gets. This is why the vault
matters as a separate pillar: it's how the model learns the
principals' values without ever being trained on them.

---

## 5. Server & deployment architecture

### 5.1 The server: Tristan's mac

Tristan's personal Mac (Apple silicon) is the trust anchor and the
v1-v3 deployment target. Everything runs there.

**What runs on the mac:**

- C++ backend (cpp-httplib + TLS, port 8443).
- SQLCipher DB at `/srv/greylock/db/greylock.db`.
- Vault at `/srv/greylock/vault/` (working tree).
- Git remote at `/srv/greylock/vault.git/` (bare repo).
- Document blob store at `/srv/greylock/docs/` (encrypted).
- Embedding model (MLX-optimized, runs in seconds per chunk).
- Retrieval + context assembly.
- Backup agent (encrypted nightly to offsite mirror).

**What does not run on the mac:**

- Frontier LLM inference. That goes to Anthropic and OpenAI APIs
  via tailnet → internet. (Local SLMs as a fallback can run if
  the principals want, but quality vs Claude/GPT-4 makes this
  v6+ optional.)

**Mac-as-server operational concerns:**

- **Prevent sleep when on power.** `caffeinate -d` as a launch
  agent, or System Settings → Lock Screen → "Prevent automatic
  sleeping on power adapter when display is off."
- **LaunchAgent for boot persistence.** `~/Library/LaunchAgents/
  com.greylock.backend.plist` runs the backend at login;
  `KeepAlive` ensures restart on crash.
- **Tristan's mac is mobile.** When the lid closes or he's away
  from his network, the server is unreachable. v1-v3 the team
  lives with that (both principals are on the road in compatible
  windows, MBA program is local). v4+ when uptime starts to
  matter, migrate to:
  - Option A: a $600 Mac mini sitting at home base, tailnet-
    joined, always on. Cheapest path.
  - Option B: a small Linux server (Intel NUC or similar) doing
    the same. More RAM/disk flexibility, gets us off macOS for
    server workloads.
  - [FILL] §7.12.

### 5.2 Network model

- Server on Tailscale only. No public IP, no port forwards.
- Backend listens on `0.0.0.0:8443` *inside* the tailnet.
- Git remote via tailnet ssh.
- MagicDNS hostname: `greylock.<tailnet>.ts.net`. Client config
  is just `https://greylock:8443`.
- API egress: outbound from server to api.anthropic.com /
  api.openai.com only. No other outbound traffic from the server.

### 5.3 Client (TUI) deployment

- Cross-compiled binary distributed via the git repo.
- First-run config: tailnet hostname, principal email.
- Session token cached per platform (Keychain / DPAPI /
  mode-600).
- Vault path: a local clone of the vault repo. TUI shows
  "uncommitted changes" in status bar; `:vault sync` from
  inside.

### 5.4 Backup model

- **Vault**: git push to a second bare remote on a separate
  device (NAS at one of the principals' residences, or a
  tailnet-reachable VPS). Continuous.
- **DB**: nightly encrypted dump to the same offsite mirror.
  Encrypted with a key held only by the principals (so a
  compromised server can't leak its own backups).
- **Documents blob store**: weekly encrypted archive.
- **Recovery drill** quarterly: restore to a fresh node, run
  DB ↔ vault ↔ docs consistency check. Logged as recurring Task.

---

## 6. Roadmap

### 6.1 v1 — Banking surface *(in flight, ~80%)*

**Goal:** consolidated read view, two users, two domains.

Shipping: Accounts tab, Transactions tab, Dashboard (replacement
widgets §7.3), Plaid Production linking, multi-user auth, session
caching, Tailscale-only network, SQLCipher with bitemporal schema.

**Bugs to clear:**
- `balance_cents` not persisted on Plaid sync.
- Vestigial shovel widgets + palette aliases removed.
- First-run entity seeding rewritten.
- `[R]` manual refresh wired.
- Rename `terminalfinance` → `greylock` (project root + DB).

### 6.2 v2 — Ledger & ontology layer

Categories tab, Merchants tab, split-tx, cross-entity
reimbursement, manual transactions, brokerage holdings read-only,
loans as Liabilities, monthly P&L + balance sheet.

### 6.3 v3 — Workflow engine, Vault, Decision log

Tasks tab, Decisions tab, Events tab, **vault ingestion
pipeline** (git post-receive hook → `IngestNote`), **Notes tab**
in TUI, Document blob store, automation rules editor, Alerts
panel.

### 6.4 v4 — Intelligence layer Phase 1 + Acquisition pipeline

Embedding pipeline, `:ask` palette command (RAG with Tier-A
default), categorization suggestions in Transactions, Targets
tab, fit score, pipeline kanban, Target → Acquisition transition
automation. **`:proposals` inbox** for confirming AI-extracted
objects.

### 6.5 v5 — Relationships, Personal life slice, Intelligence Phase 2

Relationship object type fully wired. Real Estate tab (GA rentals
with full records). Vehicles, Insurance, Subscriptions.
References auto-enrichment for Targets and People (opt-in).
Intelligence: deal memo drafting from templates; similarity-based
Target surfacing; cadence-aware Relationship alerts.

### 6.6 v6 — Forecasts + Intelligence Phase 3 (second brain)

Forecast objects + Scenario tab. Backtest module. Pattern
surfacing across the full corpus.

### 6.7 v7+ — Open

Substrate is extensible. New object types, new automations, new
model choices plug in without architectural rewrites.

---

## 7. Open architectural questions

*See `greylock-questions.md` for the consolidated list with
context and recommendations. Section numbers here mirror that
document.*

| Q | Topic |
|---|-------|
| Q1 | Plaid app structure |
| Q2 | First-run entity seeding |
| Q3 | Dashboard widget replacement |
| Q4 | Naming (Greylock vs alternatives) |
| Q5 | Single vs dual-source ledger |
| Q6 | Obsidian as default editor |
| Q7 | Git remote choice |
| Q8 | Local embedding model |
| Q9 | Embedding storage location |
| Q10 | API privacy tiering + provider terms |
| Q11 | Backup target |
| Q12 | Server migration path off the mac |
| Q13 | First non-finance object type to wire |
| Q14 | Top-level navigation keymap |
| Q15 | Entity list at launch |
| Q16 | Vault auto-commit vs manual commit |
| Q17 | Proposals inbox UX |
| Q18 | Cross-principal commentary on notes |

---

## 8. UI interaction specs

### 8.1 Top-level navigation

Vim-style `g` + letter:
- `g d` Dashboard
- `g a` Accounts
- `g t` Transactions
- `g c` Categories *(v2)*
- `g b` Budget
- `g k` Tasks *(v3)*
- `g D` Decisions *(v3)*
- `g e` Events *(v3)*
- `g n` Notes *(v3)*
- `g p` Proposals *(v4 — AI-proposed objects awaiting confirmation)*
- `g T` Targets *(v4)*
- `g R` Relationships *(v5)*
- `g r` Real Estate *(v5)*
- `g f` Forecasts *(v6)*

`Tab` / `Shift-Tab` = next/prev focus within current view.
`:` palette is the discovery fallback.
`1`–`9` switch entity.
`:ask <question>` invokes the intelligence layer *(v4)*.

### 8.2 Entity & user model

Entities at launch: `#me-rory`, `#me-tristan`, `#pcc`.

Entity-switch UI: number keys + tab strip header +
`:entity <name>`.
Shared-entity visibility: both principals see the same PCC
account list. No per-user filtering inside shared entities.

### 8.3 Dashboard

Starting widgets per Q3: Net Worth, Cash Flow This Month, Recent
Activity, Sync & Alerts. Configurable per-user. Drill-into =
full-screen, `Esc` returns.

### 8.4 Accounts view

Group by entity, institution secondary. Columns: Nickname,
Institution, Type, Balance, Last sync, Status. Row actions: `r`
rename, `H` hide, `u` unlink, `s` force sync, `Enter` drill.
Balance display: cents in data, dollars when ≥ $10k; `.` toggles.

### 8.5 Transactions view

Default: current month, current entity. Sort: `o` then column.
Filters: `f a/c/e/m/$/r/u/t`. Edit-in-place: `c m n e S`. Bulk:
`v` then `c/R`. Search: `/` substring, `/!` regex, `/m:`
merchant-only.

### 8.6 Categories *(v2)*

Own tab + modal picker. Two-level hierarchy. Auto-categorization
rules as a sub-view.

### 8.7 Budget *(v2)*

Per-entity, per-month, YTD roll-up. Table with variance bars.
Red on variance < −10%.

### 8.8 Sync / refresh

`R` syncs current entity, `Shift-R` syncs all visible. Backend
cron every 6h. Status bar: `synced 12m ago`; yellow >12h, red
>24h. Failed sync: banner + icon + drill-in.

### 8.9 Plaid link flow

Modal "Waiting for Plaid Link…" while browser is open. Post-link
auto-navigate to new account. Re-auth: banner + icon, modal
only for critical accounts.

### 8.10 Loans, investments, future financial types

Priority: credit cards → Liabilities → brokerage holdings →
retirement → identity.

### 8.11 Reimbursements

Flag inline (`R` on tx) + `:reimb` view. Status: pending / paid /
written_off. Auto-detect heuristics in v3.

### 8.12 Tasks, Decisions, Events *(v3)*

Tasks: table, `c` complete. Decisions: chronological feed,
outcome-prompt for decisions >90d without outcome. Events: list
view, attendees + links in detail.

### 8.13 Notes *(v3)*

- `g n` → list, default: this week, all kinds, accessible scope.
- Columns: Title, Kind, Author, Updated, Tags.
- Filters: `f k/a/t/s`; `/` full-text.
- `Enter` opens in `$EDITOR`; TUI watches for save and refreshes
  metadata.
- `N` new note (templates by kind).
- Status bar shows vault state.
- `:vault commit "<msg>"` and `:vault sync` for git ops in-TUI.

### 8.14 Proposals inbox *(v4)*

- `g p` → list of pending AI extractions.
- Columns: Source Note, Proposed Type, Title/Summary, Created.
- `Enter` opens the proposal: shows the source note excerpt, the
  proposed object payload, options to:
  - `a` apply as-is
  - `e` edit then apply
  - `r` reject (with optional reason)
  - `s` snooze
- Bulk: `v` visual select, `a` apply all selected.
- Empty state when inbox zero.

### 8.15 Targets *(v4)*

`g T` → pipeline by stage. `K` toggles kanban. Detail: company
facts, fit score with lineage drill-in, touch history (linked
Events), linked Decisions, owner, next action.

### 8.16 Relationships *(v5)*

`g R` → list of Relationships, default sort by "most overdue"
(`last_contacted_at` vs `cadence_target`). Columns: Person,
Kind, Last Contact, Cadence, Status. Detail: full Relationship
record including `what_theyre_working_on`, open
commitments, interaction history.

### 8.17 Intelligence (`:ask`) *(v4)*

- `:ask <question>` modal: runs retrieval, shows answer with
  drillable citations.
- `Tab` cycles citations; `Enter` jumps to source.
- Thumbs-up/down → `inferences.feedback_score`.
- `--api anthropic|openai` to pick provider; default Anthropic.
- Privacy tier shown in status bar of the modal; per-query
  consent prompt for Tier B if scope requires.

### 8.18 Documents

`:doc add <path>` to ingest. List filterable by entity / kind /
party. Open shells out.

### 8.19 Status bar, help, palette

Status bar (dense, right-to-left): entity context · sync clock ·
vault state · alert count · context hints.
Help (`?`): contextual cheat sheet; `?g` global.
Palette: fuzzy match.

### 8.20 Keyboard ergonomics

Primary: vim. Emacs opt-in. No mouse. Repeat counts supported.

### 8.21 Theme

Color meaningful only (green positive, red negative, yellow warn,
blue transfer, dim grey pending). Unicode borders default, ASCII
fallback. Dense default, `--spacious` for screenshots. Dark
default, light optional.

---

## 9. Non-goals (explicit)

- **No public web surface.** Ever.
- **No multi-tenant SaaS.** Single household.
- **No AI auto-classification in v1-v3.** Rules-based first; ML
  inference only with lineage in v4.
- **No "intelligence" panels without lineage.** Every number,
  every model answer, drillable to source.
- **No data exfiltration without an explicit, audited Action.**
  Accountant gets exports, not API access.
- **No sentience claims.** The intelligence layer is a powerful
  retrieval+reasoning system, not a conscious entity.
- **No raw data to third-party APIs without redaction tiering.**
- **No automation that mutates ontology state from inference
  alone.** Every extraction is *proposed*; a principal *applies*
  it. This is structural, not optional.
- **No personal-corpus-of-the-internet.** External information
  enters the graph through internal nodes only (§2.7 lens rule).

---

## 10. Parking lot

- Mobile companion via Tailscale.
- CLI subcommands.
- Backtest of past Decisions — calibration score per decider.
- Voice notes on Events — record on phone, drop into watched
  dir, transcribe locally.
- Tax view — annual roll-up per entity for CPA handoff.
- Crypto custody — read-only via address watch.
- Audit export — signed snapshot to offline drive for bus-factor
  cases.
- Cross-principal commentary on notes (Q18).
- Email + calendar ingestion (local IMAP/CalDAV).
- Local SLM fallback if the API is down or a query requires
  Tier C.

---

## 11. Decisions log

| Date | Decision | Made by |
|------|----------|---------|
| 2026-05-16 | Spec scope expanded from banking dashboard to ontology system | Rory |
| 2026-05-16 | Vault + Git + Intelligence layer added as pillars; ontology re-anchored as foundation | Rory |
| 2026-05-16 | Mac (Tristan's) confirmed as v1-v3 deployment target | Rory + Tristan |
| 2026-05-16 | Intelligence architecture: local retrieval + frontier API for reasoning, with privacy tiering | Rory |
| 2026-05-16 | Self-learning model: propose-not-apply; humans confirm all ontology mutations from inference | Rory |
| 2026-05-16 | Mental model: JARVIS not Ultron — extended cognition, not autonomous agent | Rory |
| 2026-05-16 | Q1 Plaid app structure: **C** — three apps (PCC + Rory personal + Tristan personal), 30 free items, clean PCC/personal separation | Rory |
| 2026-05-16 | Q2 First-run entity seeding: **B** — remove hardcoded seed; onboarding-driven entity creation | Rory |
| 2026-05-16 | Q3 Dashboard widgets: **confirm** Claude's set — Net Worth, Cash Flow This Month, Recent Activity, Sync & Alerts | Rory |
| 2026-05-16 | Q4 Naming: **Greylock** (binary + product) | Rory |
| 2026-05-16 | Q5 Ledger: **single source in Greylock**; build QuickBooks-style accounting inside, not dual-write | Rory |
| 2026-05-16 | Q6 Editor: **A** — recommend Obsidian; format stays portable | Rory |
| 2026-05-16 | Q7 Git remote: **A** — plain ssh-git with post-receive hooks | Rory |
| 2026-05-16 | Q8 Embedding model: **defer** until benchmarking is warranted | Rory |
| 2026-05-16 | Q9 Embedding storage: **A** — in main SQLCipher DB as BLOBs | Rory |
| 2026-05-16 | Q10 API privacy tiering: **defer** | Rory |
| 2026-05-16 | Q11 Backup target: **A** — tailnet-reachable NAS (Rory's place or Tristan's parents') | Rory |
| 2026-05-16 | Q12 Server migration: Tristan's mac now → future Raspberry Pi 5 16GB running local LLM + server in parallel | Rory |
| 2026-05-16 | Q13 First non-finance object type: **B** — Note (vault ingestion) | Rory |
| 2026-05-16 | Q14 Top-level keymap: **adopt** vim-style `g`+letter; Tab/Shift-Tab cycle focus within view; `:` palette fallback | Rory |
| 2026-05-16 | Q15 Entity list at launch: `#me-rory` + `#me-tristan` + `#pcc` | Rory |
| 2026-05-16 | Q16 Vault commit: **C** — hybrid, auto-commit drafts to personal branch, manual merge to main | Rory |
| 2026-05-16 | Q17 Proposals inbox UX: **confirm** | Rory |
| 2026-05-16 | Q18 Cross-principal commentary: **B** — reply-notes (wikilinked back to original) | Rory |

---

## 12. Glossary

- **Ontology**: typed schema of objects + relationships.
- **Bitemporal**: two time axes — when true, when known.
- **Lineage**: source-row chain for any derived value.
- **Action**: typed, audited mutation on the ontology.
- **Automation**: declarative *event → guard → action* rule.
- **Vault**: the Obsidian-style markdown directory.
- **Note**: a single markdown file + its ontology handle.
- **Wikilink**: `[[other note]]`, parsed and resolved to typed
  edges.
- **Embedding**: vector representation for semantic retrieval.
- **RAG**: retrieval-augmented generation — local retrieve,
  API reason.
- **Inference**: a single intelligence-layer call. Logged.
- **Pending extraction**: an AI-proposed object awaiting human
  confirmation.
- **Privacy tier**: A (ask freely) / B (ask with redaction) /
  C (local only). Controls what data crosses to the API.
- **Entity** (Greylock sense): legal person or virtual
  container for personal scope.
- **Item** (Plaid sense): one bank-login authorization.
- **Target**: a potential PCC acquisition, typed.
- **Tailnet**: the private Tailscale network.
- **JARVIS framing**: extended cognition; principals stay the
  operators. (Not Ultron — no autonomous agency.)
