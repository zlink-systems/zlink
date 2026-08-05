# Round 198: 요청 timeout 취소 wakeup 후보 반려

## 조사 범위

- Core runtime: `core/build/lib/libzlink.so.10.6.0`
- 비교 조건: tcp, 64바이트, 100 peer, active 5초, 양쪽 I/O thread 1개
- 비교 기준: peer별 process·context·ROUTER·I/O thread가 같은 paired pattern
- 판정 방식: correctness 집중 회귀 뒤 후보 1회 paired smoke

Round 197의 5회 중앙값에서 PUBSUB은 ROUTER의 79.44%였지만 REQREP와 SENDSEND는
각각 64.01%, 52.18%였다. 세 패턴의 공통 wire 송신 비용과 별개로 echo 두 패턴에만
operation 등록·terminal completion·timeout 취소가 존재하므로, 이번 라운드는 이 차이를
먼저 조사했다.

10-peer pattern trace에서는 수신 메시지당 `futex` 호출이 PUBSUB 약 2.43회, REQREP
약 8.69회, SENDSEND 약 4.86회였다. 같은 방향 ROUTER REQREP의 이전 matched trace는
약 4.89회였다. 이 수치는 함수별 CPU 비중이 아니라 동기화 경계를 찾는 보조 자료다.

코드를 대조하면 성공한 remote request operation을 완료할 때마다 timeout scheduler에서
등록 task를 제거한 뒤 전역 condition variable을 깨운다. 취소는 남아 있는 가장 이른
deadline을 더 이르게 만들 수 없으므로 timeout 정확성을 위해 이 wakeup이 필요하지는 않다.
반면 PUBSUB에는 request operation과 timeout task가 없으므로 이 후보의 직접 개선 대상이
아니다. 세 패턴에 같은 효과가 생기지 않는 이유를 이 경계로 분리했다.

trace 자료:

- `/home/hep7/.cache/zlink-core-validation/round174-spot-pubsub-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round174-spot-reqrep-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round174-spot-sendsend-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round170-router-reqrep-c10.strace`

## 후보와 correctness

`request_timeout::cancel()`이 schedule에서 task를 제거한 뒤 scheduler condition variable을
깨우지 않도록 바꿨다. 새로 더 이른 deadline을 추가하는 `schedule()`의 기존 알림은 유지했고,
timeout 값, callback 대기, task 상태와 공개 API는 바꾸지 않았다.

별도 Debug build에서 다음 집중 회귀가 모두 통과했다.

- `unittest_request_timeout_scheduler`: 1/1
- `test_mesh_node_basic`: 1/1
- `test_mesh_stress`: 1/1
- `test_mesh_monitor_matrix`: 1/1

후보 공식 runtime SHA-256은
`b49afe9cccd5dd7c23046bc856ac4acf0a6e3574d7d617cefdd836fa95b94739`였다.

## paired smoke와 판정

결과:
`bindings/c/perf/results/multi/paired/20260720-185656-s9-p02-timeout-cancel-no-wake-candidate/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 3,483,114.0 msg/s | 4,241,046.6 msg/s | 82.13% | 2.0230 | 1.9936 | 2.3728 |
| REQREP | 65,531.8 ops/s | 108,381.2 ops/s | 60.46% | 5.8891 | 2.6376 | 3.2877 |
| SENDSEND | 67,995.0 ops/s | 121,439.2 ops/s | 55.99% | 1.1246 | 2.5783 | 3.2926 |

Round 197 중앙값과 비교하면 Spot 절대 처리량은 REQREP 약 8.4%, SENDSEND 약 9.1%
높았다. 그러나 REQREP 처리량 비율은 64.01%에서 60.46%로 낮아졌고 mean·p95·p99도
함께 개선되지 않았다. SENDSEND도 지연 세 항목을 모두 통과하지 못했다. PUBSUB은 후보의
직접 대상이 아니며 단발 변동만 관측됐다. 종료 snapshot에는 PUBSUB application message
230개와 14,720바이트도 남아 있어 수신 완전성 gate 역시 통과하지 못했다. REQREP와
SENDSEND의 pending queue와 세 패턴의 multicast drop은 0이었다.

유지 조건인 Spot 절대 처리량, 대응 ROUTER 비율과 지연의 동시 개선을 입증하지 못했으므로
정식 5회 중앙값으로 확장하지 않고 후보 hunk만 원복했다. 원복 뒤 공식 runtime SHA-256은
Round 197과 같은
`a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이며,
`core/src`와 `core/include`에는 runtime보다 새로운 파일이 없다.

이번 라운드에서는 timeout 값, assertion, version과 package를 변경하지 않았다. 외부 배포와
bindings 내부 package 배포도 수행하지 않았다. S9-P02와 S9-P03은 계속 진행 중이다.
