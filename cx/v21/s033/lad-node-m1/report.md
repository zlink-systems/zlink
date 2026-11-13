# Node L0R + L1 빠른 측정

기준 HEAD: `c512887606aeb686eccc53162d9f02c3c4db63ed` (L0 경로 정합성, L1 permit, L0R module-load switch 포함). 코드 변경 없음: 실험 코드는 이미 로컬 커밋 상태다.

호출 형태 대조는 [call-shape.md](call-shape.md)에 기록했다. Binding과 L0/L0R은 두 ROUTER의 DontWait batch 및 setImmediate pump를 공유한다. L0는 고정 handler 직접 호출, L0R은 top-level Framework/queue 모듈 load, L1은 receive별 permit 추가다. Reply는 모두 새 protobuf body와 operation을 만들어 두 part를 submit한다.

## 기본 hold 결과

| 시나리오 | 크기 | binding msg/s | L0 msg/s | L0R msg/s | L1 msg/s | L0R 손실 | L1/L0R 손실 | L1/L0 손실 | 판정 | raw 결과 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation | 1024 | 110892.6 | 109492.8 | 97175.8 | 103895.4 | 11.25% | -6.91% | 5.11% | L0/binding 통과; L1 step 통과 | binding: `raw/session/binding/run1/zlink-node-send-saturation-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-send-saturation-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-send-saturation-1024/results.json` |
| send-saturation | 4096 | 87063.2 | 78579.6 | 74106.6 | 65072.8 | 5.69% | 12.19% | 17.19% | L0/binding 미달; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-send-saturation-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-send-saturation-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-send-saturation-4096/results.json` |
| request-serial | 1024 | 7202.0 | 7968.2 | 8056.2 | 4481.0 | -1.10% | 44.38% | 43.76% | L0/binding 통과; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-request-serial-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-serial-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-serial-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-serial-1024/results.json` |
| request-serial | 4096 | 6995.2 | 7861.4 | 7087.6 | 6782.0 | 9.84% | 4.31% | 13.73% | L0/binding 통과; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-request-serial-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-serial-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-serial-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-serial-4096/results.json` |
| request-backpressure | 1024 | 30957.2 | 32941.8 | 31597.0 | 30292.2 | 4.08% | 4.13% | 8.04% | L0/binding 통과; L1 경계 3회 확인 통과 (3.73%) | binding: `raw/session/binding/run1/zlink-node-request-backpressure-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-backpressure-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-backpressure-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-backpressure-1024/results.json` |
| request-backpressure | 4096 | 22793.2 | 22859.8 | 22317.2 | 22088.4 | 2.37% | 1.03% | 3.37% | L0/binding 통과; L1 step 통과 | binding: `raw/session/binding/run1/zlink-node-request-backpressure-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-backpressure-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-backpressure-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-backpressure-4096/results.json` |

각 1회 측정의 G5는 적용하지 않는다. 셀 오류 0, submitted=completed=received 검사를 통과했다. L1 단계 판정은 L1/L0 손실 7% 이하 기준이다.

기본 hold의 경계 5–9% 세 비교는 후속 hold에서 각 3회 확인했다(기본 1회 + 추가 2회).

## 경계 셀 3회 확인

기본 hold의 1회 값은 위 ladder 비교표에 그대로 둔다. 아래 비교만 기본 표본과 추가 2회를 합친 3회 중앙값으로 판정한다. G5는 최대 편차/중앙값이며 10% 초과는 판정 불가다.

| 셀 | 비교 | 이전 중앙값 | 새 중앙값 | 손실 | G5 이전/새 | 판정 | raw 결과 (각 3회) |
|---|---|---:|---:|---:|---|---|---|
| send-saturation-1024 | L0→L1 | 110389.6 | 103895.4 | 5.88% | 통과/통과 | 통과 | L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-1024/results.json`, L0: `raw/session/confirm/L0/run1/zlink-framework-node-send-saturation-1024/results.json`, L0: `raw/session/confirm/L0/run2/zlink-framework-node-send-saturation-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-send-saturation-1024/results.json`, L1: `raw/session/confirm/L1/run1/zlink-framework-node-send-saturation-1024/results.json`, L1: `raw/session/confirm/L1/run2/zlink-framework-node-send-saturation-1024/results.json` |
| send-saturation-4096 | L0→L0R | 74690.8 | 76038.2 | -1.80% | 통과/판정 불가 | 판정 불가 (L0R G5) | L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-4096/results.json`, L0: `raw/session/confirm/L0/run1/zlink-framework-node-send-saturation-4096/results.json`, L0: `raw/session/confirm/L0/run2/zlink-framework-node-send-saturation-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-send-saturation-4096/results.json`, L0R: `raw/session/confirm/L0R/run1/zlink-framework-node-send-saturation-4096/results.json`, L0R: `raw/session/confirm/L0R/run2/zlink-framework-node-send-saturation-4096/results.json` |
| request-backpressure-1024 | L0→L1 | 31601.0 | 30422.0 | 3.73% | 통과/통과 | 통과 | L0: `raw/session/L0/run1/zlink-framework-node-request-backpressure-1024/results.json`, L0: `raw/session/confirm/L0/run1/zlink-framework-node-request-backpressure-1024/results.json`, L0: `raw/session/confirm/L0/run2/zlink-framework-node-request-backpressure-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-backpressure-1024/results.json`, L1: `raw/session/confirm/L1/run1/zlink-framework-node-request-backpressure-1024/results.json`, L1: `raw/session/confirm/L1/run2/zlink-framework-node-request-backpressure-1024/results.json` |

## Request-serial 4K L0/L0R profiling

요청 시 조건(L0R 대 L0 module-load loss 9.84% > 2%)에 따라 한 쌍을 수집했다. Target 종료 시 메모리: L0 RSS 78.7 MiB, heapTotal 15.1 MiB, heapUsed 11.6 MiB; L0R RSS 94.4 MiB, heapTotal 29.6 MiB, heapUsed 12.1 MiB. 원시 메모리 기록은 `raw/profile-bench/module-L0/run1/zlink-framework-node-request-serial-4096/target.log` 및 `raw/profile-bench/module-L0R/run1/zlink-framework-node-request-serial-4096/target.log`다.

Target CPU sample 비율은 L0→L0R에서 `recv` `framework/languages/node/node_modules/@zlink-systems/zlink/dist/zlink/runtime/sockets/socket_operations.js:156` 약 7.0%→19.1%, L0 pump `framework/bench/grpc/node/zlink-framework-server/main.js:93` 약 0.5%→1.5%, GC 0.6%→1.7%, `(program)` 5.9%→14.6%였다. profile raw는 `raw/profile/module-load/L0/CPU.20261114.005724.1092573.0.001.cpuprofile` 및 `raw/profile/module-load/L0R/CPU.20261114.005754.1099754.0.001.cpuprofile`이다.

## 최종 판정

- L0의 binding 대비 0.95 이상 기준은 send-saturation 4K에서 미달이다 (L0 78,579.6 / binding 87,063.2 msg/s, 원시 파일: `raw/session/L0/run1/zlink-framework-node-send-saturation-4096/results.json`, `raw/session/binding/run1/zlink-node-send-saturation-4096/results.json`).
- L1 단계 기준은 세 셀 미달, 세 셀 통과다. 미달은 send-saturation 4K (17.19%), request-serial 1K (43.76%), request-serial 4K (13.73%)다. send-saturation 1K와 request-backpressure 1K 경계 pair는 3회 확인 후 통과했고 request-backpressure 4K는 기본 hold에서 통과했다. 위 기본 hold raw 표가 각 수치를 인용한다.
- L0R module-load 손실이 request-serial 4K에서 9.84%로 관측되어 profiling을 수행했다. Target RSS는 78.7→94.4 MiB, GC 표본 share는 0.6→1.7%였으며 상세 raw 위치는 profiling 절에 적었다.
- 검증: `framework/bench/grpc/node/build.sh` 성공, framework queue `dist` 산출물이 해당 `src`보다 새롭고 native addon은 이 worktree의 `node_modules`에서 확인했다. 측정 셀은 모두 errors=0 및 submitted=completed=received 검사를 통과했다. 추가 테스트는 실행하지 않았다.
- 구현 변경은 HEAD `c512887606aeb686eccc53162d9f02c3c4db63ed`에 이미 커밋되어 있어 이번 작업에서 새 코드 커밋은 없었다.
