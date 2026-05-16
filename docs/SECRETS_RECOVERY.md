# Secrets Recovery Runbook

> What to do when a secret value ends up somewhere it shouldn't, or when
> the env file on a host gets malformed during initial setup. Read this
> alongside the secrets-discipline rules in
> [docs/QA_PROMPT.md § Secrets discipline](QA_PROMPT.md) and
> [docs/PLAID_SETUP.md § Secrets discipline](PLAID_SETUP.md).

The discipline rule is "never echo secrets." This file is the playbook
for *when* discipline fails — and it will, occasionally. The fix is
mechanical, not moral.

---

## The two failure modes covered

### Mode A — env file value is malformed (functional)

You SSHed to the server, pasted into a heredoc with placeholders, and the
placeholder syntax leaked into the saved value. Example: an env file
ends up with literal angle-bracket characters around the value:

```
PLAID_CLIENT_ID=<69e054de61a4a2000d1628f1>
PLAID_SECRET=<ef4b34516ef45dc87aaf3e51f347de>
TF_MASTER_KEY=<4e3...>
```

The server reads the value including the brackets. Plaid rejects every
request with `INVALID_CLIENT_ID`. SQLCipher fails to decrypt the
database. Both look like "credentials are wrong" but the issue is
**punctuation around the value**, not the value itself.

The same class of failure happens with:
- Quotation marks pasted in (`PLAID_SECRET="abc..."` — the server reads `"abc..."` literally)
- Trailing whitespace from clipboard
- `\r` from a Windows clipboard (CRLF line endings)
- Newlines mid-value

### Mode B — secret value appeared somewhere it shouldn't (discipline)

A real value showed up in:
- A GitHub issue, comment, commit message, or PR body
- A bridge message
- A status writeup / synthesis page
- An AI session output (chat log, reasoning trace, command output)
- A Slack / Discord / chat-app message
- A screen recording or screenshot
- A pastebin / gist

The "blast radius" depends on where: GitHub-public is worst; private repo
or AI conversation is significantly better but still requires rotation
per the standing discipline.

---

## Diagnosis — what to use, what to avoid

When checking an env file for shape issues, **never dump the values to
stdout.** Specifically:

| ✘ Don't use | ✓ Use instead |
|---|---|
| `cat ~/.greylock.env` | `ls -la ~/.greylock.env` (check perms) |
| `od -c ~/.greylock.env` | `wc -c ~/.greylock.env` (byte count) |
| `xxd ~/.greylock.env` | `sha256sum ~/.greylock.env` (file fingerprint) |
| `awk -F= '{print $2}'` | `awk -F= '/^KEY=/{print length($2)}'` (length only) |
| `grep -o '.*' ~/.greylock.env` | `grep -c "^KEY=." ~/.greylock.env` (presence only) |

Concrete probes that reveal shape without values:

```sh
ls -la ~/.greylock.env
# expect: -rw-------  ... (mode 600)

# Each key present?
grep -c "^PLAID_CLIENT_ID=." ~/.greylock.env   # → 1
grep -c "^PLAID_SECRET=." ~/.greylock.env      # → 1
grep -c "^TF_MASTER_KEY=." ~/.greylock.env     # → 1
grep -c "^PLAID_ENV=" ~/.greylock.env          # → 1

# Length of each value (no value content displayed)
awk -F= '
  /^PLAID_CLIENT_ID=/{print "client_id length:",  length($2)}
  /^PLAID_SECRET=/   {print "secret length:",     length($2)}
  /^TF_MASTER_KEY=/  {print "master_key length:", length($2)}
' ~/.greylock.env

# Expected lengths:
#   PLAID_CLIENT_ID  — 24 hex chars (Plaid client_id)
#   PLAID_SECRET     — 30 hex chars (Plaid secret)
#   TF_MASTER_KEY    — 64 hex chars (32 random bytes, hex-encoded)
#
# If lengths are 26 / 32 / 66 — values are wrapped in <> or "".
# If lengths are 25 / 31 / 65 — likely trailing CRLF from Windows clipboard.

# Visible env value (NOT a secret — safe to display)
grep "^PLAID_ENV=" ~/.greylock.env
```

If you really need to verify a specific paste matches what you intended,
use a hash comparison — `sha256sum` of the value field via a temp file
that you immediately `shred -u`. Don't echo, don't `cat`, don't `xxd`.

---

## Recovery procedure

### Step 1 — Rotate the affected secrets

For each secret that ended up somewhere it shouldn't (or that you suspect
might have):

