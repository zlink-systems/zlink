# Round 19: shared one-way core hot path 후보 확인

- goal: 과거 기준 대비 크게 낮게 남은 one-way 64B 계열에서 perf 측정 의미 변경이 아닌 core 공통 hot path 후보가 있는지 확인한다.
- 완료 기준: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER` 64B에 공통으로 영향을 줄 수 있는 core 경로를 읽고, 최소 변경 후보가 있으면 build/test/targeted perf로 검증한다. 후보가 없거나 효과가 없으면 core 변경을 남기지 않는다.
- 시작 시각: 2026-06-14 18:10:27 +0900
- 기준 commit: `f5a7828de`
- 시작 git status: `bindings/c/samples/run_samples.sh` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-18 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 반복 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_180146_round16_pubsub_repeat.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_180820_round18_stream_tcp_repeat.txt`
- 대상 pattern/transport/size: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER` / `tcp,tls,ws,wss` / `64B`

## 현재 수치 요약

- 문제 report 대비 round 16/18 병합 current:
  - 공통 64B 26개 평균 약 `+0.13%`, 중앙값 약 `-0.81%`
  - one-way 64B 14개 평균 약 `-1.88%`, 중앙값 약 `-3.22%`
  - echo 64B 12개 평균 약 `+2.47%`, 중앙값 약 `+1.94%`
  - 10% 이상 반복 결손 없음
- 과거 기준 대비 round 16/18 병합 current:
  - 공통 64B 32개 평균 약 `-13.79%`, 중앙값 약 `-11.44%`
  - one-way 64B 16개 평균 약 `-30.14%`, 중앙값 약 `-25.86%`
  - 큰 하락: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER`, 일부 `MULTI_STREAM`

## 가설

- 가설 1: baseline 이후 message/pipe/mailbox 공통 경로의 작은 메시지 비용이 늘어 `SPOT`, `PUBSUB`, `DEALER_DEALER` one-way에 함께 나타난다.
- 가설 2: baseline 이후 perf client/server의 active window, stop token, poll 대기 의미가 달라져 과거 기준과 현재 수치가 직접 비교 가능한 core 처리량이 아니다.
- 가설 3: `SPOT`은 service/data-plane 재구성 때문에 장기 하락이 core 내부에서 생겼지만, `PUBSUB`와 `DEALER_DEALER`는 다른 원인이라 공통 최적화로는 해결되지 않는다.
- 선택한 가설: 먼저 가설 1을 코드 기준으로 추적한다. 문제 report 대비 반복 결손은 없지만, 계획의 전체 64B 복구 목표에 맞추려면 여러 one-way 계열에 공통으로 남는 core 비용 후보만 수정 대상이 될 수 있기 때문이다.

## 읽을 코드와 조건

- `core/src/runtime/core/msg.*`: 64B 메시지 생성, 복사, refcount 경로
- `core/src/runtime/core/pipe.*`: write/read, flush, check_write, activate_read 경로
- `core/src/runtime/core/mailbox.*`, `core/src/runtime/core/signaler.*`: wakeup과 command 전달 비용
- `core/src/runtime/sockets/internal/lb.*`, `core/src/runtime/sockets/internal/dist.*`: one-way distribute/load-balance 공통 경로
- `core/src/runtime/sockets/pubsub/*`, `core/src/runtime/services/spot/*`: 공통 경로를 벗어나는 fanout 비용

## 변경

- core 소스 변경: 아직 없음
- perf 소스 변경: 없음
- 변경 이유: 수정 전 call path와 병목 가설을 확인한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 읽은 코드

- `core/src/runtime/core/pipe.cpp`: `write_and_flush()`는 final frame에서도 `msg_->is_routing_id()`를 확인하고 `_msgs_written` 증가 여부를 결정한다. `write_single_message_and_flush_no_recursive_hwm_check()`는 이미 STREAM에서 쓰는 final non-routing-id 전용 helper이며, lock/HWM/flush 규칙은 유지하고 `_msgs_written`을 바로 증가한다.
- `core/src/runtime/sockets/internal/dist.cpp`: distributor final helper 적용은 round 11에서 이미 측정했고 transport별 결과가 혼재되어 되돌렸다.
- `core/src/runtime/sockets/internal/lb.cpp`: one active pipe fast path가 있지만 final frame은 아직 `write_and_flush()`를 호출한다. `DEALER_DEALER` steady state는 이 경로를 지난다.

## 적용 중인 후보

