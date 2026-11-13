# #4190 재측정 및 #4162 A/A

측정 commit: `d3fd955131c1942dc46203fde365f1c690b3d0f6` (#4245 merge).
기본 runner 값은 warmup 2초, active 5초, judged run 3회이며 요청 패턴은 직렬 요청·역압 요청·send 포화, payload는 1 KiB·4 KiB다. Framework 언어 드라이버는 8 submit stream 설정을 사용했다. C raw 기준은 C runner를 그대로 사용했다. C raw는 18셀, 언어별 측정은 gRPC 역압 요청 n/a를 빼고 48셀, Kotlin 보충은 serial 1 KiB 6셀이다.

## 처리량 및 기준 판정

처리량은 run 중앙값(msg/s)이다. 비율 열은 `framework/binding`(기준 0.60)과 역압의 `binding/C`(기준 0.80)이다. 숫자는 기준과 직접 비교한 값이고 괄호는 #4230 aggregator 판정이다. aggregator `미판정`은 G5/G6 또는 분모 셀의 기준 실패로 7.2 judgement가 지원되지 않는 경우다.

### C++

언어 종합: **미완료** — 3 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) |
|---|---:|---:|---:|---:|---:|---:|
| request-serial | 1024 | 7,704 | 13,743 | 8,822 | 0.642 (수치통과; agg 통과) | — |
| request-serial | 4096 | 7,348 | 12,873 | 7,992 | 0.621 (수치통과; agg 통과) | — |
| send-saturation | 1024 | 24,393 | 592,105 | 112,939 | 0.191 (수치미달; agg 미달) | — |
| send-saturation | 4096 | 23,987 | 255,994 | 110,435 | 0.431 (수치미달; agg 미판정) | — |

#### C++ CPU

source/target CPU µs/msg՝ `/proc/<pid>/task` TID union의 before/after tick 차이 ÷ 완료 메시지. 표의 값은 반복 중앙값. C CPU는 처리량 판정에 사용하지 않은 별도 1회 보충 실행에서 TID union으로 수집했다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target |
|---|---:|---:|---:|---:|---:|
| request-serial | 1024 | 49.6/94.6 | 38.7/35.9 | 59.9/72.8 | 58.4/45.8 (n=1/1, CPU 보충) |
| request-serial | 4096 | 51.6/96.4 | 39.5/38.2 | 68.1/74.5 | 61.8/49.1 (n=1/1, CPU 보충) |
| request-backpressure | 1024 | n/a | 12.5/11.7 | 66.3/50.1 | 51.5/13.5 (n=1/1, CPU 보충) |
| request-backpressure | 4096 | n/a | 20.2/18.3 | 70.3/58.7 | 48.4/23.3 (n=1/1, CPU 보충) |
| send-saturation | 1024 | 46.8/89.5 | 1.9/2.9 | 8.9/18.2 | 9.8/3.7 (n=1/1, CPU 보충) |
| send-saturation | 4096 | 47.8/91.2 | 4.5/6.3 | 9.1/18.9 | 14.4/8.8 (n=1/1, CPU 보충) |

### .NET

언어 종합: **미완료** — 2 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) |
|---|---:|---:|---:|---:|---:|---:|
| request-serial | 1024 | 7,279 | 10,208 | 6,347 | 0.622 (수치통과; agg 통과) | — |
| request-serial | 4096 | 6,318 | 9,273 | 6,018 | 0.649 (수치통과; agg 통과) | — |
| send-saturation | 1024 | 23,063 | 438,764 | 194,349 | 0.443 (수치미달; agg 미달) | — |
| send-saturation | 4096 | 21,361 | 177,643 | 132,452 | 0.746 (수치통과; agg 통과) | — |

#### .NET CPU

