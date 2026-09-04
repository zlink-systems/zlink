# C multi perf pull-completion 정정 요약

## 기준과 범위

- 작업 트리: `/home/hep7hep7/project/zlink-wt-bench`
- 기준 revision: `88de99a0adfd24a22fe5d388d7f44b84e8390505` (detached)
- 구현 변경: `bindings/c/perf/**`만 수정
- Core: 이 worktree의 source를 수정하지 않고 `release --lib-only`, `-j4`로 빌드
- 모든 검증 명령: `ulimit -v 16777216`
- `--core-version`, commit, push, checkout은 사용하지 않음
- 같은 머신의 다른 작업 영향을 피하기 위해 성능 절대값은 비교하거나 판정하지 않음

## 변경 파일

| 파일 | 변경 요약 |
|---|---|
| `bindings/c/perf/common/perf_zlink_part_helpers.hpp` | multipart FINAL을 MORE 전에 초기화하고 모든 initialized message를 정확히 한 번 close. ROUTER의 borrowed RID를 첫 recv 직후 owned 값으로 복사. |
| `bindings/c/perf/multi/common/perf_common.hpp` | stop 전송 공통 정책에 `SNDTIMEO=2000ms`, 최대 5회 bounded retry 추가. |
| `bindings/c/perf/multi/common/perf_multi_client_helpers.hpp` | DEALER/ROUTER SEND completion tracker, socket당 pending 1개 상한, exact ID/context 검증, `POLLCOMPLETION` drain, active/latency phase 분리. |
| `bindings/c/perf/multi/common/perf_multi_metric_header.hpp` | 기존 값은 유지하고 `phase_latency=3` 추가. |
| `bindings/c/perf/multi/common/perf_multi_relay_server.hpp` | ROUTER echo SEND completion drain, immutable retry 1개 상한, POLLIN gating, owned RID, message close/free, 5초 shutdown drain 추가. |
| `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp` | active 포화와 global in-flight 1 latency를 분리하고, socket별 SEND completion exact-ID drain 및 bounded stop 구현. |
| `bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp` | active stop barrier, latency ACK completion drain, multipart record 경계, `PHASE_LATENCY`/`PHASE_DONE`, 10초 runner STOP 대기 구현. |
| `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp` | 모든 SUB의 active backlog barrier와 global in-flight 1 latency ACK 수집 구현. |
| `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp` | PUB completion 비대상 경로 유지, NODROP=1, active barrier 반복, latency ACK gate, bounded stop/종료 구현. |
| `bindings/c/perf/multi/src/perf_multi_router_router_matched_client.cpp` | matched SENDSEND도 active/latency 두 phase로 분리하고, 이미 두 phase인 REQREP의 잘못된 active-count/latency-count 동일성 가정 제거. |
| `bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp` | cap 1, latency phase, completion context/exact-ID/admitted/terminal accounting 회귀 검사 추가. |
| `bindings/c/perf/run_benchmarks_multi.sh` | Core 자동 빌드가 `JOBS` override를 따르도록 수정. |
| `bindings/c/perf/run_comparison.py` | DD phase token과 PUB latency control 전달, server 조기 실패 시 client wakeup, DD bounded result wait 추가. |
| `bindings/c/perf/ci_multi_smoke.sh` | bounded TCP smoke와 pattern×size별 canonical throughput RESULT 검사 신설. |
| `bindings/c/perf/README.md` | CI 실행 예와 two-phase/completion 정책 설명 추가. |

## 설계

### Pull-completion 경계

- 일반 DEALER/ROUTER 전송은 `DONTWAIT FINAL`에만 context와 completion-id output을 전달한다.
- 반환 ID가 0이면 즉시 admission으로 보고 completion을 기다리지 않는다.
- 반환 ID가 nonzero이면 socket당 한 record만 pending으로 유지한다. 반환된 exact ID와 stable context를 저장하고 `POLLCOMPLETION`에서 `zlink_completion_recv(...DONTWAIT)`를 `NO_DATA`까지 반복한다.
- 모든 수신 completion은 정확히 한 번 close한다. `ADMITTED`만 성공으로 인정하고 terminal 결과는 비0 실패로 전파한다.
- multipart MORE에는 context/ID를 붙이지 않고 FINAL에만 붙인다. 로컬 FINAL message는 MORE 전에 초기화해 중간 allocation 실패로 partial record가 남지 않게 한다.

