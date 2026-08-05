# Round 170: Spot 준비 알림 경계 계측과 후보 원복

## 범위

- 조건: tcp, 64바이트, 100 peer, active 5초, 양쪽 I/O thread 1개
- 비교 기준: peer별 process·context·ROUTER·I/O thread를 맞춘 ROUTER 패턴
- 정식 5회 중앙값 판정 전 병목 확인용 1회 paired 실행
- runtime: `core/build/lib/libzlink.so.10.6.0`

Core의 bound-session 역방향 유실 수정과 전체 회귀가 끝난 뒤 현재 runtime으로
성능 시작점을 다시 측정했다. 시작 paired 결과는 다음 경로에 있다.

`bindings/c/perf/results/multi/paired/20260720-065509-s9-p02-round170-current-baseline/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | 판정 |
|------|------------:|----------------:|-----:|------|
| PUBSUB | 1,796,460.0 msg/s | 4,072,678.0 msg/s | 44.11% | 실패 |
| REQREP | 61,704.4 ops/s | 83,676.4 ops/s | 73.74% | 실패 |
| SENDSEND | 47,082.2 ops/s | 79,275.2 ops/s | 59.39% | 실패 |

모든 Spot 실행에서 multicast drop은 0이었다. PUBSUB peer 종료 snapshot에는
application message 92개, 5,888바이트가 남아 있었다. 따라서 이 결과는
처리량 목표뿐 아니라 active 종료 시 queue 잔여도 계속 조사해야 한다.

## system call 보조 계측

10 peer·3초 REQREP에 `strace -c -f`를 사용했다. 계측 오버헤드가 처리량 순서를
바꾸므로 처리량 판정에는 사용하지 않고 호출 경계를 찾는 보조 자료로만 사용한다.

- Spot: `/home/hep7/.cache/zlink-core-validation/round170-spot-reqrep-c10.strace`
- ROUTER: `/home/hep7/.cache/zlink-core-validation/round170-router-reqrep-c10.strace`

Spot은 수신 5,766건에서 `futex` 51,255회와 `sched_yield` 30,179회를 기록했다.
메시지당 각각 약 8.89회와 5.23회다. ROUTER는 수신 약 4,045건에서 `futex`
19,781회로 메시지당 약 4.89회였고 `sched_yield` 항목은 없었다. 이 차이는 Spot
dispatch가 ROUTER보다 더 많은 mutex·condition variable 경계와 서버의 즉시 재시도
반복을 거친다는 조사 근거다. 함수별 CPU 비중을 나타내는 자료는 아니다.

추가로 1 peer·2초 REQREP server를 Callgrind로 실행했다. client는 1,967건을
완료했고 server는 1,968개 request를 처리했다. raw call edge를 다시 집계한
`pthread_mutex_lock` 실제 호출 수는 5,914회로 request당 약 3.00회다. 이전에 호출
수로 해석한 796,863은 lock 함수 내부 instruction 수였다. 전체 명령 실행 수의
11.06%는 lock 함수 자체에서 사용했다. 결과 파일은
`/home/hep7/.cache/zlink-core-validation/callgrind.spot-server.2470219`다. 이 실행은
계측 오버헤드가 크므로 처리량 값은 사용하지 않는다. lock 횟수는 ingress 검증,
reply route와 mailbox admission 경계를 조사하는 보조 근거로만 사용한다.

## 준비 알림 전용 condition variable 후보

두 대안을 비교했다.

1. MeshNode에 준비 알림 전용 condition variable을 두면 공개 API를 바꾸지 않고
   lifecycle·transfer·handler 대기와 blocking drain의 깨움 경계를 분리할 수 있다.
2. 기존 poller signaler를 blocking drain에도 사용하면 FD 수명과 poller 등록 상태가
   내부 대기 계약에 들어가며 한 가지 상태를 두 경로가 함께 소유하게 된다.

첫 번째 대안을 구현해 blocking drain이 shutdown으로 종료되는 집중 회귀를 포함한
Mesh lifecycle test 15개를 통과시켰다. 다음 1회 paired 결과로 효과를 확인했다.

`bindings/c/perf/results/multi/paired/20260720-070727-s9-p02-round171-ready-cv-c100/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | 시작 비율과 비교 |
|------|------------:|----------------:|-----:|-----------------:|
| PUBSUB | 1,904,426.2 msg/s | 4,126,193.0 msg/s | 46.15% | +2.04%p |
| REQREP | 63,568.4 ops/s | 79,191.8 ops/s | 80.27% | +6.53%p |
| SENDSEND | 57,870.2 ops/s | 98,561.2 ops/s | 58.71% | -0.68%p |