source/target CPU µs/msg՝ `/proc/<pid>/task` TID union의 before/after tick 차이 ÷ 완료 메시지. 표의 값은 반복 중앙값. C CPU는 처리량 판정에 사용하지 않은 별도 1회 보충 실행에서 TID union으로 수집했다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target |
|---|---:|---:|---:|---:|---:|
| request-serial | 1024 | 415.1/341.3 | 196.2/44.1 | 260.8/91.3 | 58.4/45.8 (n=1/1, CPU 보충) |
| request-serial | 4096 | 448.6/370.2 | 216.0/44.5 | 272.0/91.2 | 61.8/49.1 (n=1/1, CPU 보충) |
| request-backpressure | 1024 | n/a | 28.6/11.5 | 81.2/46.3 | 51.5/13.5 (n=1/1, CPU 보충) |
| request-backpressure | 4096 | n/a | 34.3/19.2 | 99.9/54.4 | 48.4/23.3 (n=1/1, CPU 보충) |
| send-saturation | 1024 | 240.0/244.0 | 3.9/4.0 | 5.7/13.9 | 9.8/3.7 (n=1/1, CPU 보충) |
| send-saturation | 4096 | 252.3/265.3 | 10.2/9.9 | 10.7/20.8 | 14.4/8.8 (n=1/1, CPU 보충) |

### Java

언어 종합: **미완료** — 5 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) |
|---|---:|---:|---:|---:|---:|---:|
| request-serial | 1024 | 8,341 | 13,737 | 5,641 | 0.411 (수치미달; agg 미판정) | — |
| request-serial | 4096 | 7,958 | 13,151 | 4,535 | 0.345 (수치미달; agg 미판정) | — |
| send-saturation | 1024 | 32,915 | 553,834 | 106,351 | 0.192 (수치미달; agg 미달) | — |
| send-saturation | 4096 | 31,933 | 203,852 | 102,629 | 0.503 (수치미달; agg 미달) | — |

#### Java CPU

source/target CPU µs/msg՝ `/proc/<pid>/task` TID union의 before/after tick 차이 ÷ 완료 메시지. 표의 값은 반복 중앙값. C CPU는 처리량 판정에 사용하지 않은 별도 1회 보충 실행에서 TID union으로 수집했다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target |
|---|---:|---:|---:|---:|---:|
| request-serial | 1024 | 101.7/110.2 | 31.7/43.0 | 201.4/196.2 | 58.4/45.8 (n=1/1, CPU 보충) |
| request-serial | 4096 | 111.7/114.0 | 35.1/44.9 | 269.9/236.8 | 61.8/49.1 (n=1/1, CPU 보충) |
| request-backpressure | 1024 | n/a | 10.2/15.5 | 75.8/81.4 | 51.5/13.5 (n=1/1, CPU 보충) |
| request-backpressure | 4096 | n/a | 17.5/24.5 | 81.0/93.9 | 48.4/23.3 (n=1/1, CPU 보충) |
| send-saturation | 1024 | 59.7/61.9 | 2.3/3.4 | 10.2/24.9 | 9.8/3.7 (n=1/1, CPU 보충) |
| send-saturation | 4096 | 59.5/62.6 | 7.2/9.3 | 10.8/25.4 | 14.4/8.8 (n=1/1, CPU 보충) |

### Node

언어 종합: **미완료** — 8 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) |
|---|---:|---:|---:|---:|---:|---:|
| request-serial | 1024 | 3,196 | 9,081 | 5,425 | 0.597 (수치미달; agg 미판정) | — |
| request-serial | 4096 | 3,041 | 8,812 | 5,080 | 0.576 (수치미달; agg 미판정) | — |
| send-saturation | 1024 | 5,774 | 117,787 | 76,566 | 0.650 (수치통과; agg 미판정) | — |
| send-saturation | 4096 | 5,461 | 87,574 | 54,986 | 0.628 (수치통과; agg 미판정) | — |

#### Node CPU

source/target CPU µs/msg՝ `/proc/<pid>/task` TID union의 before/after tick 차이 ÷ 완료 메시지. 표의 값은 반복 중앙값. C CPU는 처리량 판정에 사용하지 않은 별도 1회 보충 실행에서 TID union으로 수집했다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target |
|---|---:|---:|---:|---:|---:|
| request-serial | 1024 | 317.9/157.2 | 81.5/140.5 | 161.8/116.8 | 58.4/45.8 (n=1/1, CPU 보충) |
| request-serial | 4096 | 342.8/166.9 | 83.3/147.7 | 163.8/120.5 | 61.8/49.1 (n=1/1, CPU 보충) |
| request-backpressure | 1024 | n/a | 59.7/63.2 | 116.1/52.9 | 51.5/13.5 (n=1/1, CPU 보충) |
| request-backpressure | 4096 | n/a | 73.7/70.9 | 89.1/68.4 | 48.4/23.3 (n=1/1, CPU 보충) |
| send-saturation | 1024 | 190.5/72.8 | 14.7/10.8 | 19.6/15.7 | 9.8/3.7 (n=1/1, CPU 보충) |
| send-saturation | 4096 | 204.6/79.5 | 23.7/20.5 | 30.2/25.4 | 14.4/8.8 (n=1/1, CPU 보충) |

