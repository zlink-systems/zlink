# #4350 Node L0 request-serial 4K

## CPU profile 차이

셀프 샘플 비율 차이가 1%p를 넘은 프레임이다. 표의 비율과 차이는 각 실행 프로파일에 기록된 샘플을 비교했다.

| 프레임 | binding | L0 | 차이 | 원시 프로파일 |
|---|---:|---:|---:|---|
| `(idle)` | 77.51% | 51.85% | 25.66%p | [binding](raw/profile/cpu/CPU.20261113.192849.1888388.0.001.cpuprofile), [L0](raw/profile/cpu/CPU.20261113.193048.1903686.0.001.cpuprofile) |
| `(program)` | 6.17% | 13.52% | 7.36%p | 위 프로파일 |
| `recv` (`socket_operations.js:156`) | 8.29% | 14.40% | 6.11%p | 위 프로파일 |
| `processImmediate` (`node:internal/timers:436`) | 0.77% | 2.73% | 1.96%p | 위 프로파일 |
| `routedReceivedPrefersManagedBuffer` (`message_materializer.js:26`) | 1.31% | 2.87% | 1.56%p | 위 프로파일 |
| `replyDirect` (`router_socket.js:56`) | 0.94% | 2.22% | 1.28%p | 위 프로파일 |
| `(garbage collector)` | 0.62% | 1.80% | 1.18%p | 위 프로파일 |

첫 프로파일 시도는 raw target의 CPU profile이 생성되지 않아 비교에서 제외했고, SIGTERM profile flush hook을 넣어 아래 완전한 쌍을 다시 수집했다.

프로파일링 실행의 셀 처리량은 binding 7,696.8 msg/s, L0 7,552.6 msg/s였다. 각각 [binding 결과](raw/profile-complete/bench/node/run1/zlink-node-request-serial-4096/results.json), [L0 결과](raw/profile-complete/bench/node/run1/zlink-framework-node-request-serial-4096/results.json). 프로파일은 각각 112,337개와 46,776개의 샘플을 기록했다([binding profile](raw/profile/cpu/CPU.20261113.192849.1888388.0.001.cpuprofile), [L0 profile](raw/profile/cpu/CPU.20261113.193048.1903686.0.001.cpuprofile)).

## 변형 5회 결과

비율은 같은 잠금 세션에서 측정한 binding 중앙값 대비다. spread = `max(|실행−중앙값|) / 중앙값`이다. 처리량, 비율, spread는 각 행의 다섯 원시 결과를 집계했다.

| 변형 | 중앙값 msg/s | binding 대비 | spread | 원시 결과 |
|---|---:|---:|---:|---|
| binding | 8,144.8 | 1.000 | 16.3% | [1](raw/variants/binding/run1/zlink-node-request-serial-4096/results.json), [2](raw/variants/binding/run2/zlink-node-request-serial-4096/results.json), [3](raw/variants/binding/run3/zlink-node-request-serial-4096/results.json), [4](raw/variants/binding/run4/zlink-node-request-serial-4096/results.json), [5](raw/variants/binding/run5/zlink-node-request-serial-4096/results.json) |
| 현재 L0 | 7,468.0 | 0.917 | 49.7% | [1](raw/variants/current/run1/zlink-node-request-serial-4096/results.json), [2](raw/variants/current/run2/zlink-node-request-serial-4096/results.json), [3](raw/variants/current/run3/zlink-node-request-serial-4096/results.json), [4](raw/variants/current/run4/zlink-node-request-serial-4096/results.json), [5](raw/variants/current/run5/zlink-node-request-serial-4096/results.json) |
| L0+a 두 라우터 | 7,829.0 | 0.961 | 37.6% | [1](raw/variants/l0-a/run1/zlink-node-request-serial-4096/results.json), [2](raw/variants/l0-a/run2/zlink-node-request-serial-4096/results.json), [3](raw/variants/l0-a/run3/zlink-node-request-serial-4096/results.json), [4](raw/variants/l0-a/run4/zlink-node-request-serial-4096/results.json), [5](raw/variants/l0-a/run5/zlink-node-request-serial-4096/results.json) |
| L0+b L0 경로 지연 로드 | 8,474.6 | 1.040 | 3.5% | [1](raw/variants/l0-b/run1/zlink-node-request-serial-4096/results.json), [2](raw/variants/l0-b/run2/zlink-node-request-serial-4096/results.json), [3](raw/variants/l0-b/run3/zlink-node-request-serial-4096/results.json), [4](raw/variants/l0-b/run4/zlink-node-request-serial-4096/results.json), [5](raw/variants/l0-b/run5/zlink-node-request-serial-4096/results.json) |
| L0+a+b | 7,983.2 | 0.980 | 5.3% | [1](raw/variants/l0-ab/run1/zlink-node-request-serial-4096/results.json), [2](raw/variants/l0-ab/run2/zlink-node-request-serial-4096/results.json), [3](raw/variants/l0-ab/run3/zlink-node-request-serial-4096/results.json), [4](raw/variants/l0-ab/run4/zlink-node-request-serial-4096/results.json), [5](raw/variants/l0-ab/run5/zlink-node-request-serial-4096/results.json) |

각 변형의 요청 수락/응답 경로는 그대로 두고 라우터 분리와 Framework/Nest/codec require 위치만 바꿨다. 모든 실행에서 errors=0이고 submitted=completed=target received였다(위 각 원시 결과).

## 선택한 L0와 전체 셀 재측정

`L0+a+b`를 기본 L0로 선택했다. 라우팅 구조를 binding과 맞추고 L0 실행 경로에서 Framework/Nest/codec을 require하지 않는다. 선택 commit: `dba747fe85` (`perf(node bench): align L0 server shape`).

여섯 셀(binding 대 선택 L0) 재측정은 공유 잠금 대기열에서 시작하지 못했다. 중단 시 선행 C++ 작업이 4K send-saturation의 네 번째 반복을 실행 중이었고, 그 뒤에 Java 4단계 전체 행렬이 먼저 대기 중이었다([잠금 상태](raw/final-lock-queue-at-stop.txt)). 완료 시간이 4시간 한도를 넘을 것으로 판단해 내 대기 프로세스만 종료했다. 최종 6셀 표는 미측정이다. 잠금 순서를 바꾸지 않고 재개하려면 이 행렬을 새 잠금 세션에서 실행해야 한다.

선택 commit은 `build.sh`, `node --check`, `bash -n`, `git diff --check`를 통과했다.
