---
name: docs-sync
description: Review a sendspin-server-cpp branch or diff for documentation and comment drift: stale text in docs/internals.md, README.md or CLAUDE.md, outdated doc comments in include/sendspin/server/, numeric values in docs that no longer match the code, src/ORIGIN.md entries, and comments elsewhere in the tree that describe changed behavior. Use before committing a piece of work, or when asked whether the docs are still accurate.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash
---

# Documentation Sync Review

Review the change for documentation drift it introduces or sits next to. Report findings only; do not edit files. The normative rules live in `docs/conventions.md` ("Documentation") and `docs/comment-style.md`; this checklist applies them to a diff.

## Scope

Determine the diff: if `$ARGUMENTS` names a commit range or branch, diff that; otherwise diff the current branch against main (`git diff main...HEAD`), falling back to uncommitted changes (`git diff HEAD`) if the branch has no commits yet. If the review environment already supplies the diff, review that diff directly instead of computing one.

Documentation drift is a defect: a behavior change is not complete until every description of that behavior is updated in the same change.

## Sync surfaces

For every behavioral or API change in the diff, check each of these:

1. **`docs/internals.md`**: anything it says that the diff makes untrue is a finding. It holds only cross-file facts, so a change to a thread, a cross-thread channel (its table names writer, reader and mechanism), the run or connection lifecycle, a group's playback, the formats and pipelines, or the order of work on core must be reflected there; a change confined to one function is documented at that function instead. Text the diff adds here that explains a single function, restates the spec, names a test, or quotes a constant's value is itself a finding. A new limitation goes under "Known limitations".
2. **Header doc comments** in `include/sendspin/server/`: `@brief`/`@param`/`@return` text, the threading notes on `SendspinServer`, `GroupInput`, the listener and the providers, and the `ServerConfig` and `ServerTiming` field docs (defaults and semantics).
3. **`README.md`**: the Features list, the build and test commands, and the `server_cli` command table and usage.
4. **`CLAUDE.md`**: the project layout, the key sources, build options, dependencies and conventions, when the structure they describe changes.
5. **`src/ORIGIN.md`**: a source copied or adapted from sendspin-cpp is listed with its origin; a removed one is pruned.
6. **Inline comments anywhere in the tree**: grep for the names of the functions, constants, messages, fields and states the diff changes, and read the surrounding comments in files the diff does NOT touch, `tests/` and `examples/server_cli/` included. A comment describing the old behavior of a changed function is a finding even three files away.

## Checks

- **Numbers match**: any numeric value quoted in a doc or comment (defaults, caps, timeouts, budgets, sizes, measured figures) matches the code after the diff. Verify against the source, not the hunk alone.
- **Symbols exist**: docs and comments do not reference removed or renamed types, fields, methods, config options, `ErrorKind`/`SessionEnd` values or `server_cli` commands. A deleted feature is pruned from every doc that mentions it.
- **Behavior derived from current code**: descriptions state what the code does now. Flag anything `docs/comment-style.md` bans in added text.
- **One home per fact**: an invariant is stated at the definition that enforces it; flag a copy the diff adds at the class, the member and the call site.
- **Workarounds are not laundered**: if the diff adds a workaround, the docs do not present it as designed behavior.
- **New behavior is documented**: a new public method, config field, listener callback, error kind or threading requirement with no header doc and README update is a finding.
- **The diff's own new docs match the diff's own new code**: verify each concrete claim in added doc text and comments against the code it describes; a new comment that claims a bound, a default or an order the adjacent code does not enforce is drift from day one.
- **Spec citations**: a cited spec section exists and says what the comment claims.

## Scope discipline

Only require fixes for drift the diff introduces or that lives in sections the diff touches. Pre-existing drift elsewhere is reported separately as "out of scope, worth a follow-up" and does not block the change.

## Report format

Two sections: **In scope** (fix before commit) and **Out of scope** (pre-existing drift, note only). For each finding give the doc location (file:line), what the doc says, what the code does (file:line), and the minimal correction.
