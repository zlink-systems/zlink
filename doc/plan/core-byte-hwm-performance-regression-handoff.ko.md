# Core byte-HWM 성능 회귀 최종 handoff

## 종료 상태

이번 작업은 구현·프로파일링·반복 측정까지 완료했다. 그러나 모든 case의 모든 지표가
release 0.10.1 대비 ±5% 이내라는 최초 목표는 달성하지 못했다. 남은 차이는 한 가지
hot path로 설명되지 않고 transport·message size·실행 순서에 따라 방향도 바뀐다.
검증한 추가 튜닝 후보들은 다른 pattern의 처리량 또는 tail latency를 악화시켰다.

따라서 다음 원칙으로 종료한다.

- 유한 byte HWM과 공개 동작을 완화하지 않는다.
- 성능 수치만 맞추기 위해 queue를 무제한으로 되돌리지 않는다.
- 성능이 중립적이어도 책임과 상태 경계를 명확히 한 POSDDD 리팩터링은 유지한다.
- 한 pattern을 개선하면서 다른 pattern을 5% 넘게 악화시킨 실험은 채택하지 않는다.
- 추가로 적용할 근거 있는 성능 병목 또는 POSDDD 구조 개선 항목은 현재 없다.

## 유지한 구현

- Application queue의 수락 판단과 credit은 pipe-local byte 합계를 사용한다.
- Frame charge는 `payloadBytes + sizeof(msg_t)`이며 현재 `msg_t`는 64 B다.
- Decoder는 payload allocation 전에 session pipe에 byte credit을 예약한다.
- 정상 frame hot path에서 context mutex, 전역 map, heap reservation과 context 전체
  frame별 atomic 합계를 사용하지 않는다.
- ROUTER/DEALER의 raw 경로와 request/reply 상태를 분리하고, routing ID로 이미 수락한
  owner message의 시작 상태를 pipe가 소유한다.
- 완전한 single message는 generic multipart 상태기를 거치지 않는 pipe-local fast path를
  사용한다.
- ROUTER routed receive는 실제 동시 dispatch가 활성화된 경우에만 dispatch lock을 잡는다.
- inbound decoder credit 회복은 writer state가 실제로 비활성에서 활성으로 바뀔 때만
  waiter 상태를 게시한다.
- multipart prefix snapshot은 multipart에만 게시하고, single-frame 완료 counter는
  불필요한 release barrier 없이 기존 message별 정확한 게시를 유지한다.
- TCP speculative synchronous write는 기본 경로로 유지하되 환경 변수로 진단할 수 있다.
- PUB/XPUB monitor attach와 public publish의 `dist` 동시 접근은 lifecycle lock 범위에서
  직렬화한다.
- STREAM raw client는 Core에 링크하지 않는 독립 부하 생성기이므로 multi runner의 Core
  runtime 동일성 검사 대상에서 제외했다. STREAM server는 계속 검사한다.

## POSDDD 적용 결과

- HWM 계산, byte credit, queue snapshot과 writer wakeup 결정은 pipe와 Auto-HWM policy가
  소유한다. 호출자나 benchmark에 내부 queue 정책을 복제하지 않았다.
- routed message 시작, direct terminal frame, retained part/request state를 의미가 속한
  runtime 객체로 이동해 optional state와 lifecycle을 명시했다.
- fast path와 generic multipart path의 계약을 분리하되 public API와 wire protocol은
  변경하지 않았다.
- decoder reservation, completed credit와 writer wakeup 상태를 pipe가 소유한다.
- STREAM 측정 실패의 원인이었던 runtime verifier도 Core-dependent server와 독립 raw
  client의 책임을 구분하도록 수정했다.

## Multi 전체 pattern 측정

### 1차 전체 선별

- non-STREAM 6 pattern × 4 transport × 6 size = 144 case를 local과 0.10.1에서 각각
  격리 실행했다.
- 144/144 case에서 양쪽 모두 결과 5개가 완전하게 생성됐다.
- 한 번 실행에서 하나 이상의 지표가 ±5%를 벗어난 75 case를 후보로 선정했다.

### 후보 반복 비교

- 75 case를 `local/release`, `release/local`, `local/release` 순서로 인접 실행했다.
- 총 450회 실행과 2,250개 지표가 모두 성공했다.
- 3회 중앙값 기준 66/75 case에서 하나 이상의 지표가 ±5%를 벗어났다.
- 지표별 초과 개수는 bandwidth 44, throughput 44, mean 54, P95 54, P99 52였다.
- 실행 순서, 온도와 시스템 부하에 따른 변동이 컸다. profiler를 붙인 동일 case에서는
  local이 release와 같거나 더 빨라 이전 중앙값과 방향이 바뀐 경우도 있었다.

대표 중앙값은 다음과 같다.

| Pattern / transport / size | Throughput gap | Mean gap | P95 gap | P99 gap |
|---|---:|---:|---:|---:|
| DEALER_DEALER / TCP / 4 KiB | -10.13% | +6.67% | 개선 | 개선 |
| DEALER_ROUTER_SENDSEND / TLS / 1 KiB | -21.29% | +26.91% | +25.83% | +26.50% |
| PUBSUB / TCP / 128 KiB | -29.48% | - | - | - |
| ROUTER_ROUTER_SENDSEND / TCP / 256 B | -4.59% | +3.86% | +14.04% | +17.04% |

양수 latency gap은 local이 느리다는 뜻이고, 음수 latency gap은 local이 빠르다는 뜻이다.

### STREAM CCU 100

