# Core RR REQREP 64K 성능 조사 결과

## 결론

원인은 single-part REPLY를 completion pipe에 넣을 때 `transport_sync`를 잡고 64K frame의 `write_and_flush`가 끝날 때까지 유지한 경로다. ROUTER requester는 별도 completion lane을 쓰므로 이 per-reply recursive mutex 비용과 긴 임계 구간이 드러났고, DEALER requester의 application lane에서는 같은 크기의 손실이 나타나지 않았다.

수정 뒤 `MULTI_ROUTER_ROUTER_REQREP`의 최종 runs=3 중앙값은 29.199 Kops/s다. 직후 같은 환경에서 측정한 0.15.1의 30.545 Kops/s보다 4.4% 낮아 허용 기준인 -5% 안에 들어왔다. 최종 `MULTI_DEALER_ROUTER_REQREP`는 0.15.1보다 2.2% 높았다.

## 원인과 증거

- 64K에서 HWM을 만난 횟수와 WRITABLE 발행 횟수는 RR 72,469/72,469, DR 70,293/70,293이었다. readiness 확인에 든 누적 시간도 RR 9.21ms, DR 8.65ms로 비슷했다. backpressure token 빈도나 ROUTER route 재조회는 주원인이 아니었다.
- 131,072개 REPLY 계측에서 재시도는 RR과 DR 모두 0회였다. RR completion lane은 route retain에 69.17ms, send에 791.50ms가 들었고 DR application lane은 각각 19.35ms, 525.45ms였다. RR의 completion submit 쪽에서 약 2us/op의 추가 시간이 확인됐다.
- `send_completion_staged_frames_on_pipe`의 `transport_generation_lock`만 제거한 A/B에서 RR은 27.654 Kops/s에서 29.556 Kops/s로 6.9% 올랐다. cache-line 정렬과 atomic reader gate는 같은 폭을 회복하지 못했다.
- callgrind의 10-client/1초 RR 비교는 진단용 registry-off 가설 빌드가 69.19M instructions/1,205 ops, 0.15.1이 69.04M/1,139 ops였다. current 쪽 instruction 증가가 없었으므로 명령 수보다 mutex를 포함한 wall-time 동기화가 차이를 만든다는 A/B 결과와 일치한다.

## 수정

- `core/src/api/socket/socket_request_reply_runtime_io.cpp`: single-part REPLY는 바깥 `transport_sync`를 잡지 않는다. multipart는 여러 pipe lock 구간과 rollback을 묶어야 하므로 기존 generation lock을 유지한다.
- `core/src/runtime/core/pipe.hpp`, `core/src/runtime/core/pipe.cpp`: 기존 `_out_sync` 안에서 예상 connection id가 여전히 같은지 확인하고, 맞을 때만 write와 terminal flush를 한 번에 수행하는 내부 함수를 추가했다. 새 heap allocation, 문자열 조회, public API는 없다.
- `core/src/runtime/core/session_base.cpp`: `engine_error`가 shared connection id를 0으로 만들 때 peer writer의 `_out_sync`도 함께 잡는다. 따라서 single-part REPLY와 teardown 중 먼저 lock을 잡은 쪽만 진행하며, stale generation frame은 pipe에 들어가지 않는다.
- `core/tests/integration/test_router_multiple_dealers.cpp`: 현재 generation은 쓰고 stale/cleared generation은 `EAGAIN`과 inactive admission으로 거절하며 frame을 남기지 않는 회귀 테스트를 추가했다.

## 측정표

조건은 tcp, 100 clients, 5초, 65536B, runs=3 중앙값이다. 로컬 수정 전 대표값은 두 번의 runs=3 중앙값을 평균해 표시했다.

| 구간 | Pattern | 0.17.0 Kops/s | 0.15.1 Kops/s | 처리량 차이 | 0.17.0 mean latency | 0.15.1 mean latency |
|---|---|---:|---:|---:|---:|---:|
| 사용자 D-B87, 수정 전 | RR | 21.600 | 26.200 | -17.5% | 2.410ms | 1.200ms |
| 로컬 재현, 수정 전 | RR | 27.540 | 30.235 | -8.9% | 1.920ms | 1.085ms |
| 최종, 수정 후 | RR | 29.199 | 30.545 | **-4.4% PASS** | 1.765ms | 1.032ms |
| 최종, 수정 후 | DR | 29.960 | 29.313 | +2.2% PASS | 1.796ms | 1.049ms |

최종 raw report:

- 0.17.0 수정 후: `/home/hep7hep7/project/zlink-wt-core-rr/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_024513.txt`
- 0.15.1: `/home/hep7hep7/project/zlink-perf-core-0.15.1/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_024604.txt`

## 검증

- Release LTO lib build: PASS (`JOBS=6`)
- Dev build: PASS (`JOBS=6`)
- 변경 test와 phase3 REQREP: 각 5/5 PASS
- ZMP request/reply receive transaction: 5/5 PASS
- reconnect tcp/tls/out-of-order와 single-lane REQREP 관련 6개 test: 각 5/5 PASS
- 전체 ctest(hotpath_gate 제외): 첫 실행 140/141 PASS. `test_single_lane_flow_snapshot_accounting`의 5초 accounting wait가 한 번 만료됐으나 단독 실행과 이어진 5회 반복은 모두 PASS했다. 수정한 single-part 경로가 아니라 multipart accounting test이며 재현되지 않았다.
- hotpath gate: 4개 cell 모두 PASS. reference 대비 비율은 dealer-dealer 0.9947, dealer-router-reqrep 0.9965, pair 0.9999, router-router-tcp 0.9982다.
- `git diff --check`: PASS

## BLOCKERS

- `perf` 실행 파일이 없고 `/proc/sys/kernel/perf_event_paranoid=2`여서 요청한 `perf record` 비교는 실행할 수 없었다. 가능한 callgrind 비교와 소스 계측으로 대체했다.
- 최종 측정 동안 범위 밖의 장기 실행 perf 프로세스가 남아 있었다: Node TLS server 약 0.5% CPU, 0.15.1 `comp_src_dealer_dealer_client` 약 9.3% CPU. 승인 없이 종료하지 않았다. current와 0.15.1을 바로 이어 같은 환경에서 측정했지만, 완전히 조용한 머신의 재확인은 남아 있다.
- 스펙 변경이나 bindings runner 수정은 필요하지 않았다.