- `core/src/runtime/sockets/internal/lb.cpp`
  - one active pipe이고 final frame이며 routing-id가 아닌 경우 `pipe_t::write_single_message_and_flush_no_recursive_hwm_check()`를 사용한다.
  - multipart frame과 routing-id frame은 기존 `write()` / `write_and_flush()` 경로를 유지한다.
  - 기대 효과: `DEALER_DEALER` final data frame에서 중복 분기를 줄인다.
  - 한계: `PUBSUB`/`SPOT` fanout은 distributor나 SPOT data-plane이 주 경로라서 이 변경만으로 전체 one-way 목표를 달성하기 어렵다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음
- 보안 의미를 유지한 근거: 분석 라운드 시작 단계이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 아직 없음

## 검증 예정

- source 후보가 생기면:
  - `cmake --build core/build -j$(nproc)`
  - 관련 `ctest`
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT,PUBSUB,DEALER_DEALER --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round19_shared_oneway_candidate`

## 결과

- 중간 상태:
  - 시작 직후 checkout HEAD가 `adc527b00`으로 이동한 것을 확인했다. 이 라운드의 실제 build/perf report `META,commit`은 `adc527b00`이다.
  - unrelated dirty file도 `bindings/cpp/CMakeLists.txt`로 바뀌어 있었다. perf/core 작업과 무관하므로 건드리지 않는다.
- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- focused ctest:
  - `ctest --test-dir core/build --output-on-failure -R 'test_backpressure_(oneway_)?matrix|test_transport_matrix|test_router|test_zmp_request_reply|test_multi_socket_contract_regressions|test_thread_safe_contract_policy'`
  - 결과: 19/19 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round19_lb_single_final`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_181355_round19_lb_single_final.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - completion: success 4, fail 0, status complete
  - load_avg: `16.24 10.75 5.87`로 높았다. 따라서 좋은 수치라도 재확인이 필요했지만, 실제 결과는 개선 후보가 아니었다.
- round 16 current 대비:
  - `MULTI_DEALER_DEALER tcp 64B`: `2,955,293.2 -> 2,613,760.2`, `-11.56%`
  - `MULTI_DEALER_DEALER tls 64B`: `3,061,520.8 -> 3,039,436.8`, `-0.72%`
  - `MULTI_DEALER_DEALER ws 64B`: `2,976,958.5 -> 3,016,603.8`, `+1.33%`
  - `MULTI_DEALER_DEALER wss 64B`: `3,162,940.1 -> 3,141,779.4`, `-0.67%`

## 판정

- one-active load-balancer final data frame helper 후보는 10% 이상 개선을 만들지 못했고, tcp에서는 10% 이상 하락했다.
- source 변경은 되돌렸다.
- perf 소스 변경 없음.
- rebuild로 `core/build` runtime을 source 상태로 되돌린다.
- restore build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- restored-source refresh perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT,PUBSUB,DEALER_DEALER --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round19_restored_oneway_refresh`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_181947_round19_restored_oneway_refresh.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - completion: success 12, fail 0, status complete
  - report `META,commit`: `cbb6d86c5`
  - 측정 뒤 checkout HEAD 확인: `0678c4baa`. 이 라운드는 core source diff 없이 진행했으므로 HEAD 이동은 성능 후보 판단에 반영하지 않는다.
- round 16 current 대비 restored-source refresh:
  - 12개 평균 `+1.08%`, 중앙값 `+0.09%`
  - 최저 `MULTI_SPOT tls 64B` `-13.71%`
  - 최고 `MULTI_SPOT wss 64B` `+21.61%`
  - transport별 흔들림이 커서 안정적인 개선으로 볼 수 없다.
- 문제 report 대비 restored-source refresh:
  - 비교 가능한 10개 평균 `-5.25%`, 중앙값 `-4.78%`
  - `MULTI_SPOT tcp 64B`만 `-10.02%`로 10% 경계에 걸렸지만, round 16에서는 같은 항목이 문제 report 대비 `-3.23%`였으므로 반복 결손으로 확정할 수 없다.
- 과거 기준 대비 restored-source refresh:
  - 12개 평균 `-33.41%`, 중앙값 `-27.87%`
  - 장기 하락은 여전히 크지만, round 15/17에서 확인한 perf 측정 의미 변화와 이번 round의 transport별 흔들림 때문에 바로 core hot path 회귀로 단정하지 않는다.

## 최종 판정

- 이번 라운드에서 검증한 load-balancer final helper 후보는 실패했다.
- source 변경은 남기지 않았다. 현재 core source diff 없음.
- perf runner/client/server 변경 없음.
- 보안 하드닝 보호 항목은 건드리지 않았다.
- 다음 후보는 `SPOT tcp 64B`를 단독 반복해 10% 이상 결손이 재현되는지 확인하거나, SPOT data-plane의 publish ingress/local fanout 복사 구조를 별도 라운드에서 더 좁게 추적하는 것이다.
