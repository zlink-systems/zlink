# ZoneWorld .NET golden package

Schema version: `zoneworld-golden/v1`  
Package version: `1.0.0`  
Authority: the .NET ZoneWorld reference implementation.

## Record schema and provenance

Every record contains `scenario_id`, `producer_role`, `consumer_role`,
`normalized_payload`, `raw_artifacts`, and `typed_or_state_oracle`. A raw artifact entry names
the archived file and the selector or absence scan used to derive the record. A log string is
never the sole oracle: every record pairs it with a typed wire, typed state, or store-state
assertion.

## Normalization rules

- Preserve field names and order, null and optional-field presence, literal error kinds,
  scenario ordering, exact counts, absence assertions, and relationship predicates.
- Replace only process-assigned or timing-dependent values with stable tagged placeholders.
  Placeholder classes are `<RID:n>`, `<UUID:n>`, `<ACTOR:n>`, `<PLAYER:n>`,
  `<SESSION_ID:n>`, `<OPID:n>`, `<PROBE:n>`, `<FLOW:n>`, `<CORR:n>`, `<PORT:n>`,
  `<TIMESTAMP:n>`, `<TICK:n>`, `<COUNT:n>`, generation/token tags, and equivalent named tags.
- Reuse the same tag wherever the same value participates in a correlation. Never erase a
  containing field or assertion merely because its value is nondeterministic.
- Runtime bot positions and periodic ticks may vary. The package retains their roster,
  ordering, counts, directions, owner-change predicates, and before/after relationships; only
  the sampled tick/count/value receives a tagged placeholder where needed.
- Redis endpoints, key prefixes, RIDs, lease/lifecycle tokens, and timestamps are masked while
  namespaces, row separation, record version, allocation state, and generation relationships
  remain literal.

ZoneWorld is spec-30-exempt and has no Instance Spot. Consequently the two required Relocation
Store fixtures are negative: clean ZW-B2 and the eight-request pending-through-cutover probe
both complete with zero `relocation:`-prefix access. Positive Relocation Store fixtures belong
to the stage-9 spec-30/Instance Spot lanes.

## Nine sets and port completion bars

The completion-bar text below is copied verbatim from recon §5.

| Golden set | Contents | Port completion bar |
|---|---|---|
| `typed-json-wire-vectors` | Canonical request, reply, push, border, Ops, maintenance, diagnostics, and Follow JSON vectors. | decoder/encoder exact match plus live client receives same semantic payload. |
| `world-state-transcript` | A1-A5/B1/B3/B4 ordered world-state and border-cache semantics. | same ordered transcript at specified steps. |
| `relocation-transcript` | B2/B5/B6/B7/B8 identity, binding, Follow, cutover, and terminal boundaries. | all correlations and generation predicates pass; no source re-resolve/retry evidence. |
| `stage-flow-event-ledger` | Normalized join, ready, restore, cutover, Follow, dispatch, and C4 failure events. | expected ordered partial order and exactly-once handlers. |
| `store-snapshots` | Location authority, Maintenance persistence, and both negative Relocation Store assertions. | same key namespaces/separation and state transition invariants, including both relocation-prefix absence assertions. |
| `ops-fanout-transcript` | C/D/E runtime observation, fanout, maintenance, admission, and persistence semantics. | identical observable states and duplicate/absence assertions. |
| `bot-transcript` | F1-F4 fixed roster, relocation witness, no-push, and reversal semantics. | normalized sequence and negative evidence. |
| `lifecycle-transcript` | G1-G5 generated RID, normal replacement, crash terminal, and fresh-owner semantics. | exact replacement and Unavailable boundaries. |
| `browser-transcript` | Mandatory three-test Playwright lane paired with API/headless typed assertions. | headless test and API/headless transcript both green. |

## Re-extraction and verification

Run a traced full lane into a new empty raw directory with `tools/capture_full_lane.py`, then
run `tools/extract_goldens.py`. After extraction, capture the full lane again as
`raw/verification-full` and run `tools/extract_goldens.py --verify`. Verification checks set
hashes and record shape, the complete phase/browser bar, live typed JSON field order, all four
move-rejection reasons, the G4 `Unavailable` terminal, and both zero-access store assertions.
The capture tools remove only the ZoneWorld `/tmp/tmp.*` run directories that they archive.
