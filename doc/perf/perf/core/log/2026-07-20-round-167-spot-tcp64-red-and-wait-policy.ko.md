# Round 167: Spot tcp 64바이트 red gate와 대기 정책 분리

## 범위

- 실행 시점 source commit: `57fa7ed956ce9817f16f9ae49bc145202dce6c5c`
- Core runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.10.6.0`
- Core runtime SHA-256:
  `345610491e3073f8984a3e6c8bf4eac4cd3b22a117aa1fb1d1cc4d8edb33d755`
- 시작 red gate source tree SHA-256:
  `8c848ca4a957d2d71414f96f2dc7b27ea8c4f55941bf75a3aba095f953ef3253`
- 조건: tcp, 64바이트, 100 peer, active 5초, 양쪽 I/O thread 1개,
  cell마다 5회 교차 실행
- 결과:
  `bindings/c/perf/results/multi/paired/20260720-022620-s9-p02-spot-router-tcp64/`

실행 중 다른 benchmark와 E2E process는 없었다. runner는 각 case마다 같은 Core
runtime 경로를 출력했고, paired runner는 시작·각 case·종료 시점에 runtime과
source tree hash가 바뀌지 않았음을 확인했다.

## 최초 paired red gate

| Spot 패턴 | Spot median | ROUTER median | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|-----------|------------:|--------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 336,976.8 msg/s | 1,168,418.4 msg/s | 28.84% | 1.37배 | 1.33배 | 2.09배 |
| REQREP | 8,328.6 ops/s | 130,324.6 ops/s | 6.39% | 71.03배 | 57.62배 | 84.31배 |
| SENDSEND | 10,012.4 ops/s | 241,073.4 ops/s | 4.15% | 55.23배 | 90.82배 | 117.91배 |

PUBSUB에서는 5회 중 두 번 `multicast_dropped_targets`가 각각 200과 80,873으로
증가했다. 이 결과는 처리량과 지연뿐 아니라 수신 완전성도 실패다. paired gate가
이 값을 판정에 반영하도록 고정 입력 test와 drop gate를 추가했다.

## 일반 PUBSUB 시작 기준

`MULTI_PUBSUB` tcp 64바이트를 같은 머신에서 5회 측정했다.

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_023310_s9-p02-general-pubsub-tcp64-baseline.txt`
- 처리량 median: 3,571,538.2 msg/s
- mean: 1,156.209 ms
- p95: 5,725.739 ms
- p99: 6,552.774 ms

처리량 절대 하한 3.130 Mmsg/s는 만족하지만 p95와 p99 절대 상한은 크게
초과한다. 일반 PUBSUB도 송신 속도와 수신 drain의 불균형을 별도 조사해야 한다.

## peer 수 scaling과 원인 분리

수정 전 1·2·10·100 peer smoke에서 얻은 처리량 비율은 다음과 같다. 1·2·10 peer는
진단용 1회·3초 실행이며 최종 판정값이 아니다.

| peer 수 | PUBSUB | REQREP | SENDSEND |
|--------:|-------:|-------:|---------:|
| 1 | 132.63% | 81.24% | 78.10% |
| 2 | 137.98% | 79.22% | 75.12% |
| 10 | 60.41% | 49.43% | 35.34% |
| 100 | 28.84% | 6.39% | 4.15% |

1·2 peer PUBSUB 비율은 높지만 각각 2,601,631개와 537,079개 target drop이
있으므로 성공이 아니다.

GNU time 결과에서 100-peer Spot REQREP는 약 1,920% CPU와 약 5천만 회의
involuntary context switch를 사용했다. 같은 ROUTER 실행은 136~139% CPU와
32회 이하의 involuntary context switch를 기록했다. 각 Spot child가
`drain_ready(DONTWAIT)`와 `yield()`를 반복하면서 20개 core를 포화시켰고,
완료 record를 전달해야 하는 I/O thread가 CPU를 얻지 못한 것이 첫 번째 병목이다.

## 대안 비교와 선택

첫 번째 대안은 기존 blocking `zlink_mesh_node_drain_ready()`를 child의 대기
경로에서 사용하는 것이다. MeshNode가 이미 소유한 condition variable과
`RCVTIMEO` 계약을 그대로 사용하므로 새 상태와 공개 API가 필요 없다.

두 번째 대안은 ready handler가 별도 condition variable을 깨우도록 perf harness에
callback 상태를 추가하는 것이다. 같은 wakeup을 중복 구현하고 callback 수명과
동기화 책임이 늘어난다.

