# Java ladder 측정 — 2026-11-13 조건 기준

조건: C2·C3 off, SCHED_OTHER, source CPU 0–5 / target CPU 8–13, housekeeping 6–7·14–15, guard v8. warmup 10초, active 5초. Binding은 셀 변경마다 warm 1회 + counted 1회로 측정했다. build/JAR은 각 counted run 전에 소스보다 최신인지 확인했고 재빌드하지 않았다. [run 진행·커밋·JAR/실행기 SHA-256](progress.md) · [호출 형태 비교](call-shape.md)

## 표 A — 처리량과 Framework/binding

| 셀 | Binding msg/s | L0 | L1 | L2 | L3 | L4 | L5~L7 | Framework(L5~L7)/binding |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| send-saturation 1K | [525,897.6](raw/java/send-saturation-1024/counted/attempt1/run1/zlink-java-send-saturation-1024/results.json) · [환경](raw/java/send-saturation-1024/counted/attempt1/run1/zlink-java-send-saturation-1024/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| send-saturation 4K | [205,254.6](raw/java/send-saturation-4096/counted/attempt1/run1/zlink-java-send-saturation-4096/results.json) · [환경](raw/java/send-saturation-4096/counted/attempt1/run1/zlink-java-send-saturation-4096/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-serial 1K | [12,963.8](raw/java/request-serial-1024/counted/attempt1/run1/zlink-java-request-serial-1024/results.json) · [환경](raw/java/request-serial-1024/counted/attempt1/run1/zlink-java-request-serial-1024/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-serial 4K | [11,971.8](raw/java/request-serial-4096/counted/attempt1/run1/zlink-java-request-serial-4096/results.json) · [환경](raw/java/request-serial-4096/counted/attempt1/run1/zlink-java-request-serial-4096/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-backpressure 1K | [130,125.4](raw/java/request-backpressure-1024/counted/attempt1/run1/zlink-java-request-backpressure-1024/results.json) · [환경](raw/java/request-backpressure-1024/counted/attempt1/run1/zlink-java-request-backpressure-1024/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-backpressure 4K | [111,235.4](raw/java/request-backpressure-4096/counted/attempt1/run1/zlink-java-request-backpressure-4096/results.json) · [환경](raw/java/request-backpressure-4096/counted/attempt1/run1/zlink-java-request-backpressure-4096/scheduling.txt) | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |

Binding counted 6회 모두 오류 0, submitted=completed=received이며 시작·중간 source/target thread가 전부 SCHED_OTHER, affinity가 각각 0–5 / 8–13이었다. 각 셀 warm 결과도 `raw/java/<cell>/warm/attempt1/run1/`에 보존했다. [binding JSON](../binding-daily/java-2026-11-13b.json)

## 표 B — 단계별 손실 및 기준 판정

| 셀 | L0/binding (≥0.95) | L1/L0 (손실 ≤7%) | L2/L1 (≤7%) | L3/L2 (≤7%) | L4/L3 (≤7%) | L5~L7/L4 (손실 ≤21%) |
|---|---|---|---|---|---|---|
| send-saturation 1K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| send-saturation 4K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-serial 1K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-serial 4K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-backpressure 1K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |
| request-backpressure 4K | 보류 | 보류 | 보류 | 보류 | 보류 | 보류 |

**Ladder gate:** [aa-noise5 결과](../aa-noise5/report.md)의 send-saturation 4K에서 A G5=5.39%로 5%를 넘었고 B도 4회뿐이라 이 셀의 5회 A/A가 미완료다. 측정 규칙에 따라 ladder 측정을 시작하지 않았다. 원자료의 주파수·부하·프로세스·IRQ 기록으로는 변동의 단일 원인을 특정하지 못했다. request-serial 4K G5는 A/B 3.59%/3.39%였지만 send gate를 대체하지 않는다.

원시 counted 결과와 해당 run의 시작·중간 affinity 및 모든 thread scheduling 기록은 표 A 각 셀 링크에 있다. 이 worktree에는 코드 변경이 없다.
