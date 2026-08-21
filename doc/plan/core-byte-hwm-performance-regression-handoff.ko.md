# Core Auto HWM 성능 회복 handoff

## 완료 조건

- Auto HWM의 공개 동작과 유한 byte HWM을 유지한다.
- release 0.10.1과 **동일한** benchmark 조건으로 비교한다.
- 각 동일 case의 모든 보고 항목(throughput, bandwidth, mean, P95, P99 latency)이
  0.10.1과 같은 수준이어야 한다. 처리율 95%나 latency 2배 같은 완화 기준은 사용하지
  않는다.
- 판정은 `doc/perf`의 paired 실행 원칙을 따른다. 같은 suite·pattern·transport·size를
  local과 0.10.1에 인접해 반복 실행하고 median을 비교한다. 분리된 전체 matrix 실행이나
  다른 시점의 결과는 참고용일 뿐 완료 증거가 아니다.
- single·multi의 모든 pattern, 지원 transport와 기본 message size를 확인한다.
- local Core에서 Auto HWM on·off·finite manual HWM을 각각 검증한다.
- HWM을 무제한으로 바꾸거나 benchmark 조건을 바꿔 통과시키지 않는다.

## 확정된 구현과 검증

- Application queue의 수락 판단과 credit은 pipe-local byte 합계를 사용한다.
- Frame charge는 `payloadBytes + sizeof(msg_t)`이다. 현재 `msg_t`는 64 B이므로 perf의
  4 KiB payload는 queue accounting에서 4,160 B다.
- Decoder는 payload allocation 전에 session의 inline reservation으로 검사한다.
- 정상 frame hot path에서 context mutex, 전역 map, heap reservation과 context 전체
  frame별 atomic 합계를 사용하지 않는다.
- Snapshot은 조회 시 pipe-local 값을 합산한다. Completion·monitor queue와 retained
  generation 정리는 별도 lifecycle 경로에서 처리한다.
- Balanced profile의 data HWM은 유한 4 MiB다.
- PUB/XPUB monitor attach와 public publish가 동시에 `dist`를 접근하던 ASAN
  use-after-free를 PUB/XPUB lifecycle lock 범위에서 수정했다.
- 다음 Core test가 통과했다.
  - `test_zmp_request_reply`
  - `unittest_auto_hwm_policy`
  - `unittest_zmp_decoder`
  - `test_ctx_options`
  - `test_retained_hwm_credit`
  - `test_router_handover`
  - `test_connect_rid`
