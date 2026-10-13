# Comment and Documentation Style

Every comment, doc comment and design document in this repository follows these rules. They apply to code written by people and by agents alike.

## Rules

- **Concise by default.** Say it once, in as few words as carry the meaning. A constant's doc is at most three lines; no derivations or measured figures in it.
- **One home per fact.** State an invariant at the definition that enforces it. Do not repeat it at the class, the member and the function. A copy elsewhere gets a short pointer only where the site would mislead without it. Before deleting a copy, check the remaining home actually states the fact.
- **No history.** No "previously", "used to", "the old X", "as before", "now", "no longer", "before the fix", "Fix N regression". State what is true.
- **No work-plan talk.** No "phase N" as a step of building the project, no review tags, no "built ahead of its caller", no "yet". Naming a function's own runtime steps "phase 1/phase 2" is fine.
- **No comments on self-explanatory code.** No per-call-site comments that restate the callee's contract. No `@param`/`@return` that only restates the name on private code. Usage recipes belong in public headers only.
- **No narration.** No derivations, abandoned approaches or "actually" self-corrections in code comments; keep the conclusion. Timing or rationale narratives belong in `docs/` (one line in code at most).
- **No justifying absent behavior** against code that was removed.
- **ASCII punctuation.** No em or en dashes and no dash substitutes (`--`, a sentence-level ` - `). Rephrase with a comma, colon, semicolon, parentheses, or a new sentence.
- **Cite the spec by section,** never by pull request number: `spec "Player Buffer Accounting"`. Never invent a section name; if a claim cannot be located, drop the citation.
- **Never cite another implementation as the reference** (aiosendspin, sendspin-cpp). Cite the spec section, or the RFC or draft the spec delegates to.
- **Do not edit downstream comments or docs** to explain an upstream change unless they became wrong.
- **Check comments before committing,** not after a reviewer asks.
