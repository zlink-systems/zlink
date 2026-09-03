# Spec audit D — DEALER / ROUTER / STREAM

Scope: `core/doc/spec/core/socket/06-dealer.{ko,en}.md`, `07-router.{ko,en}.md`,
`08-stream.{ko,en}.md`, audited against
`doc/principal/documentation/spec-writing-guide.ko.md`.

## Summary

These three chapters were already close to guide-compliant: each has a
result-first overview (§4.1), an owning-document table (§4.2), inline enum
comments (§8.3), sequence diagrams for multi-party flows (§7.2), and a
closing interface-observation verification section (§9.3) that already
separates from the prose layer in the large majority of cases (§4.3). The
audit found a small number of concrete, fixable violations plus several
divergences that are real but unsafe to fix without touching anchors other
documents depend on, or without guessing at unclear contract intent — those
are recorded as deferred / contract questions instead of edited, per the
hard rules.

## Findings table

| File | Violations found (guide §) | Fixed | Deferred |
|---|---|---|---|
| `07-router.ko.md` | Broken same-file anchor: `§2` link at old line 247 pointed to `#2-raw-receive-record-구분`, but the heading `## 2. DATA와 REQUEST receive` generates `#2-data와-request-receive`. The `.en` twin already used the correct slug (§2.7 link-check / hard rule "verify link targets exist"). | Yes — link retargeted to `#2-data와-request-receive`. | — |
| `06-dealer.ko.md`, `06-dealer.en.md` | §3.5 compressed noun phrase: "exact-pipe lifetime·stale 전달 소유권" / "exact-pipe lifetime and stale-delivery ownership" hides what "exact" means (§3.5's own worked example is this exact word). | Yes — reworded to "선택한 그 pipe의 lifetime·stale 전달 소유권" / "the lifetime and stale-delivery ownership of that selected pipe" (same referent: the pipe a multipart already selected, per the sentence immediately before it). | — |
| `07-router.ko.md`, `07-router.en.md` | Same §3.5 "exact-pipe" phrase, duplicated verbatim in the ROUTER weight section. | Yes — same reword applied. | — |
| `08-stream.ko.md`, `08-stream.en.md` | §5.7: the RAW/PACKET receive-mode comparison table (§3, "수신 모드"/"Receive modes") is a choice-comparison table but was missing the "언제 쓰는가"/"When to use it" leading column that §5.7 and the §7.1 checklist require for tables comparing kinds. | Yes — added a leading column stating when each mode applies, drawn from the mode descriptions already stated in the same document (§1, §6 intro) — no new claims introduced. | — |
| `06-dealer.ko.md`, `06-dealer.en.md` | §4.3/§9.3 duplication: several behavior paragraphs are restated near-verbatim in the §10/§9.3-equivalent verification section — e.g. the remote-pause/`EAGAIN` sentence (ko L181-187) reappears at the verification bullet (ko L443); the flow-state-frame paragraph (ko L168-175) is near-verbatim at bullets L436-437. Guide says the verification section should own the observable-behavior statement once, and prose should carry *why/how* only. | No | Deferred — the guide's own hard rule for this task forbids dropping or merging a normative sentence, and these prose paragraphs carry mechanism (e.g. *why* clearing pause doesn't unblock send, *why* stale frames are dropped) that the bulleted verification items do not restate in full. Collapsing either side risks losing normative content under the effort available for this pass; recommend a follow-up pass scoped specifically to de-duplication with more room to verify nothing is lost. |
| `07-router.ko.md`, `07-router.en.md` | Same §4.3/§9.3 duplication pattern between §11 (Receive flow state prose, ko L342-348) and §13 verification bullets (ko L466-467). | No | Deferred, same reasoning as above. |
| `06-dealer.{ko,en}.md`, `07-router.{ko,en}.md` | §2.2: declaration-vs-behavior ordering diverges across the three chapters. DEALER puts 공개 타입/option (§7-§8) before 함수 (§9); ROUTER puts 공개 타입 at §4, *before* the behavior sections §6-§9; STREAM front-loads all declarations into §2 before 수신 모드 (§3) and packet framing (§6). The guide's preferred order is concept-first, declarations later (§2.2 "선언과 레퍼런스는 개념 뒤 절로"). | No | Deferred — reordering ROUTER's §4 in particular is not anchor-free: `§5 ROUTER option` (ko L115) links `[§4](#4-공개-타입)`, and other documents were not checked for cross-references into this numbering (out of scope per the hard rule "touch nothing outside your assigned files" — a reorder would require checking every document, not just these six). Recommend a separate, explicitly-scoped renumbering pass. |
| `08-stream.ko.md`, `08-stream.en.md` | §3.5: "bounded receive queue" / "bounded packet receive queue" (§6 intro, and the internals diagram) is stated before its bound (`RCVHWM`) is named in §6.3. | No | Deferred as low-value — this is summary-before-detail (§4.3's intended layering), not a missing fact; the bound is stated two paragraphs later in the same top-level section. Flagged for awareness only; not edited to avoid non-surgical churn for marginal benefit. |

## CONTRACT-QUESTIONS

- **`06-dealer.ko.md:159` / `06-dealer.en.md:171`** — "각 ready count `1` Application connection의 Core control 경로로 보낸다" / "sends it over the Core control path of every ready count `1` Application connection." This clause is not parseable as written (`ready count 1` reads as a stray fragment, not a term used anywhere else in the file). It sits directly on top of the DEALER-ROUTER single-lane-count-1 rule that the task instructions explicitly forbid altering the meaning of. Left unchanged; flagging for the spec owner to clarify intent (likely "the single ready Application connection" or "one Application connection per ready pair", but I will not guess and rewrite a normative sentence).

## Headings added

None. No new headings were introduced; the stream receive-mode table gained a
column, not a section, and the two `exact-pipe` rewords stayed inside their
existing sentences.

## Validation performed

- `git diff --check` on all six files: clean (no whitespace errors).
- In-file anchor check (script-verified heading-slug vs `](#...)` links) across all six files: no broken same-file anchors remain (the one broken case, `07-router.ko.md`, was fixed).
- Cross-file relative link existence check (`../*.md`, `README.md`, sibling chapter files) for all six files: all targets exist.
- Cross-file anchor existence check for every `file.md#anchor` link referenced from the six files (glossary, errors, message, ZMP protocol, socket README, in both languages): all anchors resolve.
- Read both `.ko.md` and `.en.md` of all three chapters in full to confirm structural and content parity; no parity gaps found beyond two harmless legacy `<a id="...">` aliases in `06-dealer.en.md` (`8-results-and-readiness`, `2-dealer-options`) that are not linked from anywhere in the repo (checked via `rg`) — left untouched as out-of-scope cleanup, not a violation.
