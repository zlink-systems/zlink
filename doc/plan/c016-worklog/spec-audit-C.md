# Spec audit — core/doc/spec/core/socket (README, PAIR, PUB, SUB, XPUB, XSUB)

Audited against `doc/principal/documentation/spec-writing-guide.ko.md` §3–§8. Work done by 6
parallel sub-agents, one per file pair (ko/en edited in parity). Aggregated and validated here.

## Results table

| File | Violations found (guide §) | Fixed | Deferred |
|---|---|---|---|
| `socket/README.ko.md` / `.en.md` | §3.5 compressed noun phrase in the RID-duplicate-policy paragraph ("exact transport pair에 묶인 fence"); §3.2 missing glossary first-links for `generation` and `directional queue` | Rewrote the fence phrase as an action sentence without changing the normative meaning (reply cannot complete on the pre-handover direction; request always ends via its own timeout); added glossary links for `generation` and `directional queue` in both languages | None |
| `socket/01-pair.ko.md` / `.en.md` | §4.3/§9.3 duplication — `zlink_send_part` prose re-enumerated NONE/DONTWAIT → ID/completion outcomes already stated in §5; receive-flow monitor paragraph re-listed constants already in §5's group; ko/en parity defect (ko said "the part before last", en said "every part before the last" — different claims); §9.3 one bullet packed 3 independent tests for `DONTWAIT FINAL` | Compressed send-part prose to mechanism-only + link to §5/README; compressed monitor paragraph to a link; fixed ko to match en's stronger correct claim; split the 3-test bullet into 3 bullets | §7.2 sequence diagram for the SNDTIMEO-expiry branch (judged low benefit, risk of re-duplicating §5); recv-side NULL/`has_more_out_` restatement next to signature (judged acceptable per §8.1 interface placement) |
| `socket/02-pub.ko.md` / `.en.md` | §4.2 — body used `zlink_socket_set_receive_flow_state()` and PUB monitor flow fields without linking to the owning docs (Socket Common README, Monitoring), unlike sibling docs 06-dealer/07-router | Added relation-table row + inline links to README §`zlink_socket_set_receive_flow_state` and `../06-monitoring.{ko,en}.md` | Missing default values in inline comments for `ZLINK_PUB_OPT_VERBOSE`/`VERBOSER`/`MANUAL` (§8.3 wants defaults) — not invented since source doesn't state them (§2.6); no glossary entries for "topic"/"multipart"/"publish record" (out of scope, glossary file not owned by this pair) |
| `socket/03-sub.ko.md` / `.en.md` | §3.1/§3.3 terminology drift — local text called SUB's Auto HWM classification a "`recv_ingress` policy class" while the owning Auto HWM spec and the same paragraph use "역할"/"role" | Replaced "policy class" with "역할"/"role" in both files | Missing bottom nav-footer line (§2.2) — deferred because every sibling file in the socket directory (02-pub, 04-xpub, 05-xsub, etc.) also lacks it; fixing only this pair would create directory-wide inconsistency, recommend a separate cross-file pass |
| `socket/04-xpub.ko.md` / `.en.md` | §2.5 — three dense paragraphs each bundling 3+ independent judgments (`zlink_publish_part` topic/size/multipart rules; consume/reuse + DONTWAIT rules; `zlink_xpub_recv_part` output-param meanings + storage warning + overflow error) | Split into separate paragraphs at judgment boundaries; wording unchanged | None |
| `socket/05-xsub.ko.md` / `.en.md` | None found after full checklist pass | — (no edits made; file already compliant) | None |

## CONTRACT-QUESTIONS

- `01-pair.ko.md:167` / en `169` vs. `01-pair.ko.md:195-196` / en `192-193` — §5's closing line
  says "Socket 공통이 소유한다" (Socket Common owns verification of ownership transfer), but §5's
  own "1:1 송수신"/"1:1 send and receive" group already contains an ownership-transfer verification
  bullet (`zlink_msg_close` exactly-once rule). Internal contradiction about which document owns
  that verification item. Left unchanged — resolving it means picking an ownership side, not a
  clarity rewrite.
- `03-sub.ko.md:231-233` / en `229-231` (`zlink_subscription_at`) — states the index is over a
  "snapshot taken at query time" but doesn't state the atomic/concurrency relationship between
  that snapshot and `ZLINK_SUB_OPT_TOPICS_COUNT` (§6.3 asks for atomic scope of state reads). No
  normative statement added — the source material contains no explicit guarantee, and inventing
  one would violate §2.6. Flagged for the API owner to confirm whether a guarantee exists.

## Headings added

None — no new headings were introduced in any of the six file pairs. All edits were in-paragraph
rewrites, paragraph splits, and inline glossary/cross-document links.

## Validation performed

- `git diff --check` on all 12 files: clean (exit 0), no whitespace errors.
- `git status --porcelain -- core/doc/spec/core/socket/`: only the 6 assigned pairs plus 3
  pre-existing modified files (`06-dealer`, `07-router`, `08-stream`) that were already dirty at
  session start (unrelated RID-duplicate-policy work) and were not touched by this audit.
- Confirmed new/touched link targets exist on disk: `README.ko.md#zlink_socket_set_receive_flow_state`
  and `README.en.md#zlink_socket_set_receive_flow_state`; `../06-monitoring.ko.md` /
  `../06-monitoring.en.md`; `../glossary.ko.md#generation` and `#directional-queue` (and `.en.md`
  counterparts).
- Diffstat summary: README (+14/-6 ko, +19/-8 en), 01-pair (+26/-13 ko, +16/-9 en), 02-pub
  (+13/-5 ko, +9/-3 en), 03-sub (+2/-1 ko, +2/-1 en), 04-xpub (+8/-1 ko, +20/-5 en), 05-xsub
  (no changes).
- No files outside the assigned 6 pairs were edited. No git commands that write were run.
