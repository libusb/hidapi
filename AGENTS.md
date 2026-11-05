# Contribution Guidelines for AI Coding Agents

These are the canonical contribution and commit rules for this repository. They apply to
**every** AI coding agent used on this project (OpenAI Codex, Cursor, GitHub Copilot, Claude
Code, Gemini CLI, and others) and to any automation acting on a contributor's behalf.

This file (`AGENTS.md`) is the single source of truth. Most agents read it natively; the few
that use a dedicated file load these same rules from here — `.claude/CLAUDE.md` and `GEMINI.md`
import it, and `.github/copilot-instructions.md` points to it — so there is only ever one copy
to maintain.

## Commit Authorship

Every commit in this repository must be authored by a human (the contributor). No AI agent
(Codex, Cursor, GitHub Copilot, Claude, Gemini, or any other) may be set as the commit author.

At the end of every commit message, include an explicit attribution trailer indicating which
AI model assisted, in this format:

```
Assisted-by: AGENT_NAME:MODEL_VERSION
```

For example: `Assisted-by: claude-code:claude-opus-4-7` or
`Assisted-by: github-copilot:MODEL_VERSION`.

This applies to all commits, including those created via automation or agent workflows.

## Commit generation

No commit should be signed-off by an AI agent or OS. Only a human can sign-off their commits
with their own certificate.

## Branch Naming

- Do not prefix branches with `claude/`, `copilot/`, `codex/`, `cursor/`, `ai/`, `bot/`, or any agent-derived namespace.
- Do not append auto-generated suffixes (random IDs, timestamps, session hashes) unless genuinely required to disambiguate.
- Branch names should be explicit and brief about what is being done or asked — e.g. `add-commit-attribution`, `fix-win-hang`, etc.
- Prefer kebab-case.

## PR description content

The `Assisted-by` attribution should be included in the PR description, but no link to the
session itself should be included, as it is not publicly accessible.

When a PR fixes an issue or relates to / replaces another issue, the PR description should
include a reference to the issue number after the main description but before the
`Assisted-by:` attribution, e.g. `Fixes: #123` or `Closes: #124`.

## HIDAPI API usage contract

This is the contract HIDAPI expects of its callers. It governs both code review and code
generation: judge the library's behaviour only against usage that respects it, and only
generate example/test code that respects it. The authoritative reference is
[Multi‐threading Notes](https://github.com/libusb/hidapi/wiki/Multi%E2%80%90threading-Notes).

### Library lifecycle

- Call `hid_init()` before any other HIDAPI function. Implicit lazy initialization exists, but
  must not be relied upon when more than one thread may make the first call.
- Call `hid_exit()` last, after every device has been closed. `hid_exit()` deinitializes the
  whole library; no HIDAPI function other than a new `hid_init()` may be called after it, and
  device handles obtained before it are invalid.
- `hid_init()`/`hid_exit()` must never run concurrently with any other HIDAPI function.

### Concurrency

- HIDAPI v0.x.x is **not thread-safe**. The following must not be called concurrently from
  different threads: `hid_init`, `hid_exit`, `hid_enumerate`, `hid_open`, `hid_open_path`,
  `hid_close`, `hid_error(NULL)` — the global error string is one reason.
- Functions taking a `hid_device *` are not individually thread-safe, but different devices may
  be used from different threads — i.e. a dedicated thread per device.
- `hid_close()` must be serialized against initialization and enumeration calls.
- Since v0.15.0, a dedicated read thread using only `hid_read`, `hid_read_timeout` and
  `hid_read_error` is safe alongside other operations on the same device from another thread.
  Do not use `hid_error` from such a thread; use `hid_read_error`.
- macOS: `hid_init()` and `hid_exit()` must be called from the same thread, and that thread must
  stay alive until all devices are closed and `hid_exit()` has completed.

### For AI agents and reviewers

A finding whose trigger requires violating this contract is application misuse, not a library
defect. Examples: calling `hid_exit()` while other threads still use the API, using device
handles after `hid_exit()`, unsynchronized concurrent first-time initialization. Do not report
such scenarios as bugs, and do not add locking or other synchronization to "fix" them. Hardening
the library against misuse is a maintainer design decision, not a review fix.
