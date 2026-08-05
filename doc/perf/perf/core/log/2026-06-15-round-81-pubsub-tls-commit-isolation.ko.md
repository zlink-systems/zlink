# Round 81 - PUBSUB/tls commit isolation

## 이번 라운드 목표

- `PUBSUB/tls/64B`의 반복 하락이 May26 이후 어떤 core 변경 구간에서 생겼는지 좁힌다.
- 새 최적화 코드를 추측으로 넣지 않고, 먼저 commit 단위 수치로 원인을 분리한다.
- 완료 기준: 최소 2개 이상 중간 commit을 같은 runner 조건으로 측정하고, 다음 코드 후보 범위를 좁힌다.

## 기준 report

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - commit: `1b60c0159`
  - `MULTI_PUBSUB/tls/64B`: `2,537,614.0 ops/s`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - commit: `1b60c0159`
  - `MULTI_PUBSUB/tls/64B`: `2,623,065.0 ops/s`
- May26 commit replay on current machine:
  `/tmp/zlink-1b60-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133611_round77_may26_commit_pubsub_tls_replay.txt`
  - commit: `1b60c0159`
  - `MULTI_PUBSUB/tls/64B`: `2,460,474.8 ops/s`
- current low-load recheck:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_134041_round78_current_pubsub_tls_lowload_recheck.txt`
  - `MULTI_PUBSUB/tls/64B`: `2,293,853.4 ops/s`

## 시작 상태

- 시작 시각: 2026-06-15 13:55:51 KST
- current HEAD: `e04a9aa88`
- load_avg: `17.49 17.39 14.16`
- dirty tree:
  - core source diff는 SPOT logical queue 및 global part-helper restore 계열만 남아 있다.
  - 문서와 framework 변경은 이번 라운드 범위 밖이다.
- perf code: 수정하지 않는다.

## 병목 가설

1. `PUBSUB/tls` 하락은 May26 이후 특정 core commit에서 생겼다.
   - commit 단위 replay로 `msg_t`/ASIO/TLS/PUBSUB 중 어디를 볼지 좁힐 수 있다.
2. 하락은 현재 checkout의 uncommitted SPOT 복구와 무관하다.
   - SPOT 복구는 PUBSUB/tls data path에 직접 들어가지 않는다.
3. 보안 하드닝 commit이 원인이라도 보안 의미는 유지해야 한다.
   - guard 제거가 아니라 같은 의미를 더 싼 구현으로 바꾸는 후보만 허용한다.

## 먼저 검증할 가설

- 가설 1. May26 이후 중간 commit을 별도 worktree에서 빌드하고 focused `PUBSUB/tls/64B`를 측정한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 없음. 이번 라운드는 commit isolation 측정부터 수행한다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 예정.

## 실행 결과

### `fb4855bc6`

- worktree: `/tmp/zlink-fb4855-pubsub-tls`
- build:
  - `cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release`
  - `cmake --build core/build -j$(nproc)`
  - 통과.
- perf build:
  - `cmake -S bindings/c -B bindings/c/build -DCMAKE_BUILD_TYPE=Release -DZLINK_C_BUILD_BENCHMARKS=ON`
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round81_fb4855_pubsub_tls`
- runner runtime:
  - `/tmp/zlink-fb4855-pubsub-tls/core/build/lib/libzlink.so.6.0.4`
- report:
  - `/tmp/zlink-fb4855-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_140054_round81_fb4855_pubsub_tls.txt`
- load_avg:
  - `2.30 12.36 13.37`
- result:
  - `MULTI_PUBSUB/tls/64B = 2,241,060.6 ops/s`

### `17223f779`

- build:
  - `cmake --build core/build -j$(nproc)`
  - 통과.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round81_17223_pubsub_tls`
- runner runtime:
  - `/tmp/zlink-fb4855-pubsub-tls/core/build/lib/libzlink.so.6.0.3`
- report:
  - `/tmp/zlink-fb4855-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_140536_round81_17223_pubsub_tls.txt`
- load_avg:
  - `2.67 10.46 12.66`
- result:
  - `MULTI_PUBSUB/tls/64B = 2,289,029.0 ops/s`

### `52e5e8b74`

- build:
  - `cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release -DWITH_OPENPGM=OFF`
  - `cmake --build core/build -j$(nproc)`
  - 통과.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round81_52e5_pubsub_tls`
- runner runtime:
  - `/tmp/zlink-fb4855-pubsub-tls/core/build/lib/libzlink.so.6.0.3`
- report:
  - `/tmp/zlink-fb4855-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_141015_round81_52e5_pubsub_tls.txt`
- load_avg:
  - `2.61 10.16 12.35`
- result:
  - `MULTI_PUBSUB/tls/64B = 2,326,630.2 ops/s`

## 판정

- `fb4855bc6`는 이미 낮은 군이므로 command-body clamp 이후만 원인으로 볼 수 없다.
- `52e5e8b74`에서 `17223f779`로 갈 때의 하락은 약 `-1.6%`라 `msg_t::data()/size()` inline 변경 하나로 `PUBSUB/tls`의 큰 회귀를 설명하기 어렵다.
- `1b60c0159` May26 replay `2,460,474.8 ops/s`와 `52e5e8b74` `2,326,630.2 ops/s` 사이에는 약 `-5.4%` 차이가 있다.
  이 구간에는 `bindings/c/perf` refresh가 크게 섞여 있으므로, 이 차이를 core regression으로 단정하지 않는다.
- 따라서 `PUBSUB/tls`만 보고 보안 guard나 core 구조를 되돌리는 것은 근거가 약하다.
- 다음 후보는 `PUBSUB/tls` 전용 특수 분기가 아니라 전체 one-way 64B hot path에서 상태를 늘리지 않는 변경으로 다시 고른다.