- 위 일곱 test를 대상으로 한 `ctest` 재실행은 7/7 통과했다.
- ASAN PUBSUB TCP 64·256 B focused run은 2/2 통과했다.
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260821_234956_asan-final-pubsub-tcp-64-256.txt`
- single runner 결과에는 Core source·version·revision·runtime provenance를 남기도록
  보완했다. 측정 workload는 변경하지 않았다.
- 기존 byte-HWM backpressure 회귀는 `test_router_mandatory_hwm`에 있다. ROUTER의
  byte HWM을 채워 `EAGAIN`을 확인하며, routed multipart도 같은 test에서 확인한다.
- `test_retained_hwm_credit`은 HWM에 막힌 송신이 receive lease release 뒤 재개되는지
  확인하며, 이번 focused Core test에 포함해 통과했다.
- 이번 focused Core test에는 `test_router_mandatory_hwm`을 포함하지 않았다.

## Multi Auto-HWM의 현재 동작

- 기본 multi는 `PERF_CTX_AUTO_HWM_ENABLE=1`, `balanced`다. 수동 HWM·OS buffer override는
  `PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1`인 debug 실행에서만 적용한다.
- multi client 100 CCU는 client process의 단일 ctx에 socket 100개를 만들고, server
  process도 별도 단일 ctx를 사용한다. client 100개를 별도 process 100개로 만들지 않는다.
- 현재 planner는 attached application pipe마다 out·in physical queue를 모두 집계한다.
  따라서 100 연결이면 ctx별로 보통 200 방향을 계획한다. 이는 data traffic이 양방향이라는
  뜻이 아니라, 현 planner의 queue 집계 방식이다.
- 이 환경은 약 11.68 GiB 물리 메모리를 감지하고 balanced 예산으로 ctx당 약 1.168 GiB를
  사용한다. data 방향 200개를 4 MiB로 계획하는 데 800 MiB가 필요하므로, 연결 완료 후
  각 방향의 적용 HWM은 보통 4,194,304 B다.
- HWM은 예약 메모리가 아니라 backlog 상한이다. 단방향 workload에서 반대 방향까지 같은
  예산을 계획하는 것이 적절한지는 별도 설계 판단이 필요하다. 성능 수치를 맞추기 위해
  임의로 제외하거나 HWM을 낮추지 않는다.
- Auto-HWM policy unit test는 예산 배분과 admission counter를 확인하지만, "Auto-HWM이
  계산한 값 적용 → queue를 채움 → `EAGAIN` → drain 뒤 재개"를 하나의 end-to-end 회귀로
  검증하지는 않는다.

## 현재 성능 상태

아직 성능 회복은 완료되지 않았다. 아래 수치는 과거 실행의 참고값이며 paired median
증거가 아니므로 완료 판정에 사용하지 않는다.

- `DEALER_ROUTER_SENDSEND`, TCP 256 B의 격리 기록은 local 121,744 Kops/s,
  release 124,844 Kops/s로 약 97.5%다.
- `ROUTER_ROUTER_SENDSEND`, TCP 256 B의 이전 full-matrix 기록은 local 79,870 Kops/s,
  release 146,556 Kops/s로 약 54.5%다. 이 값은 첫 paired 재측정 대상으로 삼는다.
- request/reply TCP 256 B의 이전 인접 기록은 local 84.453 Kops/s,
  release 93.468 Kops/s로 약 90.4%다.
- local single 전체 matrix 1,260/1,260과 local multi 수동 4 MiB PUBSUB 검증은 완료됐지만,
  0.10.1과의 공식 paired 비교가 아니므로 성능 gate 통과 증거가 아니다.
- release single 전체 실행은 PAIR 뒤 PUBSUB에서 중단됐고, release multi 전체 실행은 일부
  TLS/WSS case가 abort했다. 두 결과 모두 완료 증거로 사용하지 않는다.

## 다음 세션의 작업 순서

1. `doc/perf` 원칙대로 `ROUTER_ROUTER_SENDSEND` TCP 256 B를 local과 0.10.1에 인접한
   paired 3회 실행으로 재측정한다. 자동 HWM 기본값을 유지하고 각 실행의
   `AUTO_HWM_DETAIL`을 함께 보관한다.
2. 같은 방식으로 `DEALER_ROUTER_SENDSEND`와 request/reply TCP 256 B를 재측정한다.
   과거 수치나 서로 다른 전체 matrix의 값을 join하지 않는다.
3. 0.10.1 수준에 미달한 case만 profiler로 Core hot path를 확인하고, 원인을 소유한 Core
   모듈에서 수정한다. 측정 조건, client 수, cooldown, HWM을 성능 수치에 맞춰 바꾸지 않는다.
4. 수정 후 해당 case를 다시 paired 측정한다. 모든 기본 size·transport·pattern으로 범위를
   넓히되, 각 비교는 같은 case를 연속해 실행한다.
5. Auto-HWM의 양방향 queue 집계를 변경해야 한다면, 먼저 단방향 data-plane과
   control/reverse queue의 예산 계약을 결정한다. 이는 benchmark 회복을 위한 임시 조정으로
   처리하지 않는다.
6. Auto-HWM이 계산한 적용 HWM을 실제로 채워 nonblocking send가 `EAGAIN`이 되고, drain
   뒤 송신이 재개되는 end-to-end 회귀를 추가한다. 100 CCU를 그대로 test fixture에 복제하지
   말고, budget·연결 수를 작게 고정해 동일한 planner 분배와 byte credit을 재현한다.
7. 마지막에 최신 Core build, `test_router_mandatory_hwm`을 포함한 focused Core tests,
   `git diff --check`, 그리고 모든 policy-compliant paired 결과를 확인한 뒤에만 완료로
   표시한다.

## 작업 보호

- 현재 branch는 `codex/bindings-0.11.1-performance`이며 dirty worktree를 유지한다.
- 기존 변경과 `gmon.out`은 사용자 작업이므로 수정하거나 삭제하지 않는다.
- commit·push·branch 전환은 요청 전까지 하지 않는다.
