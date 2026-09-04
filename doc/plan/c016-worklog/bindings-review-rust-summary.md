# Rust binding DONTWAIT/WRITABLE port — review and fix summary

- Tree: `/home/hep7hep7/project/zlink` (main), reviewed commit `85eb9425a1`, Core `50d77800f2` / 0.17.0 (`core/build/lib/libzlink.so.0.17.0`, sha256 `3bfbf1a1…b41db3fb`, not rebuilt)
- Scope touched: `bindings/rust/**` only; no git state changes; `bindings/rust/include` untouched (8 files byte-identical to `core/include`)

## Changed files

| File | Change |
|---|---|
| `bindings/rust/src/internal/completion_owner.rs` | Rewritten drain owner: one lazily started reactor thread per socket (blocks in a native poller on `POLLCOMPLETION`, retires after ~100 ms idle), lazy SEND registration keyed by a never-reused opaque context, parked-WRITABLE replay for the register/drain race, `RwLock` submit gate, generation-safe thread retire, `wait_request` restored; unit tests extended (parked replay, ENOENT/ETERM/ESHUTDOWN terminal mapping) |
| `bindings/rust/src/runtime/messaging/operations/send_ops.rs` | `SendFuture` no longer self-drives: parks on `Pending`, registers only on a wait token, per-part Core shared copies (`zlink_msg_copy`) without a per-attempt `Vec`/`MessageParts` clone, errno captured before the copy is closed |
| `bindings/rust/src/runtime/messaging/operations/routed_async.rs` | REQUEST future/sync paths back to waker/condvar waits (no `yield_now` loop, no re-poll kick); `RequestFuture` keeps its own context for cleanup |
| `bindings/rust/src/contracts/messaging/operations.rs` | Removed the now-unused `MessageParts::try_clone` |
| `bindings/rust/src/internal.rs` | Export `CompletionEntryKind` |
| `bindings/rust/perf/single/src/common.rs` | `submit_now` no longer `yield_now`-spins: the sender thread registers its socket with a public `Poller` (`POLLOUT|POLLCOMPLETION`, `drive_sends_with_poller`) and drives parked SEND futures from `Poller::wait(-1)`; without such a poller it parks on a real waker |
| `bindings/rust/perf/single/src/perf_{pair,dealer_router,dealer_dealer,router_router}.rs` | sender threads call `common::drive_sends_with_poller(&sender)` |
| `bindings/rust/README.rustdoc.md` | Describes the reactor-thread private path (was: "driven by nonblocking executor turns") |
| `bindings/rust/tests/routed_async_tests.rs` | 3 new public-API regression tests; 2 pre-existing flaky REQUEST tests hardened |

## Bug review (checklist a–h)

