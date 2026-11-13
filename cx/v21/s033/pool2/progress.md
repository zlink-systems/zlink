# Progress

- 2026-11-11: 시작 상태 확인. Branch `perf/1639-pool-exp`, 기존 로컬 커밋 `17c5a80757`; `.pool-build/`은 기존 untracked 빌드 산출물로 유지.
- 기존 base 빌드와 이전 3-round harness를 확인했다. 이번 범위는 send-saturation 1 KiB, variant당 5회이며 base는 재빌드하지 않는다.
- `provider_task_context.cpp`에 선택적 completion queue histogram을 추가했다. 로그 미설정 시 queue depth 추적은 relaxed atomic increment/decrement이고 histogram lock/map 갱신은 하지 않는다.
- union TID 계측을 적용할 copied `analyze-22k.py`는 `/root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py`에 둘 예정이다.
- Copied the 22k analyzer and changed process/thread accounting to the union of before/after TIDs; new after-only TIDs contribute their full counters, and before-only counts are retained.
- Added send-saturation collector/analyzer and copied runner. Each window is 5 seconds during the 5-second active phase, recording both processes' snapshots and perf counters; pool variants write separate source/target queue logs.
- Pool build completed via `cpp-build-slot.sh` without an outer measurement lock. Client and Framework server binaries are newer than the instrumented source and both contain `ZLINK_PERF_COMPLETION_POOL` and `ZLINK_PERF_COMPLETION_QUEUE_LOG` strings.
- Bash syntax and Python compilation checks passed. Starting 20 serial captures (five rounds, rotating base/pool8/pool3/pool2), each under `flock -o /root/jobs/fx/cx/measure.lock`.
- All 20 measurements finished serially with exit 0. Raw `results.json`, fixed-window before/after TSV + `samples.json`, perf CSV, and queue histograms were opened and checked before writing values into the final report.
- Summary generated from raw captures with union TID accounting. Every run recorded one before-only TID for each process; after-only handling is implemented in both analyzers. Pool queue logs have source samples; target has zero samples/max 0.
- Base five-run throughput range is 190,189–194,350 msg/s (median 190,727). Pool3 median 191,407 and pool2 median 191,311 are within that range; each has two individual runs below its minimum. Report presents these as comparison inputs, without a decision.
- Final Korean report written once at `/root/jobs/fx/cx/v21/s033/pool2/report.md`; raw data retained under `measure/`.
- Queue instrumentation committed locally as `df30841b95` (`perf(cpp): record completion queue depth for experiments`); not pushed. Worktree remains on the specified branch; `.pool-build/` remains the existing untracked build directory. No benchmark process remains.