사용자 지정에 따라 STREAM은 10,000 CCU 대신 100 CCU로 측정했다.

- 4 transport × 4 size × local/release × 3회 = 96회가 모두 성공했다.
- 이 실행 뒤 실험적 completed-credit 64 KiB batch를 진행성 검증 때문에 제거했다.
  따라서 이 STREAM 결과는 병목 판단 자료이며 최종 source의 공식 gate 증거로 사용하지 않는다.
- 16 case 중 10 case는 모든 지표가 ±5% 이내였다.
- 6 case는 절대 차이 기준으로 하나 이상의 지표가 5%를 넘었다. 그중 WS 1 KiB는
  local의 throughput이 8.92% 높고 latency가 8.16~24.66% 낮아 회귀가 아니라 개선이다.
- 주요 실제 회귀는 TCP 64 B의 throughput -13.67%, mean +19.45%, P95 +20.74%,
  P99 +18.33%와 WSS 1 KiB의 throughput -5.77%, mean +6.04%, P95 +5.72%,
  P99 +5.37%다.
- TCP 256 B는 throughput -4.96%와 mean +4.30%로 경계 안이지만 P95 +7.81%,
  P99 +9.05%였다. WS 64 B는 P95만 +5.62%였다.

원시 반복 결과는 작업 환경의 `/tmp/zlink-matrix-repeat/results.tsv`와
`/tmp/zlink-stream-ccu100-repeat/results.tsv`에 있다.

## PUB/SUB 격차 원인

PUB/SUB TCP 128 KiB는 전용 socket 구현 회귀가 아니라 적용 HWM 차이의 영향이 컸다.

- Auto-HWM on에서 local의 유효값은 PUB server 4,096,000 B, SUB client 2,097,152 B였다.
- release 0.10.1은 송신 HWM이 사실상 무제한이었다.
- local Auto-HWM on 중앙값은 26.7 Kmsg/s, release는 33.4 Kmsg/s였다.
- local Auto-HWM off로 양쪽을 4,096,000 B로 맞추자 중앙값이 38.0 Kmsg/s로 올라갔다.
- 대신 local Auto-HWM off의 P95는 449 ms, P99는 1,160 ms로 악화됐다. Auto-HWM on의
  대표 P95/P99는 약 179/235 ms였다.

즉 SUB queue를 늘리면 처리량 격차는 사라지지만 tail latency가 크게 악화된다. 유한 HWM과
낮은 queue residency를 지키기 위해 기본 정책 변경은 채택하지 않았다.

## 프로파일링 결론

- ROUTER_ROUTER TCP 256 B current server에서 send 32.94%, recv 8.43%, `epoll_ctl`
  7.65%, `epoll_wait` 5.88%, write 4.71%가 관찰됐다.
- TLS DEALER_ROUTER 1 KiB는 server와 client를 각각 프로파일했다. profiler 부하 아래에서는
  local이 release보다 빠르고 CPU 사용도 낮아, routed server hot path 하나로 고정된 회귀가
  아니었다.
- per-frame completed credit atomic 게시 비용은 관찰되어 64 KiB 경계 단위 게시를
  실험했다. 일부 throughput gap은 줄었지만 pattern별 결과가 혼합됐고 backpressure
  진행성 검증 중 timeout이 발생해 최종 구현에서는 제거했다.

## 폐기한 실험

다음 항목은 일부 case를 개선해도 다른 pattern의 throughput 또는 tail latency를 5% 넘게
악화시켜 폐기했다.

- ROUTER batch 8/32, route cache 128
- native/single TCP write 강제, speculative sync write 비활성
- inbound poll rate 256/2,048/4,096
- HWM 4 MiB 강제 또는 Auto-HWM 비활성
- generic fairness budget, mailbox single-wake, ZMP threshold, socket send buffer 4 KiB
- completed inbound credit의 64 KiB batch 게시

## 검증 상태

- Core local build와 test build가 성공했다.
- `test_retained_hwm_credit`, `test_helper_recv_part_basic`,
  `test_router_multiple_dealers`, `unittest_pipe_byte_charge`는 4/4 통과했다.
- `test_blocking_generic_dealer_recv_remains_on_transport_pipe`는 기존과 같은
  `blocking DEALER receive created an internal payload queue` 실패가 재현됐다. 이번
  completed-credit 게시 변경 전에도 재현된 항목이며 남은 테스트 실패로 기록한다.
- `test_backpressure_oneway_matrix_single_socket`은 CTest의 120초 제한과 별도 직접 실행
  모두 완료되지 않았다. completed-credit batch를 제거하고 다시 빌드한 뒤에도 동일했다.
  assertion 출력 없이 장시간 진행된 unresolved 진행성 실패다.
- multi runner의 STREAM runtime 검사를 수정한 뒤 local과 release 준비 실행 및 CCU 100
  96회 본 실행이 모두 성공했다.
- single local 전체 matrix 1,260/1,260은 이전 작업에서 완료됐다. release single 전체
  실행은 PUBSUB 구간에서 중단되어 공식 paired 전체 증거는 완성되지 않았다.
- 최종 runtime 재빌드, runner 문법 검사와 `git diff --check`가 통과했다. 마지막 변경을
  제거한 source에서 focused Core test 4개도 다시 4/4 통과했다.

## 보호 사항

- branch는 `codex/bindings-0.11.1-performance`이며 전환하지 않았다.
- 기존 변경과 untracked `gmon.out`을 보존한다.
- commit·push는 수행하지 않는다.
