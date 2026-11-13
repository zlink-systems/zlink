# Base Spot 단계 시간과 22k app-worker 차이 (#1639)

## Spot stage timing

30초 warm-up/30초 측정, PM=1, 1 KiB send-saturation, 같은 perf probe와 analyzer를 사용했다. 단위는 µs.

| 구간 | base p50 / p99 | chained p50 / p99 |
|---|---:|---:|
| HTTP 진입 → gate 제출 전 (`before_schedule`) | 255 / 436 | 1,158 / 6,197 |
| gate 제출 → 실행 (`schedule_to_run`) | 20 / 38 | 32 / 1,942 |
| 실행 → Spot (`run_to_spot`) | 29 / 46 | 40 / 60 |
| Spot → HTTP 종료 (`spot_to_http`) | 79 / 125 | 134 / 3,761 |

base의 HTTP 진입부터 gate 제출 전까지는 chained처럼 크지 않았다. base raw: `/root/jobs/fx/cx/v21/s033/bstage/stage/base-valid/run1/zlink-framework-cpp-send-saturation-1024/capture/stage-summary.json`; chained raw: `/root/jobs/fx/cx/v21/s033/psched8/stage/chained/run1/zlink-framework-cpp-send-saturation-1024/capture/stage-summary.json`.

## Base 22k A/B/A/B

모두 base build, request-backpressure 1 KiB, warm-up 30초, active 20초, `TARGET_RATE=48000`(관측 32.0–32.4k msg/s), source CPU 0–7, target CPU 8–15. 시간은 µs/msg. `역할 합`은 app-worker + receive, `target 전체`는 target 프로세스에서 양 snapshot에 잡힌 모든 스레드의 합이다. wakes는 target/source, sendto와 GHz는 각 프로세스에서 sendto가 가장 많은 IO 스레드 값이다.

| 실행 | 관측 msg/s | app-worker | receive | 역할 합 | target 전체 | wakes T/S | sendto T/S | GHz T/S |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A1 psmeas 셸 | 32,351 | 21.87 | 15.04 | 36.91 | 66.53 | 1.336 / 2.913 | 0.802 / 0.718 | 2.883 / 2.897 |
| B1 psched8 supervisor | 32,000 | 23.00 | 15.71 | 38.71 | 69.59 | 1.370 / 2.909 | 0.830 / 0.738 | 2.861 / 2.876 |
| A2 psmeas 셸 | 32,277 | 21.68 | 15.21 | 36.89 | 66.42 | 1.322 / 2.977 | 0.792 / 0.717 | 2.879 / 2.891 |
| B2 psched8 supervisor | 32,268 | 11.83 | 15.10 | 26.93 | 57.13 | 1.349 / 2.979 | 0.810 / 0.722 | 2.875 / 2.885 |

raw captures: `/root/jobs/fx/cx/v21/s033/bstage/measure/{A1,B1,A2,B2}/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture/`. 각 `windows.json`, `A-{target,source}-{before,after}.tsv`, `A-perfstat.csv`, `target.log`를 집계했다.

이전 세션도 raw에서 재확인했다. psmeas의 app-worker는 22.08–22.54, 역할 합은 37.45–37.92 µs/msg였고, psched8은 각각 11.51–11.80, 27.19–27.45였다. **측정값의 2배 차이는 analyzer의 TID 교집합 집계가 원인**이다: [psmeas/analyze-22k.py](/root/jobs/fx/cx/v21/s033/psmeas/analyze-22k.py:30)와 [psched8/analyze-22k.py](/root/jobs/fx/cx/v21/s033/psched8/analyze-22k.py:30)는 before와 after 양쪽에 있는 TID만 `threads`에 넣고(32행), ROLE 로그의 TID 중 그 집합에 남은 것만 합산한다(47–50행). 세 app-worker TID 가운데 이전 psmeas raw는 두 개가 양쪽 snapshot에 있어 약 11 µs씩 합쳐졌고, psched8 raw는 한 개만 있어 약 11.5 µs만 잡혔다. 새 실행도 A1/B1/A2는 두 개, B2는 한 개만 양쪽에 남았다. 따라서 **셸/Python launch 차이만으로는 설명되지 않으며**, B1이 A 수준으로 나와 launch 효과는 두 번의 교대 측정에서 재현되지 않았다.

runner 대조: workload·build·trace off·pinning·역할 로깅·측정 시간은 동일하다. psmeas는 `run-B.sh`에서 `setsid taskset`으로 source/target을 직접 띄우고, psched8은 `processes.py` supervisor가 띄운다([psmeas/run-B.sh](/root/jobs/fx/cx/v21/s033/psmeas/run-B.sh:64), [psched8/run-B.sh](/root/jobs/fx/cx/v21/s033/psched8/run-B.sh:64), [psched8/processes.py](/root/jobs/fx/cx/v21/s033/psched8/processes.py:5)). supervisor는 `BENCH_PROCESS_SOCKET`을 자식 환경에 추가한다. 양쪽 collector는 전달받은 source/client와 target/Framework server leader PID의 `/proc/<pid>/task` 스레드를 집계한다. collector 차이는 psched8이 TID 조회 중 `ProcessLookupError`도 건너뛴다는 점이다([psmeas/collect-22k.py](/root/jobs/fx/cx/v21/s033/psmeas/collect-22k.py:19), [psched8/collect-22k.py](/root/jobs/fx/cx/v21/s033/psched8/collect-22k.py:19)). `runner-common.sh`도 다르다: 기본 collector 경로가 서로 다르지만 이번 실행은 둘 다 `BENCH_COLLECTOR`를 명시해 차이가 적용되지 않았다. 정리 신호는 psmeas가 마지막에 `SIGKILL`, psched8이 `SIGTERM`이며, active capture 후 종료 단계다.

**가설:** 역할 TID가 측정 구간 양끝에 모두 살아 있지 않은 변동은 5초 window와 worker-thread 수명/재사용 타이밍에서 생겼을 수 있다. 현재 자료는 어떤 TID가 사라지거나 늦게 생긴 근본 원인까지 특정하지 않는다. 그래서 app-worker 2× 값은 안정적인 session 차이로 취급하면 안 된다.

첫 stage 시도 두 번과 첫 B 호출은 복사 harness의 collector/supervisor 연결 오류로 원하는 결과를 만들지 못했으며 raw와 로그를 보존했다. 유효 stage 측정은 한 번 완료했다. 유효한 22k 실행은 A/B/A/B 순서다. 작업 종료 시 `bstage:*` perf probe 제거를 확인했다.

## 감독자 지시 (pool 작업이 이 보고서를 읽을 때 적용)

- Launch 방식은 원인이 아니다. psched8 supervisor 방식을 그대로 쓴다.
- 22k thread 집계는 before/after 교집합이 아니라 합집합으로 한다. after에만 있는 TID(측정 구간 중 생긴 thread)는 after 값을 그대로 더한다. before에만 있는 TID 수는 따로 보고한다. 역할 합·process 전체·zl-cmpl 합·깨우기 모두 이 규칙으로 계산하고, 교집합 값은 쓰지 않는다.
- 감독자 확인: psched8 base target에서 after에만 있는 thread 하나가 10.7–11.2 µs/msg였다. 합집합으로 다시 세면 base target process 전체는 67.1–69.4, chained는 92.2–92.5 µs/msg다.
