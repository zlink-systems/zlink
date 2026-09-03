# Spec audit A — core/doc/spec/core (README, glossary, 00–04)

Audited against `doc/principal/documentation/spec-writing-guide.ko.md`. Normative content
(MUST/MAY, values, enums, error codes, sequences, sizes, timeouts, ordering) was preserved
exactly; only clarity edits were made. `git diff --check` is clean on all touched files;
relative links/anchors touched were verified to resolve.

## Summary table

| file | violations found (guide §) | fixed | deferred |
|---|---|---|---|
| README.ko.md / README.en.md | None substantive — TOC/index docs are exempt from §3.1–3.5 term-intro rules per the guide's own carve-out ("제목, 목차… 첫 사용 판단에서 제외"). §7.1 tables already have condition+result per cell. §4.1 scope stated in opening paragraph. ko/en parity confirmed. | — | Missing bottom nav line (§2.2) is a repo-wide gap (also present in 00-public-contract-governance before this pass), not unique to these files — flagged for a repo-wide pass rather than a local one-off fix, to avoid desyncing from the ~40 other spec files nobody else is touching. |
| glossary.ko.md / glossary.en.md | §3.4's full 7-row registration table isn't used — glossary gives short definitions + link-out to the owning doc instead, per its own explicit meta-note ("정확한 계약은 해당 spec 문서가 소유"). Judged a deliberate, coherent design (consistent with §4.2 don't-duplicate-owned-contracts and §3.2 glossary-as-pointer), not a defect. | — | Left glossary format as-is (design choice, not a violation). "completion progress lane" entry's "bidirectional request wait cycle" phrasing is a mildly compressed noun phrase (§3.5) but not on the guide's flagged-word list; left unchanged to avoid nuance drift on normative-adjacent text — noted for a follow-up pass. |
| 00-public-contract-governance.ko.md / .en.md | §2.2 — no bottom navigation line (only present under the title, not repeated at document end). | Added matching bottom nav block to both files. | Doc-type table lacks a "when to use" column (§5.7) — judged non-required since it's an ownership map, not a "pick one" comparison, so left as-is. "Compatibility facade" left unglossed as a recognizable industry term (§3.3/§3.5 borderline) — low confidence, not touched. |
| 01-context.ko.md / .en.md | §8.3 — `zlink_auto_hwm_profile_t` enum values had no inline "when to use" comments, unlike every other enum in the file. | Added inline comments per value + a link to Auto HWM §2 for exact ratios/caps. | None. |
| 02-message.ko.md / .en.md | §2.5 — one sentence in the §6 input-rule paragraph packed 4 independent return-value facts into one block. | Converted to a 2-column table (function → return value), matching the §7.1 table pattern. | None. This pair was already close to the guide's own canonical example (§9.3 cites this file's Korean §8 verification-requirements section directly) — confirmed the rest already holds: §3.1/§3.2 term intro, §4.1 scope-first, §4.4 contract/implementation split, §6.2 scoped numbers, §8.3 inline struct-field comments, §9.3 interface-observation-only verification section. |
| 03-errors.ko.md / .en.md | §8.3 — `EFSM`, `ENOCOMPATPROTO`, `ETERM`, `EMTHREAD` were declared without inline meaning comments while sibling errno values (`ESTALE`, `EALREADY`, `EDEADLK`, `ESHUTDOWN`, `EPROTOTYPE`, `EOVERFLOW`) had them; meanings existed only scattered in later errno-mapping tables. | Added inline comments sourced verbatim from the existing mapping table (no new claims introduced). | See CONTRACT-QUESTIONS below (`generation` term ambiguity) — left unresolved, not a wording fix. |
| 04-events.ko.md / .en.md | §3.3 — public field `transport_lane` referred to as "Application lane" in the event-field table and in §6 prose, inconsistent with the correct `transport_lane` usage already present elsewhere in the same section. | Replaced all 3 occurrences per language with backticked `` `transport_lane` ``. | None — rest of doc already conforms to §3.1/3.2/3.5/4.1/4.2/4.3/5.1/6.2/6.3/7.1/8. |

## CONTRACT-QUESTIONS

- `core/doc/spec/core/03-errors.ko.md:366` (mirrored in `03-errors.en.md`): the `ZLINK_REQUEST_CONFLICT`
  row reads "request correlation 또는 generation 충돌" ("request correlation or generation
  conflict"). It is unclear whether this "generation" is the same concept as
  [`glossary.ko.md#generation`](/home/hep7/project/zlink/core/doc/spec/core/glossary.ko.md)
  (directional-queue recreation version) or a distinct request/completion generation counter.
  Left unlinked and unchanged — linking it to the glossary term would risk pointing readers at
  the wrong concept if the two are actually different, and merging them silently would be a
  content change outside this task's scope. Needs a decision from someone with domain context
  on the request/reply dispatch internals.

No other suspicious or ambiguous normative statements were found in any of the seven file pairs.

## Headings added

- None. Only standard bottom navigation lines (§2.2, not new headings) were added to
  `00-public-contract-governance.ko.md` / `.en.md`.

## Notes on scope not acted on (recorded, not fixed)

- Repo-wide missing bottom-nav-line convention gap (§2.2) beyond this task's 7 file pairs —
  out of scope for this pass since fixing only these files would desync from other untouched
  spec files.
- Glossary's compact-table-vs-registration-table format (§3.4) — judged intentional design,
  not raised as a defect.