## Kotlin 보충 셀

요청 직렬 @1 KiB, 3회; gRPC Kotlin 및 Framework Kotlin. 처리량은 중앙값이다.

| 구현 | msg/s | source/target CPU µs/msg |
|---|---:|---:|
| gRPC Kotlin | 6,725 | 155.8/120.1 |
| Framework Kotlin | 4,358 | 286.3/252.4 |

## A/A (#4162)

`aa-clean`, separate, 20회 실행. analyzer가 p50을 제공하지 않아 요청한 p50 ±5% 기준은 **실패(판정 자료 없음)**이며, p50 범위는 N/A다. 참고로 제공된 평균 지연 중앙값은 0.006109 ms, 범위는 0.003244–0.047212 ms이고 평균 기준 ±5%도 실패다.

## 측정 조건 및 기록

- governor: 각 측정 lock window에서 cpu0–15 `performance`; lock 해제 시 trap으로 `powersave` 복원.
- placement B: source 0–7, target 8–15. Kotlin runner의 별도 process도 source 0–7, target 8–15에 배치.
- 잠금 창(UTC): cpp 08:37:59–08:50:23; .NET 09:12:07–09:32:51; Java+Kotlin 09:34:02–09:55:25; Node 09:56:28–10:05:09; C+A/A 10:25:35–10:32:26. C와 A/A는 같은 exclusive lock window였다. C CPU 보충 10:57:13–10:58:54.
- uptime/load, free memory, cpu0–15 `scaling_cur_freq`는 각 셀의 `conditions-before.txt`/`conditions-after.txt`에 저장했다. 전체 raw 로그는 `raw/`에 보존했다.
- A/A 조건 전/후는 `raw/aa-clean-conditions-before.txt`와 `raw/aa-clean-conditions-after.txt`; run별 raw는 f4162 경로 아래 `aa-clean` label로 보존되어 있다.
- CPU 스냅샷 완전성: 기본 C 18셀의 첫 `/proc` watcher는 7셀에서만 양쪽 양수 TID delta를 얻었고 conditions-before 16/18을 작성했다. 별도 C CPU 보충 실행은 6개 pattern/size 행의 CPU와 conditions-before/after를 모두 수집했고, CPU 표는 그 보충 실행을 사용한다. 다른 유효한 주요 데이터셋의 CPU rows는 C++ 48, .NET 48, Java 48, Kotlin 6, Node 48이며 null CPU 값은 없다.
- 첫 C 실행은 gRPC 보조 실행 파일이 없어 runner preflight에서 종료됐다. 그 보조 타깃은 C++ build slot에서 빌드한 후 같은 코드로 C raw를 실행했다. 첫 .NET raw는 launcher CPU를 잡아 CPU 집계에서 제외하고 유효 재측정 48셀을 사용했다. 첫 C++ 시도도 불완전 raw로 보존했으며 judgement에는 재측정 48셀만 넣었다.

## 검증 및 결과 위치

- 실행: C/C++ `cpp-build-slot.sh`로 빌드; Java/Node/.NET/C/Kotlin의 각 `run_local*.sh`; `bench_aggregate.py --lang {cpp,dotnet,java,node} --min-runs 3`; `aggregate_cpu.py`와 C 전용 TID-union 집계 및 `proc_capture_c.py` 보충 watcher; `observe.py --runs 20 --label aa-clean --placement separate`; `analyze-aa.py`.
- aggregator markdown/JSON: `raw/aggregates/{cpp,dotnet,java,node}.{md,json}`. Runner raw와 조건·CPU 스냅샷: `raw/bench/`. Driver/build logs: `raw/*-driver.log`, `build-logs/`.
- language verdict: C++ 미완료; .NET 미완료; Java 미완료; Node 미완료.
- CPU 측정은 C raw에서 불완전하며, A/A p50은 analyzer 출력에 없다. 이 외 source 코드·bench 코드 변경은 없다.

