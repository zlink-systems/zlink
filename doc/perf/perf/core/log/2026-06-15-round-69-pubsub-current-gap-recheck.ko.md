# Round 69: PUBSUB 64B one-way 현재 gap 재측정

- goal:
  - `MULTI_PUBSUB` 64B one-way가 corrected May26 기준 대비 현재도 반복 gap인지 확인하고,
    재현되는 전송에 한해 core hot path 후보를 찾는다.
  - 완료 기준: standalone/current 반복에서 하락 전송을 특정하고, POSD-safe core 후보가 있으면
    build/test/perf로 검증한다. 하락이 재현되지 않으면 run-order/load 영향으로 분리한다.
- 시작 시각: 2026-06-15 KST
- 기준 commit: `7ce06becc`
- 시작 git status:
  - core source diff는 SPOT logical queue 및 part-helper restore 계열만 남아 있다.
  - `framework/languages/dotnet/doc/guide/01-overview.ko.md` 변경과 `_workspace/`,
    기존 perf log untracked 파일은 이번 라운드 범위 밖이다.
- corrected baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 문제 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained 기준 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`

## 현재 관찰

- round65 final reduced full의 남은 gap:
  - problem 대비 worst: `MULTI_PUBSUB/tcp,tls,wss`
  - May26 full 대비 worst: `MULTI_PUBSUB/tls,wss,tcp`
- round65 empty-subscription active pipe 후보:
  - 같은-window 기준 `tcp +3.66%`, `tls +1.23%`, `ws -5.19%`, `wss +1.13%`
  - `ws` 하락과 추가 상태 때문에 배제했다.

## 가설

- 가설 1:
  - `PUBSUB` gap은 run-order/load 영향이다. standalone low-load 반복에서는 May26 full 대비
    `-10%` 이상 하락이 약해지거나 사라진다.
- 가설 2:
  - PUB/SUB 64B payload가 LMSG 경로를 타면서 refcount/cache locality가 병목이다.
    하지만 small-LMSG pool 후보는 round 10에서 혼합/하락으로 배제됐다.
- 가설 3:
  - XPUB subscription matching 또는 empty-subscription active pipe 관리가 병목이다.
    repeated-topic cache와 empty-subscription active pipe 후보는 각각 round 30, round 65에서
    효과가 부족하거나 하락 항목을 만들었다.
- 먼저 검증할 가설:
  - 가설 1. source 변경 전 `PUBSUB tcp,tls,ws,wss 64B` standalone 반복을 실행한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 source 변경 전이다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending message, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 후보가 생기면 기록한다.

## Standalone current 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round69_pubsub_all_transport_current_recheck`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_121644_round69_pubsub_all_transport_current_recheck.txt`
- load_avg:
  `0.71 1.54 3.33`
- completion:
  `success=4`, `fail=0`, `status=complete`

| 항목 | May26 full | round69 current | May26 대비 | problem 대비 |
|------|------------|-----------------|------------|--------------|
| `MULTI_PUBSUB/tcp/64` | `2,661,635.6` | `2,534,274.0` | `-4.79%` | `-3.57%` |
| `MULTI_PUBSUB/tls/64` | `2,623,065.0` | `2,297,376.4` | `-12.42%` | `-6.10%` |
| `MULTI_PUBSUB/ws/64` | `2,201,277.0` | `2,118,383.8` | `-3.77%` | n/a |
| `MULTI_PUBSUB/wss/64` | `2,760,571.0` | `2,548,733.2` | `-7.67%` | n/a |

- 판정:
  - low-load standalone에서 `tcp`, `ws`는 동급이다.
  - `wss`는 관찰 구간이지만 `-10%` 반복 회귀는 아니다.
  - `tls`만 May26 full 대비 `-10%` 이상 하락이 재현된다.

## PUBSUB/tls 단독 반복

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round69_pubsub_tls_repeat`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_122011_round69_pubsub_tls_repeat.txt`
- load_avg:
  `1.58 1.79 3.07`
- completion:
  `success=1`, `fail=0`, `status=complete`
- result:
  - `MULTI_PUBSUB/tls/64 = 2,261,513.4`
  - May26 full 대비 `-13.78%`
- 판정:
  - `PUBSUB/tls` gap은 source 후보로 볼 만큼 반복된다.

## 후보: TLS transport speculative write enable

- 후보:
  - `ssl_transport_t`에는 nonblocking `write_some()` 구현이 있지만
    `supports_speculative_write()`는 `false`였다.
  - `PUBSUB/tls` 작은 one-way burst가 async write 경계 비용에 민감할 수 있으므로,
    기존 transport capability를 `true`로 바꿔 검증했다.
- POSD 판단:
  - 새 API나 benchmark shortcut은 아니다.
  - 다만 TLS transport 전체 의미를 넓히는 변경이므로 성능 근거가 약하면 남기지 않는다.
- source:
  - `core/src/runtime/transports/tls/ssl_transport.hpp`
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- focused CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|transport_matrix|multi_socket_contract_regressions|zmp_request_reply|backpressure_oneway_matrix|backpressure_matrix)$'`
  - result:
    6/6 통과.
- perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round69_ssl_spec_write_pubsub_tls_candidate`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_122300_round69_ssl_spec_write_pubsub_tls_candidate.txt`
- load_avg:
  `21.96 11.21 6.34`
- result:
  - `MULTI_PUBSUB/tls/64 = 2,269,351.8`
  - 직전 단독 current `2,261,513.4` 대비 `+0.35%`

## 후보 판정과 원복

- `+0.35%`는 오차 범위이며, 후보 측정 시작 load도 높다.
- TLS transport 전체 capability를 바꾸는 변경을 남길 성능 근거가 없다.
- source 변경은 원복했다.
- 원복 후 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime이 후보 빌드로 남지 않게 했다.
- `git diff -- core/src/runtime/transports/tls/ssl_transport.hpp` 출력 없음.

## Round 69 중간 판정

- `PUBSUB/tls` 하락은 재현되지만, TLS speculative write enable은 개선 후보가 아니다.
- `PUBSUB` socket-level 후보들은 round30/42/65/67에서 이미 반복 배제됐다.
- 남은 후보는 TLS-specific async write batching 또는 SSL transport write handler 비용인데,
  transport-wide 변경은 adjacent TLS patterns까지 볼 수 있어야 한다.
- 이 시점의 source 변경은 없다.

## 후보: ASIO handler allocator를 non-STREAM으로 확대

- 후보:
  - `handler_allocator` 자체는 socket type에 묶이지 않은 ASIO handler allocation 최적화다.
  - 현재 `asio_engine_t`는 handshake/read/write/gather-write handler allocator를 STREAM socket에만
    사용한다.
  - `PUBSUB/tls`는 speculative write를 쓰지 않는 async write 경로라 handler allocation 비용이
    드러날 수 있어, 조건을 모든 socket type으로 넓혀 A/B 측정했다.
- source:
  - `core/src/runtime/engine/asio/asio_engine.cpp`
- POSD 판단:
  - 새 API나 perf shortcut은 아니다.
  - 하지만 기존 이름과 적용 범위가 STREAM 중심이라, 성능 근거가 약하면 일반화하지 않는다.
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- focused CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|transport_matrix|multi_socket_contract_regressions|zmp_request_reply|backpressure_oneway_matrix|backpressure_matrix)$'`
  - result:
    6/6 통과.
- perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round69_asio_handler_alloc_all_pubsub_tls_candidate`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_122719_round69_asio_handler_alloc_all_pubsub_tls_candidate.txt`
- load_avg:
  `21.91 20.35 11.52`
- result:
  - `MULTI_PUBSUB/tls/64 = 2,282,599.8`
  - 직전 단독 current `2,261,513.4` 대비 `+0.93%`
  - all-transport current `2,297,376.4` 대비 `-0.64%`

## 후보 판정과 원복 2

- `+0.93%`는 오차 범위이며, all-transport current보다도 낮다.
- 시작 load가 높아 절대값 신뢰도도 낮다.
- handler allocator 적용 범위를 넓히는 구조 변경을 남길 성능 근거가 없다.
- source 변경은 원복했다.
- 원복 후 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime이 후보 빌드로 남지 않게 했다.
- `git diff -- core/src/runtime/engine/asio/asio_engine.cpp core/src/runtime/transports/tls/ssl_transport.hpp`
  출력 없음.

## Round 69 최종 판정

- `PUBSUB/tls` 하락은 반복되지만, 이번 라운드에서 검증한 TLS output 후보 2개는 모두 효과가 없다.
- 최종 source 변경 없음.
- perf runner/client/server 변경 없음.
- 보안 하드닝 의미 변경 없음.
- 다음 후보:
  - `PUBSUB/tls`만 계속 좁히기보다는 전체 64B 목표에 더 직접적인 후보를 다시 골라야 한다.
  - 현재 남은 큰 미달은 `PUBSUB/tls` 단일 신호와 전체 reduced full 중앙값 미달이다.