| # | File:line (as reviewed) | Symptom | Fix |
|---|---|---|---|
| a | `send_ops.rs:224-233` | OK: one DONTWAIT attempt, ID 0 → Ready without waiting; kept | none |
| b | `send_ops.rs:323`, `operations.rs:41` | Payload retained by the Future and only the matching token/context/RID WRITABLE resumes it — correct. But every attempt cloned the whole `MessageParts` (Vec alloc + N `zlink_msg_copy`) | per-part Core shared copy on the stack, no Vec; token matching logic unchanged |
| b | `completion_owner.rs:351-395`, `:397-418` | Every SEND (even immediately admitted) inserted/removed a registry entry, built a `Vec` of REQUEST waiters, and created + destroyed a native poller (`zlink_poller_new/destroy`) per send | lazy registration only when Core issues a token; a WRITABLE pulled before the registration lands is parked by context and replayed (`ParkedWritable`) |
| b/e | `send_ops.rs:202-216`, `completion_owner.rs:485-523` | Executor-turn "reactor": `Pending` + `wake_by_ref()` + timeout-0 `zlink_poller_wait` = 100 % CPU busy loop while parked; nothing else could wake the future (no public poller) | reactor thread per socket blocking in `zlink_poller_wait(POLLCOMPLETION, 25 ms bound)`; futures are woken by the drain. Regression test `backpressured_send_resumes_without_executor_repolls` (poll count ≤ 8 across a HWM→WRITABLE cycle) |
| e/h | `routed_async.rs:85-92`, `:161-183` | `submit_sync` spun with `yield_now`, `RequestFuture` re-polled itself whenever any SEND token was live (`progress_kick`) | restored condvar/waker waits; test `request_alongside_live_send_tokens_is_not_repolled` (poll count ≤ 4) |
| c | `completion_owner.rs:747-764` | TERMINAL WRITABLE mapped via `submit_error_from_errno` (ENOENT→NotFound, ESHUTDOWN/ETERM→Terminated) — correct | public test `removing_the_target_fails_a_parked_router_send` (`disconnect_rid` → parked ROUTER send fails, no permanent wait); unit test now covers ENOENT/ETERM/ESHUTDOWN |
| d | `send_ops.rs:384-395` | NOT_CONNECTED/EHOSTUNREACH without token → immediate `NotConnected` — correct (existing test) | none |
| e | `poller.rs:238-247` | public poller drains through NO_DATA on POLLOUT or POLLCOMPLETION when it owns the queue; REQUEST vs WRITABLE dispatch by context — correct | none |
| f | `completion_owner.rs:314-324`, `:586-590` | `submit_gate` was a `Mutex` held for the whole blocking `submit_sync`: concurrent senders on one socket were serialized behind a parked blocking send | `RwLock` (shared for submits, exclusive for close); close still cannot enter Core with a freed handle |
| f | `completion_owner.rs:237-243`, `send_ops.rs:283-298` | drop/close cleanup: detached tombstones and shutdown settle — correct; with the old design a dropped future's tombstone stayed until socket shutdown because nothing drained | reactor thread drains tombstones; `parked_writables` cleared at shutdown |
| f | `completion_owner.rs:673-693` | thread retire vs stop race | generation check on the owned poller before a thread retires itself |
| g | `native_errors.rs:11-27`, `results.rs` | mapping matches docs | none |
| h | REQUEST/blocking/PUB | blocking send and publish paths unchanged from 0.16 apart from the submit gate; REQUEST semantics restored to pre-port waits | none |
| test | `routed_async_tests.rs:430-460`, `:465-520` | pre-existing flake (3/6 failures on the untouched port): the replier thread dropped the ROUTER right after submitting the reply, racing inproc delivery → `TimedOut` | replier returns the ROUTER; it is dropped only after the reply was consumed (sleep-free); 5/5 + 14/14 green afterwards |

## Hot-path performance review

| Item | Verdict | Fix | Measurement |
|---|---|---|---|
| per-send allocation/copies | defect: `MessageParts::try_clone` (Vec alloc + msg copies) + `Arc<CompletionEntry>` alloc per send | stack `zlink_msg_t` copy per part (Core shared copy; >64 B is refcount only); entry allocated only on first backpressure | see table below |
| per-send locks/map/syscalls | defect: state mutex ×2, HashMap insert/remove, `Vec` of waiters, native poller create/destroy per send | removed; success path = `RwLock` read + atomic context counter + native call | |
| poll/drain loop spin | defect (executor busy loop, `yield_now` loops in sync REQUEST and perf helper) | reactor thread parked in native poller; all waiters park | |
| completion wait structure | HashMap O(1) by context | kept | |

DEALER_ROUTER tcp 1024 B, `run_benchmarks.sh --pattern DEALER_ROUTER --transports tcp --msg-sizes 1024 --duration 3 --runs 1`, same Core runtime and machine, one run each:

| Build | throughput msg/s | bandwidth MB/s | latency avg ms | p95 | p99 |
|---|---|---|---|---|---|
| pre-port binding (70a9998998, worktree `/home/hep7hep7/project/zlink-wt-rustbase`, target `/tmp/zlink-rust-target/base`) | 171888 | 176.0 | 4.002 | 10.844 | 14.582 |
| before review (port 85eb9425a1, executor-turn spin) | 301034 | 308.3 | 0.077 | 0.183 | 0.479 |
| after review, intermediate (reactor thread only; sender parked on waker) | 130124 | 133.2 | 8.226 | 17.041 | 27.285 |
| after review, final (reactor thread in binding; perf sender drives its SEND futures from its own public `Poller::wait`) | 258606 | 264.8 | 2.060 | 5.196 | 8.033 |
| after review, final, second sample from the 2 s smoke run | 281791 | — | 2.100 | — | 8.519 |

