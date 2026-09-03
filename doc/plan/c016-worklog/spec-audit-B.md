# Spec audit B — 05-polling / 06-monitoring / 07-utilities / 08-runtime-boundary

Audited against `doc/principal/documentation/spec-writing-guide.ko.md` (read in full,
lines 1–825). Both ko and en of each pair were read end to end; edits were applied to
both languages to keep parity.

## Summary table

| file | violations found (guide §) | fixed | deferred |
|---|---|---|---|
| 05-polling.ko.md / .en.md | Missing closing nav line at end of document (§2.2). `backpressure` used without glossary link/definition on first use (§3.2). | Both fixed in ko and en. | none |
| 06-monitoring.ko.md / .en.md | Missing closing nav line at end of document (§2.2). | Fixed in ko and en. | none |
| 07-utilities.ko.md / .en.md | None found — already has top+bottom nav, terms link correctly on first use, tables/inline comments already follow §7/§8.3, contract vs. implementation statements already separated. | n/a | none |
| 08-runtime-boundary.ko.md / .en.md | Missing closing nav line at end of document (§2.2). `Auto HWM budget` used without glossary link/definition on first use (§3.2). | Both fixed in ko and en. | `generation`/`pair generation` linking ambiguity (see below); this document's `**bold header**` verification-section formatting vs 06-monitoring's blank-line style (cosmetic, not a guide violation) |

## Detail

### 05-polling.{ko,en}.md
- §2.2 requires the same nav line under the title and at the end of the document. The
  file had it only at the top. Added the identical nav block
  (`[Core 스펙 목차](README.ko.md) | [이전: Events](04-events.ko.md) | [다음:
  Monitoring](06-monitoring.ko.md)`, and the English equivalent) at the end of both
  files.
- §3.2 requires a glossary link plus a same-sentence definition on first use of a
  glossary term. `backpressure` (glossary.ko.md#backpressure /
  glossary.en.md#backpressure) appeared in §3 ("따라서 특정 target의 nonblocking
  submit이 backpressure를 반환한 뒤…" / "…reports backpressure, observing…") without a
  link or definition, even though the same term is correctly linked+defined in
  08-runtime-boundary.md. Added the glossary link and the glossary's own definition
  sentence in place, preserving all normative content (no wording about the
  `ZLINK_POLLOUT` guarantee was changed).

### 06-monitoring.{ko,en}.md
- Same §2.2 nav-line omission as above; added the identical top nav block
  (`Polling` ↔ `Utilities`) at the end of both files.
- Verified `HWM`, `I/O thread`, and `water-filling` — the glossary terms this document
  uses — already link correctly on first use; no other §3.2 gaps found.
- Verified the `ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY = 0x10000016` enum
  value and its no-lost-wake-adjacent paragraphs were left untouched (only line numbers
  shifted by the trailing nav-block insertion).

### 07-utilities.{ko,en}.md
- No violations found. This file already:
  - opens with result/scope in §1 (§4.1),
  - links related-contract owners instead of duplicating them (§4.2),
  - keeps declarations (enum/struct/option) after concept prose and the verification
    section last (§2.2 document shape),
  - uses inline comments for constants/fields rather than a separate table (§8.3),
  - has both top and bottom nav lines already.
- No edits made; file is unchanged in the diff.

### 08-runtime-boundary.{ko,en}.md
- Same §2.2 nav-line omission; added the identical top nav block
  (`Utilities` ↔ `소켓 개요`/`Socket Overview`) at the end of both files.
- §3.2: `Auto HWM budget` (glossary.ko.md#auto-hwm-budget / glossary.en.md#auto-hwm-budget)
  appeared in §3 ("...HWM admission과 Auto HWM budget에서 제외되는 계약은..." / "...HWM
  admission and the Auto HWM budget.") without a link or definition, unlike the correctly
  linked `HWM` term in the same sentence. Added the glossary link and definition
  sentence, without changing which fields are excluded or the ownership statement
  pointing to Auto HWM's spec.
- Confirmed §4.4 separation is already correct: §6 "내부 구조"/"Internal structure" carries
  an explicit "이 절의 계약 소유"/"Contract ownership for this section" callout pointing
  back to §2–§5 and §7, and the "Raw-only 불변 조건"/"Raw-only invariants" bullets read as
  contract-level rules, not implementation narration.

## CONTRACT-QUESTIONS

None. No contract statement in these four files (ko or en) looked wrong, inconsistent,
or unintelligible relative to its counterpart language or to the guide. All normative
content (MUST/MAY semantics, enum names and values including
`ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY = 0x10000016`, error codes, sequences,
sizes, timeouts, ordering guarantees) was left exactly as written.

## Deferred (editorial judgment calls, not fixed)

- **08-runtime-boundary.md `generation` / `pair generation` (ko lines ~166, ~175; en
  lines ~181, ~191).** The glossary defines `generation` narrowly as "같은 방향 queue를
  다시 만들 때 이전 것과 구분하는 버전 번호" (a version number distinguishing a
  re-created directional queue). Two nearby uses don't clearly match that definition:
  (1) "Mesh lifecycle generation, descriptor revision, Actor authority owner generation"
  is a list of *other* systems' concepts that Core explicitly does **not** interpret
  connection identity as — linking here would misleadingly imply Core's own generation
  concept applies. (2) "pair generation" for ROUTER-ROUTER connection-pair validation may
  or may not be the same concept as the glossary's directional-queue generation. Per
  §3.2 ("서로 다른 개념이면 이름과 용어집 항목을 분리한다"), a wrong link is worse than
  no link, so both were left unlinked rather than guessed at.
- **Verification-section bold-header spacing.** 06-monitoring.md's §9 subsections
  (`**Open과 pull 소비**` etc.) have a blank line before the following bullet list; some
  of 08-runtime-boundary.md's §7 subsections do not. This is a cosmetic formatting
  difference across documents, not a guide violation in either file individually, and
  fixing it would produce a large, semantically empty diff against the "keep diffs
  reviewable" hard rule — left as is.
- Did not re-examine table shapes in 06-monitoring.md's §3.2 `value` table or §6.2
  detail-bit table against §5.7's "언제 쓰는가"-first rule: both are reference/lookup
  tables for a value or field the reader already has in hand (not a menu of kinds to
  choose between), so §5.7 does not apply. Same reasoning applies to 05-polling.md's §3
  source-kind table (raw socket / timer / FD) — the caller already knows which source
  type it's using; it isn't choosing among options.

## Headings added

None. No new top-level or sub-level headings were introduced in any of the eight files.
The only structural addition was the closing navigation block (§2.2), which is not a
heading and does not change any anchor.

## Validation

- `git diff --check` — clean for all eight files (no whitespace errors).
- Relative-link existence check — every non-HTTP link target in the eight changed files
  resolves to an existing path (verified via a script that normalizes each link's path
  against the file's directory and checks `os.path.exists`).
- Heading-count parity check (`grep -c '^#\+ '`) confirms ko/en structural parity is
  unchanged by the edits: 05-polling 12/12, 06-monitoring 25/25, 07-utilities 29/29,
  08-runtime-boundary 14/14.
