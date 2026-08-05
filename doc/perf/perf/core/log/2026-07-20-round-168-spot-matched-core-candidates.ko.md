# Round 168: matched Spot Core 후보 검증과 원복

## 범위

- source commit: `57fa7ed956ce9817f16f9ae49bc145202dce6c5c`
- 조건: tcp, 64바이트, 100 peer, active 5초, 양쪽 I/O thread 1개
- 비교 기준: peer마다 process·context·ROUTER·I/O thread를 하나씩 사용하는
  matched ROUTER client
- 정식 판정이 아닌 병목 후보 확인용 1회 smoke

모든 실행은 `core/build/lib/libzlink.so.10.6.0`을 사용했다. paired runner가 시작,
각 case와 종료 시점에 source tree와 runtime SHA-256이 바뀌지 않았는지 확인했다.
각 Spot 종료 snapshot의 pending application·infrastructure message, pending byte와
multicast drop은 모두 0이었다.

## 측정 의미 보정

ROUTER echo 기준은 송신 timestamp부터 echo 수신까지의 왕복 시간을 2로 나눈
one-way 추정값을 기록했다. Spot REQREP와 SENDSEND는 같은 왕복 구간을 2로 나누지
않아 서로 다른 의미의 지연을 비교하고 있었다.

공통 metric helper에 echo와 one-way 구분을 추가했다. Spot REQREP와 SENDSEND는
왕복 시간의 절반을 기록하고 Spot PUBSUB은 기존 one-way 시간을 유지한다. 고정 입력
test는 200ns 왕복이 100ns로, PUBSUB 200ns는 그대로 200ns로 계산되는지 확인한다.
이 보정은 throughput, 수신 건수와 queue 상태를 바꾸지 않는다.

## 100 peer resource profile

matched c100의 GNU time과 수신 건수로 계산한 voluntary context switch는 다음과
같다. 이 값은 함수별 CPU profile이 아니라 다음 조사 범위를 정하는 보조 증거다.

| 패턴 | Spot switch/message | ROUTER switch/message |
|------|--------------------:|----------------------:|
| REQREP | 6.41 | 4.44 |
| SENDSEND | 4.74 | 3.13 |

Callgrind 1-peer 보조 실행에서는 `zlink_mesh_node_drain_ready()`의 exclusive
instruction 비중이 client 21.58%, server 13.38%였다. 계측 오버헤드로 실행 전체가
완료되지 않았으므로 이 비율을 정식 성능 결과로 사용하지 않았다.

## 효과가 없어 원복한 후보

### process-global claim table

첫 번째 대안은 claim owner table을 MeshNode별 `unordered_map`으로 옮기고 node
mutex로 보호하는 방법이었다. 두 번째 대안은 public claim 안에 heap token pointer를
넣는 방법이었지만 allocation과 stale lifetime 복잡성이 커서 선택하지 않았다.

첫 번째 대안을 구현해 Core lifecycle·stress 3개 test를 통과시킨 뒤 측정했다.

| 패턴 | 변경 전 Spot | 후보 Spot | 후보 matched ROUTER | 후보 비율 |
|------|-------------:|----------:|---------------------:|----------:|
| REQREP | 63,132.4 | 64,565.6 | 94,631.8 | 68.23% |
| SENDSEND | 68,947.0 | 63,943.6 | 120,764.6 | 52.95% |

- 결과:
  `bindings/c/perf/results/multi/paired/20260720-030432-s9-p03-node-local-claims-c100/`
- 후보 runtime SHA-256:
  `7f07661c4c68a60269a45f67c9bbebefcf2c78b489b7ea3c40f7a21a29a3fe26`

REQREP 변화는 실행 변동 범위이고 SENDSEND는 낮아졌다. 100 peer 구성도 process마다
MeshNode 하나이므로 process-local 전역 mutex에는 서로 다른 peer가 경합하지 않는다.
이 후보는 원복했다.

### server blocking ready

client에서 효과가 있었던 blocking ready를 echo server에도 적용했다. 대안은 ready
handler와 별도 condition variable을 추가하는 방법이었지만 같은 wakeup 상태를
중복하게 된다. 기존 MeshNode condition variable을 사용하는 쪽을 먼저 시험했다.

REQREP 62,959.4 ops/s, SENDSEND 63,560.0 ops/s로 처리량 개선이 없었다. 이 후보도
원복했다.

- 결과:
  `bindings/c/perf/results/multi/paired/20260720-031207-s9-p03-blocking-server-c100/`

### mailbox intrusive FIFO

다음 공통 비용 후보는 `queued_record_t` allocation 뒤 `std::list` node를 다시
할당하는 mailbox queue였다. 첫 번째 대안은 이미 할당한 record가 ownership link를
가지는 internal FIFO로 list node allocation을 제거하는 방법이었다. 두 번째 대안인
`deque`나 별도 object pool은 completion commit 중 allocation 가능성 또는 allocator
lifetime 복잡성이 더 컸다.

첫 번째 대안은 public 구조와 계약을 바꾸지 않고 FIFO 순서와 allocation 없는
completion commit을 유지했다. Core lifecycle·stress 3개 test와 perf policy
29개 test를 통과시킨 뒤 측정했다.

| 패턴 | 변경 전 Spot | 후보 Spot | 후보 matched ROUTER | 후보 비율 |
|------|-------------:|----------:|---------------------:|----------:|
| REQREP | 63,132.4 | 62,010.0 | 91,725.4 | 67.60% |
| SENDSEND | 68,947.0 | 64,027.8 | 119,451.6 | 53.60% |

- 결과:
  `bindings/c/perf/results/multi/paired/20260720-031938-s9-p03-intrusive-record-queue-c100/`
- 후보 runtime SHA-256:
  `4e2d43989f010a3e279c4a5c36e16c7eb687d9921d3311ef4d19f8c1435f0073`

두 패턴 모두 의미 있는 개선이 없어 이 후보도 원복했다. 원복 뒤 공식 runtime을
다시 만들었고 SHA-256은 시작 runtime과 같은
`345610491e3073f8984a3e6c8bf4eac4cd3b22a117aa1fb1d1cc4d8edb33d755`다.
따라서 이 라운드에서 유지한 Core 변경은 없다.

## 판정과 다음 조사

process-global claim table, server wait 정책과 mailbox list node allocation은 c100
처리량 차이의 주 원인이 아니다. 남은 차이는 pending queue나 drop이 아니라 Spot
application dispatch가 ROUTER보다 메시지마다 더 많은 wakeup과 public dispatch
단계를 거치는 현상과 함께 나타난다.

다음 후보는 wire ingress에서 mailbox admission, readiness wakeup, claim recv와
completion wakeup 경계를 각각 계측해 메시지당 비용을 분리한 뒤 선택한다. 효과가
측정되지 않은 Core 변경은 남기지 않는다. P02와 P03은 아직 완료가 아니며 version
변경과 package 배포도 진행하지 않았다.