Reading:
- The "before" 301k was produced by the sender thread busy-polling the future (every `Pending` turn = a new DONTWAIT attempt with fresh message copies, timeout-0 poller probe) — the busy loop the brief classifies as a defect; its 0.077 ms latency reflects a pipe kept empty by that spin. The pre-port helper polled a 0.16-style future once and dropped it on `Pending` (fire-and-forget), so it never waited for credit either.
- With a parked sender, the wait per HWM episode is Core's LWM-edge WRITABLE plus the wake chain. Two hops (Core → binding reactor thread → sender unpark) cost 0.3–3.8 ms per episode in a micro-probe and gave 130k. Letting the sender thread own a public `Poller` (the application-owned completion path, already supported by the binding) removes one hop: 259–282k msg/s, i.e. above the pre-port baseline and ~86–94 % of the spin number with no spin, sleep or timer.
- A probe with the binding reactor registered for `POLLOUT|POLLCOMPLETION` instead of `POLLCOMPLETION` gave the same 120.8k, so the record does wake the reactor; `POLLCOMPLETION` is kept because a level `POLLOUT` would make the reactor loop while a REQUEST is outstanding on a writable socket. `--hwm 100000` also gave 133k (auto-HWM is in force in this runner), confirming the loss was wake-chain cost, not HWM cycling.

## Smoke gate

- `bash bindings/rust/tests/run_tests.sh` (ZLINK_CORE_SOURCE=local, RELEASE_MODE=0, CARGO_BUILD_JOBS=2, CARGO_TARGET_DIR=/tmp/zlink-rust-target/review): **14/14 PASS** (lib, 12 suites, samples)
- new regression tests: `routed_async_tests` 12/12, run 5 consecutive times green
- `cargo fmt --all -- --check`: PASS; `git diff --check -- bindings/rust`: PASS; header mirror cmp: 8/8 identical
- perf single smoke `--pattern PAIR,DEALER_ROUTER,PUBSUB --transports tcp,inproc --msg-sizes 1024 --duration 2 --runs 1` (final code, status=complete, 30/30 result lines, report `perf_rust_single_linux_*_review_smoke2.txt`):

| pattern | transport | msg/s | lat avg ms | p99 |
|---|---|---|---|---|
| PAIR | tcp | 176383 | 6.43 | 20.13 |
| PAIR | inproc | 346148 | 1.64 | 3.62 |
| DEALER_ROUTER | tcp | 281791 | 2.10 | 8.52 |
| DEALER_ROUTER | inproc | 244532 | 0.92 | 4.20 |
| PUBSUB | tcp | 289925 | 1.88 | 5.60 |
| PUBSUB | inproc | 284237 | 1.34 | 5.28 |

- perf multi smoke (`run_benchmarks_multi.sh --clients 8 --duration 2 --msg-sizes 1024,65536 --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB --transports tcp`; the runner takes CLI options, not CCU/DUR/SIZES env vars):

status=complete, exit 0, 57 s (final binding code; the multi runner's own park-based `block_on_all` executor is unchanged):

| pattern | size | msg/s | lat avg ms |
|---|---|---|---|
| MULTI_DEALER_DEALER | 1024 | 269535 | 50.8 |
| MULTI_DEALER_DEALER | 65536 | 41300 | 6.2 |
| MULTI_DEALER_ROUTER_SENDSEND | 1024 | 68886 | 108.2 |
| MULTI_DEALER_ROUTER_SENDSEND | 65536 | 12424 | 63.6 |
| MULTI_PUBSUB | 1024 | 379093 | 141.2 |
| MULTI_PUBSUB | 65536 | 28542 | 34.7 |

## BLOCKERS / notes for the maintainer

- No BLOCKER. No public API change was needed.
- Perf position: final 259–282k msg/s versus pre-port 172k and spin-port 301k (DEALER_ROUTER tcp 1024 B). The remaining gap to the spin number is the cost of actually waiting for Core's WRITABLE instead of re-attempting on every executor turn. If a managed SEND future is driven by an application executor without a public `Poller`, the binding reactor thread path applies (two wake hops; ~130k in this saturated single-sender benchmark).
- Close semantics: `close()`/drop waits for in-flight native submits (shared `RwLock`), so a blocking `submit_sync` parked with an infinite SNDTIMEO delays close from another thread until it returns. Same as the port (plain Mutex) and consistent with Core's EBUSY lifecycle rule; 0.16 had no gate.
- The reactor thread's 25 ms `zlink_poller_wait` bound (same as the pre-port REQUEST runtime thread) only bounds stop/ownership-transfer latency; it wakes immediately on completion records and retires after ~100 ms without entries.
- Git worktree `/home/hep7hep7/project/zlink-wt-rustbase` (70a9998998, `core/build` symlinked to the main tree) was created for the pre-port measurement as instructed and left in place.
