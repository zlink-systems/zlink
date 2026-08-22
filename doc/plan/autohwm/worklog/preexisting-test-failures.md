# Pre-existing test failure investigation: test_xpub_nodrop / test_zmp_metadata / test_router_multiple_dealers

## Task

Fix three deterministic test failures alleged to reproduce in this worktree:

1. `test_xpub_nodrop` — `core/tests/integration/test_xpub_nodrop.cpp:327`
2. `test_zmp_metadata` — `core/tests/integration/test_zmp_metadata.cpp:506`
3. `test_router_multiple_dealers` — `core/tests/integration/test_router_multiple_dealers.cpp:714`

said to reproduce identically at baseline commit `07bb04fc4f` ("autohwm stage2:
poll for async credit publication in retained cross-thread test").

## Finding: this worktree's HEAD does not reproduce the failures

This worktree's `HEAD` is `1264653381` ("dotnet-zljr-coordinator-fence-unconditional-for-unbound-join").
It is **not** a descendant of `07bb04fc4f`, nor of
`codex/bindings-0.11.1-performance` (tip `e498dd9737`, "autohwm stage3: record
the flow-state design, evidence and checklist rows"), the branch this task's
framing describes as this worktree's lineage:

```
$ git merge-base HEAD 07bb04fc4f
16f896c532e3742717869d1c7019c6237791a968
$ git rev-list --count 07bb04fc4f..HEAD   # commits on HEAD's side only
185
$ git rev-list --count HEAD..07bb04fc4f   # commits on baseline's side only
21
```

Both branches share only a distant common ancestor; HEAD carries an unrelated
185-commit history (dotnet/Java coordinator work) that never merged the
autohwm/flow-state stage-2/stage-3 line the task description assumes. Two of
the ten regression targets named in the task's ctest command,
`unittest_flow_state_frame` and `test_flow_state_paired`, do not exist as
build targets in this worktree at all — independent confirmation that the
"recent flow-state work" mentioned in the task is not part of this worktree's
actual history.

### Verification performed in this worktree (per the task's build recipe)

- Configured `core/build-tests` with `-DZLINK_BUILD_TESTS=ON
  -DCMAKE_BUILD_TYPE=Release`, built with `--parallel 8` (clean rebuild, so
  the statically-linked binaries reflect current `HEAD`).
- `test_xpub_nodrop`: 5 standalone runs, all `PASS` (4/4 sub-tests each run).
- `test_zmp_metadata`: 3 standalone runs (2 direct + 1 via ctest), all `PASS`
  (9/9 sub-tests each run).
- `test_router_multiple_dealers`: 3 standalone runs (2 direct + 1 via ctest),
  all `PASS` (20/20 sub-tests each run).
- `ctest -R '^(test_xpub_nodrop|test_zmp_metadata|test_router_multiple_dealers)$' -j3`: 3/3 passed.
- Full suite `ctest -j8`: **87/87 passed**, 0 failures.
- Regression subset from the task (8 of the 10 named targets exist here):
  `test_zmp_request_reply`, `unittest_auto_hwm_policy`, `unittest_zmp_decoder`,
  `test_ctx_options`, `test_retained_hwm_credit`, `test_router_handover`,
  `test_connect_rid`, `test_router_mandatory_hwm` — **8/8 passed**.
  `unittest_flow_state_frame` and `test_flow_state_paired` are absent from
  `ctest -N` in this worktree (no such targets configured).

No source changes were made in this worktree: there is no failure to fix at
its current `HEAD`, and speculatively editing already-passing accounting code
risked a real regression for no verifiable benefit.

## Root cause at the true baseline (07bb04fc4f), for the record

To confirm the task's premise and to document root cause for whoever owns the
`codex/bindings-0.11.1-performance` lineage, `07bb04fc4f` was checked out into
an independent scratch clone (outside this worktree, so the worktree's
protected/isolated state was never touched) and built/run with the same
recipe. All three failures reproduced immediately and deterministically:

- `test_xpub_nodrop` (`test_pub_blocking_publish_succeeds_while_subscriber_drains_tcp`,
  fails at line 327): `blocking publish timeout: sent=41 recv=41` — the
  blocking-PUB/SUB pair stalls at ~41 of 10000 messages inside the 15s drain
  window (pathologically slow, not merely close to the deadline).
- `test_zmp_metadata` (`test_tcp_decoder_hwm_isolated_by_origin_connection`,
  fails at line 506): `wait_for_current_accounted_bytes(frame_bytes * 2)`
  never becomes true — origin `b`'s frame is not admitted independently of
  origin `a`'s full queue, contradicting the origin-local admission rule in
  `core/doc/spec/core/01-context.en.md` ("Ordinary admission checks only this
  origin-local sum and the applied HWM of that queue. Core does not block
  other queues merely because context `current_accounted_bytes` exceeds
  `effective_core_budget_bytes`.").
- `test_router_multiple_dealers`
  (`test_physical_queue_snapshot_accounts_multipart_once`, fails at line 714):
  `Expected 0 to be greater than 0` — `provisional_accounted_bytes` stays 0
  immediately after `pipe_t::write()` of a `MORE`-flagged frame.

### Mechanism (baseline `core/src/runtime/core/pipe.cpp`)

At `07bb04fc4f`, `pipe_t` carries a `const bool _registry_accounting` set at
construction (`pipe.cpp:239`) from `pipepair()`'s
`resolved_queue_class != physical_queue_class_application` (`pipe.cpp:98-105`).
For the ordinary case — `physical_queue_class_application`, which is the
default for every `pipepair()` call site that does not explicitly ask for a
monitor/completion queue class (`socket_base_endpoint.cpp:183-191`,
`session_base.cpp:325-331`, and the test's own direct `pipepair()` call) —
this evaluates to `false`. `write_inner_unlocked()`
(`pipe.cpp:2165-2198`) and `read_internal()` (`pipe.cpp:711-754`) both gate
every `_physical_queue_registry.account_provisional_frame` /
`commit_message` / `release_committed_frame` call behind
`if (_registry_accounting)`; when it is false, accounting instead runs
through the legacy pipe-local counters (`_out_pipe->write(...)`) and never
touches `ctx_physical_queue_registry_t`. Consequences:

- `zlink_ctx_get_auto_hwm_budget_snapshot()` and its
  `provisional_accounted_bytes` / `current_accounted_bytes` fields read only
  the registry, so for application-class pipes those fields never move —
  exactly `test_physical_queue_snapshot_accounts_multipart_once`'s failure.
- Because origin-local admission for decoded inbound frames
  (`reserve_decoder_frame` in `ctx_physical_queue_registry.cpp`) and the
  legacy per-pipe write-side accounting are two independent ledgers for the
  same application-class direction, the byte-credit signal a decoder relies
  on to admit origin `b` while origin `a` is full can desync from what is
  actually queued, breaking the origin-isolation guarantee
  (`test_tcp_decoder_hwm_isolated_by_origin_connection`).
- The same split-ledger accounting is the most likely source of the observed
  throughput collapse in the blocking-PUB/SUB test: credit release on read
  and admission on write disagreeing about which ledger is authoritative can
  turn steady-state draining into a near-serialized trickle.

This worktree's current `pipe.cpp` has no `_registry_accounting` member at
all — every accounting call in `write_inner_unlocked()` / `read_internal()`
is unconditional on the physical-queue registry, which is consistent with the
"physical queue accounting" section of
`core/doc/spec/core/01-context.en.md` treating the registry as the single
source of truth. That is presumably how (an ancestor of) the fix for these
three failures landed upstream; it did not arrive via this worktree's own
commit history, so there is nothing to cherry-pick from here.

## Summary (3 items)

1. **test_xpub_nodrop** — root cause at baseline `07bb04fc4f`: dual
   write-side/read-side accounting ledgers (`_registry_accounting` gate in
   `core/src/runtime/core/pipe.cpp`) for application-class pipes causing
   near-serialized blocking-publish throughput (`sent=41 recv=41` in 15s).
   Not reproducible in this worktree's `HEAD` (1264653381): 5/5 standalone
   runs pass; the gate is absent from this worktree's `pipe.cpp`. No fix
   applied here — nothing to fix at current `HEAD`.
2. **test_zmp_metadata** — root cause at baseline: the same dual-ledger split
   lets decoder-side admission for one origin connection interfere with an
   independent origin's admission, violating the origin-local HWM rule in
   `core/doc/spec/core/01-context.en.md`. Not reproducible in this worktree's
   `HEAD`: 3/3 standalone/ctest runs pass. No fix applied here.
3. **test_router_multiple_dealers** — root cause at baseline:
   `provisional_accounted_bytes`/`current_accounted_bytes` never move for a
   `physical_queue_class_application` pipe because `pipepair()`'s default
   queue class sets `_registry_accounting = false`, routing writes through
   the legacy pipe-local counters instead of
   `ctx_physical_queue_registry_t`. Not reproducible in this worktree's
   `HEAD`: 3/3 standalone/ctest runs pass, plus the full 87-test suite and
   the 8 available regression targets all pass. No fix applied here.

This worktree's `HEAD` is off the `codex/bindings-0.11.1-performance`
lineage the task assumes (see "Finding" above); whoever maintains that
branch should confirm whether its current tip still exhibits the
`_registry_accounting` defect described here, since the baseline commit named
in the task does.