첫 번째 대안을 선택했다. child process와 context, MeshNode, I/O thread 구성은
바꾸지 않고 wait 정책만 변경했다. Core shutdown은 상태를 `STOPPED`로 바꾸는
경로에서 node condition variable을 `notify_all()`하며, blocking ready drain은
이 상태를 `ESHUTDOWN`으로 반환한다. 10·100 peer smoke도 남은 child와 shutdown
timeout 없이 종료됐다.

## wait 정책 변경 결과

100-peer 1회 smoke에서 다음 변화가 있었다.

| 패턴 | 수정 전 | 수정 후 | 배수 | 종료 snapshot |
|------|--------:|--------:|-----:|---------------|
| REQREP | 8,328.6 ops/s | 60,725.4 ops/s | 7.29배 | pending 0 |
| SENDSEND | 10,012.4 ops/s | 59,215.4 ops/s | 5.91배 | pending 0 |

수정 뒤에도 기존 단일-process ROUTER 기준 대비 비율은 각각 49.05%와 26.19%다.
따라서 wait 정책은 실제 병목이었지만 90% 목표를 닫지는 못했다.

PUBSUB에서는 Spot과 새 ROUTER one-way child가 모두 blocking receive를 사용하도록
맞췄다. 100-peer smoke 결과는 Spot 1,864,876.8 msg/s, ROUTER
4,173,458.2 msg/s로 44.68%였고 target drop은 0이었다. p95는 각각 1.714 ms와
1.092 ms였다.

## 남은 비교 조건 결함과 다음 단계

`MULTI_ROUTER_ROUTER_REQREP`와 `MULTI_ROUTER_ROUTER_SENDSEND` client는 process
하나와 context 하나 안에 socket 100개를 만든다. Spot은 process 100개가 각각
context, MeshNode와 I/O thread를 하나씩 가진다. 따라서 현재 REQREP·SENDSEND
비율은 peer 수만 같고 process/context 자원이 달라 최종 90% 판정에 사용할 수
없다.

이 결함을 닫기 위해 paired gate에서만 선택하는 matched client를 추가했다. 기존
일반 benchmark client와 결과 이름은 바꾸지 않았다. matched client는 peer마다
process, context, ROUTER와 I/O thread를 하나씩 만들고 고유 RID를 사용한다. 부모
process는 각 child의 exact count와 latency 합, 균등 reservoir를 가중 집계한다.

1회 scaling smoke 결과와 raw 자료는 다음과 같다.

| peer 수 | Spot REQREP / matched ROUTER | 비율 | Spot SENDSEND / matched ROUTER | 비율 |
|--------:|------------------------------:|-----:|--------------------------------:|-----:|
| 1 | 5,936.7 / 7,480.0 ops/s | 79.37% | 6,744.7 / 8,663.3 ops/s | 77.85% |
| 10 | 32,429.0 / 55,133.3 ops/s | 58.82% | 35,957.3 / 68,336.7 ops/s | 52.62% |
| 100 | 63,132.4 / 77,917.2 ops/s | 81.02% | 68,947.0 / 122,078.4 ops/s | 56.48% |

- 1 peer:
  `bindings/c/perf/results/multi/paired/20260720-025430-s9-p02-matched-c1/`
- 10 peer:
  `bindings/c/perf/results/multi/paired/20260720-025506-s9-p02-matched-c10/`
- 100 peer:
  `bindings/c/perf/results/multi/paired/20260720-025540-s9-p02-matched-c100/`

100-peer Spot REQREP는 matched 기준에 90%보다 약 9%p 부족하다. SENDSEND는 약
34%p 부족하다. latency도 두 패턴 모두 목표를 만족하지 못한다. 따라서 matched
조건을 반영해도 Core 데이터 경로 개선이 더 필요하다. 앞서 기록한 단일-process
ROUTER 비율은 `unmatched diagnostic`으로만 보존하고 90% gate 근거로 사용하지
않는다.

다음 단계에서는 Core `drain_ready()`와 wire ingress/operation completion 경계를
계측한다. Linux `perf`는 설치되어 있지 않다. Callgrind 1-peer 시도는 계측
오버헤드 때문에 startup timeout이 발생해 정식 profile로 사용하지 않았지만,
종료된 client와 server profile에서 `zlink_mesh_node_drain_ready()`가 각각
21.6%와 13.4%의 exclusive instruction을 차지해 다음 좁은 조사 후보로 남겼다.

P02와 P03은 아직 완료가 아니다. version 변경과 package 배포도 진행하지 않았다.
