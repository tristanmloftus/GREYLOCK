# Contributing to Greylock

## v1-freeze (active until v1.0.0 tag)

Per [ROADMAP.md §4-A](ROADMAP.md), no v3+ work lands until v1.0.0 ships. The `v1-freeze` CI check (`.github/workflows/v1-freeze.yml`) fails any PR touching:

- `src/views/GraphView.*`
- `src/views/AskView.*`
- `src/views/DecisionDetailView.*`
- `src/views/RelationshipDetailView.*`
- Any path containing `V3ObjectsHandler`
- `tests/test_graph*`, `tests/test_ask*`, `tests/test_decision_detail*`, `tests/test_relationship_detail*`, `tests/test_v3_*`

**Escape hatch:** add `[v1-ok-touches-v3]` to the PR title. Use only for:

- Removing v3 code (cleanup PRs)
- Fixing a v3-side bug that genuinely blocks v1
- Renames / mechanical refactors that incidentally touch v3 files

The freeze lifts the moment `v1.0.0` is tagged. After that, v2 (ledger + reimbursements) becomes the next allowed work; v3+ stays gated until v2 ships.

## How PRs land

1. Branch off `main`. Do not branch off `v0.2-dev` for v1 work — that branch carries v3/v4 surfaces that are frozen.
2. Open PR against `main`.
3. CI runs `build-linux` + `v1-freeze` checks. Both must be green.
4. Auto-merge with `--rebase` is fine for single-commit docs/CI PRs. Code PRs prefer review + merge commit.
