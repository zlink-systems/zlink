# Round 17: PUBSUB/SPOT 장기 하락 신호 분리

- goal: 과거 기준 대비 `MULTI_PUBSUB`/`MULTI_SPOT` 64B one-way 하락이 core runtime 회귀인지, perf 측정 의미 변화인지 분리한다.
- 완료 기준: baseline 이후 core hot path와 perf client/server 의미 차이를 읽고, core 수정 후보로 볼 수 있는 안정 신호가 있는지 판정한다.
- 시작 시각: 2026-06-14 18:03:58 +0900
- 기준 commit: `2e327a74a`
- 시작 git status: `bindings/python/samples/*`, `bindings/python/tests/test_sample_alignment.py` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-16 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 반복 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
- PUBSUB 재확인 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_180146_round16_pubsub_repeat.txt`
- 대상 pattern/transport/size: `MULTI_PUBSUB`, `MULTI_SPOT` / `tcp,tls,ws,wss` / `64B`

## 가설

- 가설 1: baseline 이후 PUBSUB/SPOT core fanout, matching, 또는 data-plane 경로가 느려져 과거 기준 대비 큰 하락을 만든다.
- 가설 2: baseline 이후 perf client/server의 수신/종료/측정 방식이 바뀌어 과거 기준과 현재 report가 같은 의미의 처리량을 측정하지 않는다.
- 선택한 가설: 먼저 가설 2를 확인한다. round 16에서 문제 report 대비 10% 이상 반복 결손은 사라졌지만, 과거 기준 대비로는 PUBSUB/SPOT one-way가 크게 낮게 남아 있기 때문이다.

## 읽을 코드와 조건

- core:
  - `core/src/runtime/sockets/pubsub/pub.cpp`
  - `core/src/runtime/sockets/pubsub/xpub.cpp`
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_pub.cpp`
  - `core/src/runtime/services/spot/data_plane/*`
- perf:
  - `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp`
  - `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp`
  - `bindings/c/perf/multi/src/perf_multi_spot_client.cpp`
  - `bindings/c/perf/multi/src/perf_multi_spot_server.cpp`

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 장기 하락 신호가 core 수정 대상인지 먼저 분리하는 read-only 감사다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: read-only 감사이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 없음. 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 결과

- perf 변화:
  - `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp`: baseline 이후 receive loop가 deadline 기반 `zlink_poller_wait(..., timeout_ms)`에서 stop token/phase 종료를 기다리는 `zlink_poller_wait(..., -1)` 중심으로 바뀌었다.
  - `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp`: active phase 종료 시 stop token publish가 추가됐고, EAGAIN 시 짧은 POLLOUT 대기 대신 send close 후 다음 루프로 넘어가는 구조가 됐다.
  - `bindings/c/perf/multi/src/perf_multi_spot_client.cpp`: recv worker 구조, per-slot recv mutex, phase duration/grace 계산, ready control coordination이 baseline 이후 변경됐다.
  - `bindings/c/perf/multi/src/perf_multi_spot_server.cpp`: phase duration/probe/cooldown 처리와 control timing이 baseline 이후 변경됐다.
- core PUBSUB 확인:
  - `core/src/runtime/sockets/pubsub/pub.cpp`: PUB attach와 recv 불가 의미는 baseline과 같다.
  - `core/src/runtime/sockets/pubsub/xpub.cpp`: subscription trie 처리, ready count 갱신, distributor 호출 구조는 baseline과 실질적으로 같다.
  - `core/src/runtime/sockets/internal/dist.cpp`: 현재와 baseline 모두 one matching pipe fast path를 가진다. round 8/11에서 dist hot path 후보를 이미 측정했고 의미 있는 개선은 없었다.
- core SPOT 확인:
  - `core/src/runtime/services/spot/pubsub/spot_pub.cpp`: side-handle publish는 `_publish_sync`를 잡고 `logical_multipart_publish(..., force_sync=true)`를 호출한다.
  - data-plane publish ingress, local fanout, mesh publish 경로는 baseline 이후 큰 내부 재구성이 있다.
  - 그러나 round 16에서 문제 report 대비 `MULTI_SPOT`은 tcp `-3.23%`, tls `+11.09%`였고 ws/wss는 문제 report에 실패/누락되어 직접 비교 대상이 아니었다. problem report 대비 안정적인 10% 결손은 확인되지 않았다.

## 판정

- 과거 기준 대비 PUBSUB/SPOT one-way 하락은 실제로 크다. round 16 병합 기준으로 과거 기준 대비 PUBSUB는 대략 `-25%`에서 `-33%`, SPOT은 `-40%`에서 `-52%` 범위다.
- 하지만 이 하락을 현재 core hot path 회귀로 확정할 수 없다.
  - PUBSUB core hot path는 baseline과 현재가 실질적으로 같고, 현재 반복 측정에서는 문제 report 대비 `-3.88%`에서 `-5.86%` 범위다.
  - SPOT은 core 내부 재구성이 크지만, 현재 문제 report 대비 반복 결손이 없다.
  - PUBSUB/SPOT perf client/server의 측정 종료, poll 대기, stop token, worker/phase control 방식이 baseline 이후 바뀌어 과거 기준과 현재 report가 같은 의미의 처리량인지 보장할 수 없다.
- 따라서 이 라운드에서도 core 소스 변경을 하지 않는다. 과거 기준 복구를 목표로 삼으려면 perf 측정 의미를 먼저 동일하게 만든 read-only 비교가 필요하지만, 계획 범위상 perf 변경은 core 성능 개선으로 계산하지 않는다.

## 다음 후보

- 현재 problem report 대비 10% 이상 반복 결손이 없으므로, 다음 core 변경 후보는 아직 없다.
- 계속 진행한다면 STREAM tcp `-7.11%`처럼 5-10% 사이의 후보는 계획 원칙상 바로 수정하지 말고, 단독 반복으로 10% 이상 재현되는지 먼저 확인해야 한다.