### 측정 phase

- Phase 1은 기존 포화 active window를 유지한다. active deadline 이후에는 새 metric을 세지 않고 이미 제출한 completion과 wire backlog/reply를 drain한다.
- Phase 2는 1초 latency window다. 일반 multi echo와 REQREP는 logical client/socket당 in-flight 1, DEALER_DEALER와 PUBSUB는 route 특성상 global in-flight 1로 제한한다.
- RESULT의 7열 형식과 `throughput`, `bandwidth`, `latency`, `latency_p95`, `latency_p99` 이름 및 gate cell key는 바꾸지 않았다.

### Pattern별 종료

- DEALER_DEALER: active stop N개 수신 후 `PHASE_LATENCY`; latency ACK와 server SEND completion을 모두 drain하고 final stop N개를 받은 뒤 `PHASE_DONE`; client가 `CLIENT_DONE`을 내면 runner가 server에 `STOP`을 보낸다.
- DEALER_ROUTER_SENDSEND/ROUTER_ROUTER_SENDSEND: client와 relay 양쪽 SEND completion을 cap 1로 drain한다. Relay는 pending/outstanding 동안 새 POLLIN을 막고, 정상 STOP 뒤에도 최대 5초 동안 마지막 completion을 pull한다.
- PUBSUB: publish에는 SEND completion이 없으므로 completion API를 호출하지 않는다. NODROP backpressure, 모든 SUB가 확인하는 active stop barrier, sequence별 latency ACK를 사용한다.
- Blocking stop은 한 번에 최대 2초, 최대 5회다. DD/PUB 최종 server control wait는 최대 10초이고 runner의 기존 terminate/kill 상한도 유지된다.

## 검증

| 검증 | 결과 |
|---|---|
| `JOBS=4 bash scripts/build-core.sh release --lib-only` | 성공, local `core/build/lib/libzlink.so.0.16.0` 사용 |
| `cmake --build bindings/c/build -j4` | 전체 C perf target 성공 |
| `bindings/c/build/perf/perf_multi_metrics_test` | 성공 |
| `python3 -m unittest discover -s bindings/c/perf/tests -p 'test*.py'` | 12/12 성공 |
| `python3 -m unittest discover -s bindings/c/perf/single/tests -p 'test*.py'` | 47/47 성공 |
| `bash -n`, `python3 -m py_compile`, CI `--help`, canonical/wrong-source/empty-value AWK 판정 | 성공 |
| CI 누락 진단(`NOT_A_PATTERN`, 4096B) | 예상대로 exit 1; 누락 pattern/transport/size/metric 출력 |
| `git diff --check` 및 변경 경로 검사 | 성공; repo 변경은 `bindings/c/perf/**`뿐 |
| CI smoke, TCP, 8 clients, 2초, 4096B, DD/DR-SENDSEND/PUBSUB, timeout 60 | exit 0; throughput 셀 3/3, RESULT 15/15 |
| CI smoke, 동일 조건, 65536B, timeout 60 | **exit 0, 16초**; throughput 셀 3/3, RESULT 15/15 |
| multi ALL, TCP, 8 clients, 1초, 1024B, 1회 | 7개 pattern 완료, RESULT 35/35 |
| single ALL, TCP, 1초, 1024B, 1회 | 7개 pattern 완료, RESULT 35/35 |
| matched ROUTER SENDSEND/REQREP, TCP, 8 clients, 1초, 1024B | 2개 pattern 완료, RESULT 10/10 |

## QUESTIONS

1. 요청에는 현재 main에서 64KiB가 비0로 60초 이내 빠르게 실패할 것으로 예상되어 있었지만, clean revision `88de99a0ad`의 local Core를 다시 lib-only 빌드한 뒤 64KiB 단독 및 4096→65536 연속 실행 모두 정상 완료했다. 최종 단독 검증은 16초, exit 0, 대상 셀 3/3이었다. 재현에 별도 Core 상태나 fault trigger가 필요한지, 아니면 정상 완료를 더 강한 결과로 인정할지 확인이 필요하다.
