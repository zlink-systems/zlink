# Node L0R + L1 빠른 측정

기준 HEAD: `c512887606aeb686eccc53162d9f02c3c4db63ed` (L0 경로 정합성, L1 permit, L0R module-load switch 포함). 코드 변경 없음: 실험 코드는 이미 로컬 커밋 상태다.

호출 형태 대조는 [call-shape.md](call-shape.md)에 기록했다. 측정 전에 커밋과 산출물 해시, binding/L0/L0R/L1 receive·reply 코드 위치를 `progress.md`에 기록한다.

측정 결과는 lock hold 종료 직후 여기에 추가한다.

## 진행 중 첫 수치

| 시나리오 | 크기 | binding msg/s | 오류·완료 일치 | 원시 파일 |
|---|---:|---:|---|---|
| send-saturation | 1024 | 110892.6 | errors=0, submitted=completed=received=561108 | `raw/session/binding/run1/zlink-node-send-saturation-1024/results.json` |

## 기본 hold 결과

| 시나리오 | 크기 | binding msg/s | L0 msg/s | L0R msg/s | L1 msg/s | L0R 손실 | L1/L0R 손실 | L1/L0 손실 | 판정 | raw 결과 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation | 1024 | 110892.6 | 109492.8 | 97175.8 | 103895.4 | 11.25% | -6.91% | 5.11% | L0/binding 통과; L1 step 통과 | binding: `raw/session/binding/run1/zlink-node-send-saturation-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-send-saturation-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-send-saturation-1024/results.json` |
| send-saturation | 4096 | 87063.2 | 78579.6 | 74106.6 | 65072.8 | 5.69% | 12.19% | 17.19% | L0/binding 미달; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-send-saturation-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-send-saturation-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-send-saturation-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-send-saturation-4096/results.json` |
| request-serial | 1024 | 7202.0 | 7968.2 | 8056.2 | 4481.0 | -1.10% | 44.38% | 43.76% | L0/binding 통과; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-request-serial-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-serial-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-serial-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-serial-1024/results.json` |
| request-serial | 4096 | 6995.2 | 7861.4 | 7087.6 | 6782.0 | 9.84% | 4.31% | 13.73% | L0/binding 통과; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-request-serial-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-serial-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-serial-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-serial-4096/results.json` |
| request-backpressure | 1024 | 30957.2 | 32941.8 | 31597.0 | 30292.2 | 4.08% | 4.13% | 8.04% | L0/binding 통과; L1 step 미달 | binding: `raw/session/binding/run1/zlink-node-request-backpressure-1024/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-backpressure-1024/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-backpressure-1024/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-backpressure-1024/results.json` |
| request-backpressure | 4096 | 22793.2 | 22859.8 | 22317.2 | 22088.4 | 2.37% | 1.03% | 3.37% | L0/binding 통과; L1 step 통과 | binding: `raw/session/binding/run1/zlink-node-request-backpressure-4096/results.json`, L0: `raw/session/L0/run1/zlink-framework-node-request-backpressure-4096/results.json`, L0R: `raw/session/L0R/run1/zlink-framework-node-request-backpressure-4096/results.json`, L1: `raw/session/L1/run1/zlink-framework-node-request-backpressure-4096/results.json` |

각 1회 측정의 G5는 적용하지 않는다. 셀 오류 0, submitted=completed=received 검사를 통과했다. L1 단계 판정은 L1/L0 손실 7% 이하 기준이다.

경계 5–9% 셀은 짧은 후속 hold에서 해당 비교 pair를 각 3회 측정해야 한다. 현재 필요 셀: send-saturation-1024 L0-L1, send-saturation-4096 L0-L0R, request-backpressure-1024 L0-L1.
