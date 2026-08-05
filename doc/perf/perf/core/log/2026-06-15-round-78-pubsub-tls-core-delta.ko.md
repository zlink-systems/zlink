# Round 78 - PUBSUB/tls core delta

## 이번 라운드 목표

- `MULTI_PUBSUB/tls/64B`에서 May26 기준 대비 남은 current checkout 하락을 줄인다.
- 완료 기준: focused `PUBSUB/tls/64B`가 current round77 `2,271,125.6 ops/s` 대비 `+5%` 이상이고, 보안 하드닝 의미를 약화하지 않으며, 관련 core tests 통과.
- 후보 효과가 `+5%` 미만이면 변경을 남기지 않는다.

## 기준 report

- May26 smoke: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - commit: `1b60c0159`
  - load_avg: `2.88 3.10 3.03`
  - `MULTI_PUBSUB/tls/64B`: `2,537,614.0 ops/s`
- May26 full: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - commit: `1b60c0159`
  - load_avg: `2.16 2.30 2.56`
  - `MULTI_PUBSUB/tls/64B`: `2,623,065.0 ops/s`
- current focused: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133649_round77_current_pubsub_tls_ab_recheck.txt`
  - commit: `6201e5db6`
  - load_avg: `9.62 18.04 14.32`
  - `MULTI_PUBSUB/tls/64B`: `2,271,125.6 ops/s`
- May26 replay on current machine: `/tmp/zlink-1b60-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133611_round77_may26_commit_pubsub_tls_replay.txt`
  - commit: `1b60c0159`
  - load_avg: `17.85 20.48 14.91`
  - `MULTI_PUBSUB/tls/64B`: `2,460,474.8 ops/s`

## 현재 상태

- time: `2026-06-15 13:39:23 KST`
- HEAD: `6201e5db6`
- load_avg: `0.96 10.64 12.09`
- dirty tree: core SPOT recovery changes retained; many unrelated docs modified by other work.
- perf code: 수정하지 않는다.

## 병목 가설

1. May26 이후 core `msg_t` command-body guard가 subscription command handling에 비용을 추가했다.
   - 이 guard는 보안 hardening이므로 되돌리지 않는다.
   - steady-state publish data path가 아니라 연결 초기 subscription path라 hot path 가능성은 낮다.
2. May26 이후 `msg_t::data()`/`size()` accessor inline 변경 또는 security guard가 small-message data path에 영향을 줬다.
   - 보안 의미를 유지하면서 accessor 비용만 줄일 수 있는지 확인한다.
3. `PUBSUB/tls` 하락은 core보다 C perf harness/환경 차이 영향이 섞였다.
   - round77 A/B에서 May26 code도 과거 report보다 낮았지만 current보다 높았다.

## 먼저 검증할 가설

- `msg_t::data()`/`size()` accessor 현 상태를 확인하고, 보안 guard와 무관하게 inline hot path를 회복할 수 있는지 본다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음
- 보안 의미를 유지한 근거: command body size clamp, decoder/message/send guard, WS/WSS pending-copy 제거는 되돌리지 않는다.
- 추가로 실행한 회귀 테스트: round79 후보 검증 뒤 복구 상태에서 focused 11개 테스트 통과.

## 추가 확인

- `msg_t::data()`/`size()`는 이미 header inline 상태라 May26 대비 되돌릴 후보가 없었다.
- command-body clamp는 연결 초기 subscription command 처리에 걸리는 보안 guard이고 steady-state publish data path가 아니므로 되돌리지 않았다.
- `ZLINK_USE_RADIX_TREE`는 현재 `core/build/platform.hpp`에서 꺼져 있어 `xsub_t`는 `trie_with_size_t`를 사용한다.
- `trie_t::check()`는 May26 이후 기능 변경이 없고 이미 반복문 기반이다.
- perf 클라이언트는 빈 구독이 아니라 `"bench"` topic을 구독하므로 empty-subscription 캐시류 후보는 기준 workload에 맞지 않는다.
- round79에서 `_has_empty_subscription` 바깥 load를 relaxed로 낮추는 무상태 후보를 검증했지만 `2,298,855.0 ops/s`로 round78 current 대비 약 `+0.22%`에 그쳤고 되돌렸다.

## 현재 판정

- `PUBSUB/tls/64B` 하락은 May26 commit replay와 current 사이에도 남아 있지만, 지금까지 확인한 retained core delta 중 보안 의미를 유지하면서 `+5%` 이상을 회복할 단순 후보는 찾지 못했다.
- 기존에 시도했던 항목 중 상태나 캐시를 늘리는 후보는 POSD 관점에서 채택하지 않는다.
- 하락 없이 작은 개선만 보이는 후보도 이번 workload에서 노이즈 수준이면 남기지 않는다.
