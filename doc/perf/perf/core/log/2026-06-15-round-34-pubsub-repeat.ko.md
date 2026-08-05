# Round 34: PUBSUB 64B repeat

- 목표: `MULTI_PUBSUB` 64B current-vs-problem gap이 반복 가능한지 확인하고, 반복되면 core PUB/SUB hot path 후보를 검토한다.
- 기준 baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최신 full current sweep: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`
- round30 one-way repeat: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_002958_round30_oneway_worst_repeat.txt`

## Current evidence

From round29 full sweep, `MULTI_PUBSUB` current vs problem:
- tcp: `2599897.6`, `-1.07%`
- tls: `2272469.6`, `-7.12%`
- ws: `2216451.0`, `-0.18%`
- wss: `2556827.4`, `-4.59%`

From round30 one-way repeat:
- tcp: `2349093.2`, `-10.62%`
- tls: `2263801.8`, `-7.48%`
- ws: `2146525.8`, `-3.33%`
- wss: `2539648.6`, `-5.23%`

## Hypotheses

1. `PUBSUB/tcp/64` has a repeatable 10% gap, and remaining cost is in XPUB matching/distribution or pipe write path.
2. `PUBSUB/tcp/64` was a round30 variance artifact; standalone repeat will not meet the 10% threshold.
3. PUBSUB long-term baseline drop is real, but current-vs-problem gap is not stable enough to justify a source change.

Selected first check: repeat `PUBSUB` tcp/tls 64B with clean source. If tcp remains worse than problem by at least 10%, inspect PUB/SUB matching/distribution path. If not, do not modify source for PUBSUB this round.

## Initial git state

- core source diff: none at round start.
- untracked previous perf logs exist under `doc/plan/perf/core/log`; leave them untouched.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. 이 단계는 측정만 수행한다.
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize policy를 변경하지 않는다.
- 추가로 실행한 회귀 테스트: source 후보가 생기면 관련 테스트를 기록한다.

## Clean repeat

Command:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round34_pubsub_tcp_tls_repeat
```

Report:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_013105_round34_pubsub_tcp_tls_repeat.txt`

Result:

- `MULTI_PUBSUB/tcp/64`: `2583098.0`
- `MULTI_PUBSUB/tls/64`: `2274165.4`
- failure: `0`

Interpretation:

- `tcp` is `-1.71%` vs problem report `2628109.8`; the round30 `-10.62%` gap did not reproduce.
- `tls` is `-7.05%` vs problem report `2446707.8`; still below the 10% repeated-regression threshold.
- No PUBSUB source change is justified in this round.

## Stream baseline correction carried forward

- User-corrected target: `MULTI_STREAM/tcp/64` from baseline report, not WS.
- Baseline `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`: `400124.6`.
- Problem report: `299395.0`.
- Recent clean current repeats: around `323k` to `337k`; still far below the 400k target.
- Next focus: STREAM/tcp/64 core path, especially packet-handler echo over ASIO TCP.

Clean recheck:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round34_stream_tcp64_clean_recheck
```

Report:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_013449_round34_stream_tcp64_clean_recheck.txt`

Result:

- `MULTI_STREAM/tcp/64`: `335068.0`
- failure: `0`
- runtime: `core/build/lib/libzlink.so.6.0.4`

Baseline comparison:

- Baseline: `400124.6`
- Problem report: `299395.0`
- Current clean recheck: `335068.0`
- Current vs baseline: `-16.26%`
- Current vs problem: `+11.91%`

Important comparison caveat:

- In baseline commit `cb605c6c1`, `bindings/c/perf/multi/common/perf_multi_stream_session.hpp` sent STREAM echo replies without a server-side `send_mutex`.
- In current checkout, the same perf server takes `send_mutex` in immediate send and pending drain.
- This is outside the allowed improvement surface for this task, so no perf source change is made. It does mean the 400k baseline is not a pure core-runtime comparison against the current perf harness.

## Candidate A: packet handler complete-frame view fast path

Change:

- In `core/src/runtime/sockets/stream/stream.cpp`, add a fast path in `stream_dispatch_packet_msg_from_io()` for the case where the incoming `msg_` contains exactly one complete packet frame.
- The candidate used `msg_t::init_view()` for header/body instead of copying through `pipe_t::stream_packet_state_t`.
- Split/coalesced frames stayed on the existing reassembly path.

Validation:

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(multi_stream_server_reassembly|stream_socket|stream_fastpath|stream_threadsafe)$'
```

Result:

- Build: passed.
- Focused ctest: `4/4` passed.

Perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round34_stream_packet_view_fastpath
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round34_stream_packet_view_fastpath_repeat
```

Reports:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_013712_round34_stream_packet_view_fastpath.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_013744_round34_stream_packet_view_fastpath_repeat.txt`

Results:

- Candidate run 1: `325470.0`
- Candidate run 2: `326908.0`
- Clean recheck before candidate: `335068.0`

Decision:

- Candidate A is worse than clean recheck and does not move toward 400k.
- Source change was reverted.
- `cmake --build core/build -j$(nproc)` was rerun after revert and passed.
- Current source diff after revert: none.

## Candidate B: combine current callback send virtual path

Change:

- Temporarily added an internal `stream_dispatch_try_send_current_msg_from_io()` virtual to combine the current-callback routing-id match and direct current-pipe send.
- Goal was to reduce duplicated TLS lookup and virtual dispatch in the packet-handler echo path.

Validation:

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(multi_stream_server_reassembly|stream_socket|stream_fastpath|stream_threadsafe)$'
```

Result:

- Build: passed.
- Focused ctest: `4/4` passed.

Perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round34_stream_current_send_combined
```

Report:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_014128_round34_stream_current_send_combined.txt`

Result:

- Candidate B: `310843.6`
- Clean recheck before candidates: `335068.0`

Decision:

- Candidate B is worse than clean and does not move toward 400k.
- Source change was reverted.
- `cmake --build core/build -j$(nproc)` was rerun after revert and passed.
- Current source diff after revert: none.

## Round 34 conclusion

- PUBSUB tcp/tls did not reproduce a 10% current-vs-problem gap, so no PUBSUB source change was kept.
- STREAM/tcp/64 clean current is `335068.0`, better than problem `299395.0` but still `-16.26%` vs corrected baseline `400124.6`.
- Two core-only STREAM candidates were tested and reverted because both were worse than clean.
- Baseline-vs-current STREAM comparison is affected by a perf harness change: current `perf_multi_stream_session.hpp` serializes echo sends with `send_mutex`, while baseline commit `cb605c6c1` did not.
- Under the active task constraints, that perf harness change is recorded as evidence but not modified.