REQREP 단발 비율은 높아졌지만 SENDSEND 비율은 낮아졌다. SENDSEND의 Spot 절대
처리량은 22.9% 높아졌으나 같은 실행의 ROUTER도 24.3% 높아져 준비 알림 분리의
효과로 볼 수 없다. 이 후보는 메시지마다 필요한 wait·notify 횟수나 Spot server의
`DONTWAIT` 재시도와 `yield`도 제거하지 않는다. 세 패턴의 공통 병목이라는 증거가
없고 90% 목표에도 미달하므로 전용 condition variable과 회귀를 모두 원복했다.

## 빈 DONTWAIT 조회 fast path 후보

Callgrind에서 확인한 빈 조회의 mutex 횟수를 줄이기 위해 두 대안을 비교했다.

1. server를 poller 또는 blocking drain으로 바꾸면 호출자가 대기 정책을 알아야 하며,
   앞선 blocking server 실험에서도 c100 처리량 개선이 없었다.
2. Core가 보수적인 atomic readiness hint를 소유하면 준비 항목이 없다고 확인된
   `DONTWAIT` 호출만 mutex 획득 전에 `EAGAIN`으로 끝낼 수 있다. 오래된 true 값은
   기존 mutex scan으로 들어가므로 정확성에는 영향을 주지 않는다.

두 번째 대안은 별도 build에서 lifecycle 14/14, concurrent submit·claim과
lost-wakeup stress 3/3, peer 22/22를 통과했다. 공식 `core/build`로 바꾼 뒤 다음
1회 paired 결과를 얻었다.

`bindings/c/perf/results/multi/paired/20260720-072621-s9-p02-round172-ready-hint-c100/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | 시작 비율과 비교 |
|------|------------:|----------------:|-----:|-----------------:|
| PUBSUB | 1,922,480.0 msg/s | 4,102,354.6 msg/s | 46.86% | +2.75%p |
| REQREP | 62,131.6 ops/s | 76,498.4 ops/s | 81.22% | +7.48%p |
| SENDSEND | 65,656.8 ops/s | 114,427.0 ops/s | 57.38% | -2.01%p |

SENDSEND 비율은 낮아졌고 절대 처리량 상승보다 같은 실행의 ROUTER 상승 폭이 더
컸다. PUBSUB과 REQREP도 90% 목표에 미달했으며 mean·p95·p99 지연 gate를 모두
통과하지 못했다. 빈 조회 비용은 실제였지만 이 비용을 줄이는 것만으로 세 패턴의
공통 병목이 개선됐다는 증거가 없으므로 atomic hint도 원복했다.

## 원복 확인과 다음 조사

원복 뒤 공식 `core/build`를 두 번 다시 만들었다. runtime SHA-256은 correctness
gate에서 사용한 값과 같은
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`이며,
`core/src`와 `core/include`에서 runtime보다 최신 파일은 없다.

다음 후보는 condition variable의 종류나 빈 조회만 바꾸는 것이 아니라 메시지당
실제 알림·claim·수신·release 횟수와 payload·wire 처리 비용을 각각 계측한 뒤
선택한다.
효과가 입증된 Core 변경이 없으므로 S9-P02와 이후 정식 5회 중앙값 gate는 아직
완료하지 않는다. version과 package도 변경하지 않았다.
