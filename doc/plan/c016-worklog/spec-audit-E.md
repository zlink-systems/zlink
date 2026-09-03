# Spec audit E — core/doc/spec/core/protocol + core/doc/spec/core/systems

Audited against `doc/principal/documentation/spec-writing-guide.ko.md` (§3–8 checklist).
Work split across 4 parallel agents by file group; ko/en kept in parity throughout.
`git diff --check` on all 22 files: clean.

## File-by-file results

| File | Violations found (guide §) | Fixed | Deferred |
|---|---|---|---|
| protocol/README.ko.md / .en.md | None significant — short index page already states scope, links owning docs, table has proper columns. | — | — |
| protocol/01-zmp.ko.md / .en.md | §3.5 compressed term "exact Application pipe"/"exact pipe" in "Peer-weight control" (§9) never unpacked which pipe it means. §3.2 "Completion connection" (handshake §4) first-used with no glossary link despite being defined as "completion progress lane" in glossary. §3.2 "HWM" first used (WEIGHT-bypass sentence, §4.1) without glossary link. | All three fixed: "exact pipe" reworded to state plainly it means the specific pipe that received the command (not another pipe to the same peer), then reused as plain "그 pipe"/"that pipe"; added first-use glossary link + one-clause gloss for "Completion connection" → `glossary.md#completion-progress-lane`; added glossary link for "HWM" → `glossary.md#hwm`. Applied identically ko/en. | Left "admission" (used repeatedly: `socket lane-set admission`, `HWM admission`, `outbound admission`) unlinked/unexpanded — meaning is clear from context each time, not combined into an opaque compound noun. Left "generation" (connection generation, §4.1) unlinked to glossary's `generation` entry, which defines a *directional-queue* regeneration counter, not a *connection* generation counter — see CONTRACT-QUESTIONS. |
| protocol/02-raw.ko.md / .en.md | None significant — states scope first, links owning docs (STREAM socket, ZMP), has closing verification-requirements section, no unexplained compressed terms. | — | — |
| systems/README.ko.md / .en.md | None. | — | — |
| systems/01-architecture.ko.md / .en.md | None. | — | — |
| systems/02-threading-model.ko.md / .en.md | None. | — | — |
| systems/03-io-thread.ko.md / .en.md | None. | — | — |
| systems/04-thread-safety.ko.md / .en.md | None. | — | — |
| systems/05-connection-memory.ko.md / .en.md | None — already conforms to §4.1/§4.2/§4.4/§6.2/§9.3. ko/en in full parity. | — | — |
| systems/06-auto-hwm.ko.md / .en.md | §3.5: bare `exact` hiding meaning — "exact pipe owner" (ko:516) and "exact pair" (ko:575), mirrored en:405, en:454. | Reworded to spell out "that same/that specific" pipe owner / pair, in both languages. | The 605 (ko) vs 484 (en) line-count gap looked like a possible parity issue but was verified NOT to be one: heading-by-heading and paragraph-by-paragraph comparison shows identical structure/content; the difference is purely ko's manual mid-paragraph line-wrapping vs en's long single lines. Left as-is per the no-unnecessary-reflow hard rule. |
| systems/07-core-source-layout.ko.md / .en.md | §2.5: two independent judgments (layer-boundary rule + Framework-owned concept list) packed into one paragraph. §3.1/§4.2: bare concept names (`MeshName, ChannelName, Spot, Actor, Location Store...`) listed with no behavior sentence and no pointer to the owning doc. | Added a lead-in sentence identifying these as Framework-layer concepts, linked to `08-posd-module-structure.{ko,en}.md` §3 (which already states Framework's position), and split the "does not contain X" judgment from the seam judgment. | — |
| systems/08-posd-module-structure.ko.md / .en.md | §3.5/§7.1: §2 responsibility table cells were bare noun lists with no verb/subject (e.g. "argument, handle, ownership과 result mapping"). §3.1: en §1 led with the name "POSD (...) is..." before behavior, while ko led with behavior — parity/shape mismatch. | Rewrote all 5 table-cell rows in both languages as verb phrases (same layers/responsibilities, phrasing only); flipped en §1 opening to lead with behavior before naming POSD; unified the inline socket gloss shape between ko/en. | — |
| systems/09-design-decisions.ko.md / .en.md | §2.5: §4 Multipart atomicity crammed three judgments (what's grouped, what's delegated, what caller is spared) into one sentence. §3.5: `transaction 처리`, `partial failure cleanup` used as compressed nouns without saying what's cleaned up. Defect (not guide-related): en §6 had a broken em-dash construction with no conjunction after the closing dash — didn't parse; ko was already correct. | Split §4 into separate sentences, unpacking "partial failure cleanup" into "disposes of parts not yet sent"; fixed en §6 punctuation to restore ko/en parity and grammatical correctness. | — |
| systems/10-hot-path.ko.md / .en.md | §4.2 candidate: `plan Phase 5.2` (§5.2, ~ko:115/en:125) is an unlinked reference to an external planning doc. | Not fixed — `rg -rl "Phase 5.2" doc/` finds no matching document to link to. | Recorded under CONTRACT-QUESTIONS. No other violations; the hot-path scope table (§2) and forbidden-operations list (§3) were left untouched per hard rule — no edits made to either file's normative content. |

## CONTRACT-QUESTIONS

1. **`core/doc/spec/core/protocol/01-zmp.ko.md:201` / `01-zmp.en.md:204`** — "물리 connection ID와 generation은 wire property나 public target이 아니다" / "Physical connection IDs and generations are neither wire properties nor public targets." The document's "generation" here denotes a per-physical-connection version counter (incremented on reconnect), while `core/doc/spec/core/glossary.ko.md#generation` defines `generation` as a *directional-queue* regeneration counter. These may be the same underlying mechanism viewed from two angles, or two genuinely distinct counters sharing a name. Not linked/merged because doing so would either overwrite the glossary's narrower definition or misrepresent ZMP's connection-generation semantics — needs a judgment call from whoever owns both documents.

2. **`core/doc/spec/core/systems/10-hot-path.ko.md:115` and `10-hot-path.en.md:125`** — text references "plan Phase 5.2" / `bindings/c/perf/perf_regression_gate.py` as the owning artifact for the release-comparison gate, but no document containing "Phase 5.2" exists anywhere under `doc/`. Left unchanged per the hard rule protecting this sensitive file; flagging so the doc owner can confirm whether the plan doc was renamed/removed, or the reference is stale and needs a fix (out of scope for this audit — meaning was not touched).

## Headings added

None. No heading text or anchors were changed, renamed, or removed in any of the 22 files. All new links point to existing headings/anchors (verified to resolve on disk):
- `glossary.{ko,en}.md#completion-progress-lane`, `glossary.{ko,en}.md#hwm` (from protocol/01-zmp)
- `08-posd-module-structure.{ko,en}.md` §3 (from systems/07-core-source-layout)

## Groups with no findings requiring edits

protocol/README, protocol/02-raw, systems/README, systems/01-architecture, systems/02-threading-model, systems/03-io-thread, systems/04-thread-safety, systems/05-connection-memory — all already conformed to the guide; no edits made.

## Validation performed

- `git diff --check` across all 22 files: clean.
- Every relative link/anchor touched in an edited file was confirmed to resolve to an existing file/heading on disk.
- Normative content (wire formats, byte values, property names, MUST/MAY semantics, enum names, error codes, sequences, sizes, timeouts, ordering guarantees, the systems/10-hot-path scope table and forbidden-operations list) was left unchanged everywhere; only prose/terminology/structure was edited.