| Secret | Where to rotate |
|---|---|
| Plaid `client_id` / `secret` (Sandbox or Production) | Plaid dashboard → Developers → API Keys → Rotate. The old value invalidates the moment you click. |
| `TF_MASTER_KEY` | Generate fresh on the server: `openssl rand -hex 32`. **Be aware:** every Plaid token in the existing `accounts` table becomes encrypted-garbage after rotation. All linked accounts must be re-linked. Production has higher cost; Sandbox is cheap (3 min of curl). |
| Plaid `access_token` (an actual bank's token) | Plaid dashboard → Items → Remove Item, then re-Link the account in Plaid Link. |
| Session bearer / TOTP recovery codes | `/auth/logout` invalidates active session. TOTP secret rotation requires re-enrollment. |

Rotation is one-click for Plaid creds. For `TF_MASTER_KEY` the rotation
itself is `openssl rand -hex 32`; the painful part is re-linking accounts.
Always rotate sooner rather than later — re-linking gets more expensive as
more accounts accumulate.

### Step 2 — Decide on the residual-entropy question

If only *part* of a secret leaked (e.g., first few hex chars), residual
entropy is usually still uncrackable in the time horizon that matters.
But discipline says rotate anyway. The cost is trivial relative to the
risk of being wrong about "how much entropy is enough."

Default: **rotate.** Save the philosophical debate for when rotation is
actually expensive.

### Step 3 — Fix the env file cleanly

Open your own terminal (not an AI assistant's shell), SSH to the host,
and rewrite `~/.greylock.env` from scratch. **Do not** edit in place — the
existing malformed content can persist via paste mishaps.

```sh
ssh tristan@<skynet-host>
unset HISTFILE; set +o history; umask 077

# Type the heredoc by hand. Replace each ALL_CAPS_PLACEHOLDER with the
# actual value. Do NOT keep the placeholder characters. Do NOT wrap
# values in quotes. Do NOT include trailing newlines or spaces.
cat > ~/.greylock.env <<EOF
PLAID_CLIENT_ID=NEW_PROD_CLIENT_ID_24_HEX_CHARS_NO_BRACKETS
PLAID_SECRET=NEW_PROD_SECRET_30_HEX_CHARS_NO_BRACKETS
PLAID_ENV=production
TF_MASTER_KEY=FRESH_64_HEX_NO_BRACKETS_FROM_OPENSSL_RAND_HEX_32
TF_SERVER_BIND_ADDR=0.0.0.0
EOF
chmod 600 ~/.greylock.env
exit
```

### Step 4 — Verify shape (no values displayed)

Run the length probe from the Diagnosis section. Expected lengths exactly:

| Variable | Expected length |
|---|---|
| `PLAID_CLIENT_ID` | 24 |
| `PLAID_SECRET` | 30 |
| `TF_MASTER_KEY` | 64 |

If any length is wrong, the env file has invisible characters or
punctuation. Re-do step 3.

### Step 5 — Restart the server with fresh env

```sh
pkill -TERM -f TerminalFinanceServer
sleep 1
cd ~/code/GREYLOCK
set -a; source ~/.greylock.env; set +a
nohup ./build/TerminalFinanceServer > ~/loftus-mcp-bridge/logs/greylock-server.log 2>&1 &
disown
sleep 2
tail -n 15 ~/loftus-mcp-bridge/logs/greylock-server.log
```

Confirm:
- `Database opened with master key: yes`
- `PlaidTokenBroker initialized (TF_MASTER_KEY present).`
- `PlaidSyncScheduler: started`
- `Listening on https://0.0.0.0:8443`

### Step 6 — Re-link affected accounts (only if `TF_MASTER_KEY` was rotated)

For each previously-linked account in `accounts` where
`is_plaid_linked = 1`:

1. The old `encrypted_token` is unusable — decrypt would fail under the
   new master key.
2. Either: `UPDATE accounts SET is_plaid_linked=0, encrypted_token=NULL WHERE id='...';`
   and re-Link via the normal flow.
3. Or: drop the old account row entirely if it's a test artifact (the
   Sandbox-linked account from initial bring-up usually qualifies).

---

## Prevention — heredoc patterns that don't bite

The angle-bracket gotcha happens because the placeholder reads like a
literal during paste. Patterns that avoid it:

### Pattern A — ALL_CAPS placeholders (no special characters)

```sh
cat > ~/.greylock.env <<EOF
PLAID_CLIENT_ID=REPLACE_WITH_CLIENT_ID
PLAID_SECRET=REPLACE_WITH_SECRET
EOF
```

The pasting human sees `REPLACE_WITH_CLIENT_ID` and is forced to
delete-and-retype rather than paste-over-the-brackets. No special chars
to accidentally preserve.

### Pattern B — Generate values directly in the shell

For `TF_MASTER_KEY` specifically, never paste — generate in-place:

```sh
TF_MASTER_KEY=$(openssl rand -hex 32)
echo "$TF_MASTER_KEY"   # ONCE, for paper backup. Then never again.
cat > ~/.greylock.env <<EOF
TF_MASTER_KEY=$TF_MASTER_KEY
EOF
unset TF_MASTER_KEY
```

The value is in a shell variable, expanded inline. No copy-paste round
trip means no clipboard artifacts.

### Pattern C — Pipe from Bitwarden Send / 1Password CLI

If your password manager has a CLI that can fetch a secret to stdout:

```sh
cat > ~/.greylock.env <<EOF
PLAID_SECRET=$(bw get password "Plaid Production Secret")
EOF
```

No human ever types the value. Provided the CLI is set up + the value
is in the manager, this is the cleanest path.

---

## After the recovery

- Update the audit trail: comment on the relevant GitHub issue noting
  "rotated keys X and Y, refreshed env, server restarted, accounts
  re-linked." No values, just the actions.
- If the leak was into a GitHub artifact (issue, comment, commit), the
  rotation is complete but the OLD value remains in GitHub's audit log.
  GitHub Support can perform a permanent purge if needed; contact them
  with the SHA and the values to scrub. For private repos with no
  outside collaborators, this is usually optional.
- If the leak was into an AI conversation history, the value remains in
  that conversation's record. Provider-side retention policies apply.
  Rotation is the only mitigation; exposure can't be revoked.

---

## Related

- [docs/PLAID_SETUP.md](PLAID_SETUP.md) — initial Plaid setup, env file format
- [docs/QA_PROMPT.md](QA_PROMPT.md) — § Secrets discipline (the non-negotiable rules)
- [docs/WORKFLOW.md](WORKFLOW.md) — § Standing discipline — Secrets
- PCC-SharedOS pre-commit hook — pattern-blocks `TF_MASTER_KEY`, `PLAID_*`,
  raw hex strings ≥32 chars from committing into the shared vault
