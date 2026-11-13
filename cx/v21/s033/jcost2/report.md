원인 1 할당: 요청당 약 2,654바이트입니다.
원인 1 CPU: Spring 핸들러 경로 JFR 표본 추정은 요청당 2.06마이크로초입니다.
원인 1 wakeup: 대상 실행 풀 wakeup은 요청당 0.106961회입니다.
원인 2 할당: 요청당 약 115바이트입니다.
원인 2 CPU: 공통 ForkJoinPool 작업자 합계는 요청당 24.91마이크로초입니다.
원인 2 wakeup: 공통 ForkJoinPool 작업자는 요청당 0.483012회 깨어났습니다.

코드 변경 없음. 리팩토링 점검은 적용하지 않았습니다.

## 측정 기준

새 Framework 캡처는 `/root/jobs/fx/cx/ratio045diag/zlink-framework-java-request-backpressure-1024/`이고, Binding 캡처는 `/root/jobs/fx/cx/ratio045diag/zlink-java-request-backpressure-1024/`입니다. `window.json`의 완료 수 차이를 분모로 사용했습니다. Framework 소스 307,108건, 대상 308,896건, Binding 소스 517,733건, 대상 499,116건입니다. 3초 perf 구간에서 `sched_wakeup`은 wakee TID를 프로세스 스레드 목록과 대조해 세고, `sched_switch`는 prev TID가 해당 프로세스인 사건을 셌습니다. 스레드 이름은 perf와 `window.json`에 기록된 커널 comm 값이며, 15자 제한으로 잘린 이름은 기록된 그대로 표시했습니다. 풀 작업자는 같은 comm 이름끼리 합쳤습니다.

`swapper`는 깨우기 사건이 기록된 CPU의 idle task입니다. 해당 행은 perf가 기록한 waker comm을 나타내며, 커널 인터럽트나 그보다 앞선 사용자 스레드를 식별하지 않습니다. 따라서 `swapper`를 실제 애플리케이션 호출자로 해석하지 않습니다. `sched_wakeup`은 깨어난 OS 스레드 사건이며 완료 항목 제출 횟수와 같지 않습니다. 이미 실행 중인 작업자에게 제출된 작업은 깨우기로 나타나지 않을 수 있습니다.

원인별 할당 바이트는 기존 보고서의 JFR 가중 할당 표본 추정값을 유지했습니다. 원인 1은 Spring 핸들러 생성·초기화·정리 경로의 약 2,654바이트/요청이고, 원인 2는 `ForkJoinPool.commonPool().execute` 제출 경로의 약 115바이트/요청입니다.

## 요청당 wakeup 수

각 칸은 `사건 수 (완료 요청당 횟수)`입니다. 주요 작업자 이름을 표시했으며, `기타`는 아래 상세 목록의 나머지 JVM·GC·JFR·관리 스레드 이름을 합친 값입니다.

| Wakee comm | Framework 소스 | Framework 대상 | Binding 소스 | Binding 대상 |
|---|---:|---:|---:|---:|
| `ForkJoinPool.co` 공통 풀 작업자 | 148,337 (0.483012) | 0 (0) | 0 (0) | 0 (0) |
| `ForkJoinPool-1-` 작업자 | 120 (0.000391) | 33,036 (0.106949) | 0 (0) | 0 (0) |
| `VirtualThread-u` | 0 (0) | 1 (0.000003) | 0 (0) | 0 (0) |
| `HTTP-Dispatcher` | 8 (0.000026) | 4 (0.000013) | 7 (0.000014) | 4 (0.000008) |
| `ZLINKbg/IO/0` | 33,376 (0.108678) | 39,222 (0.126975) | 41,793 (0.080723) | 61,407 (0.123032) |
| `ZLINKbg/IO/1` | 38 (0.000124) | 32,245 (0.104388) | 34 (0.000066) | 48,105 (0.096380) |
| `ZLINKbg/IO/2` | 36,826 (0.119912) | 35 (0.000113) | 48,904 (0.094458) | 34 (0.000068) |
| `ZLINKbg/IO/3` | 41 (0.000134) | 35 (0.000113) | 34 (0.000066) | 34 (0.000068) |
| `zlink-timer` | 0 (0) | 108 (0.000350) | 0 (0) | 0 (0) |
| `zlink-jvm-raw-m` | 39,466 (0.128509) | 43,136 (0.139646) | 0 (0) | 0 (0) |
| `bench-phase-act` | 38,124 (0.124139) | 0 (0) | 41,384 (0.079933) | 0 (0) |
| `bench-raw-reque` | 0 (0) | 0 (0) | 0 (0) | 63,042 (0.126307) |
| 기타 | 1,781 (0.005799) | 1,631 (0.005281) | 896 (0.001731) | 860 (0.001723) |
| **전체** | **298,117 (0.970724)** | **149,453 (0.483830)** | **133,052 (0.256990)** | **173,486 (0.347587)** |

Framework 소스의 공통 ForkJoinPool 작업자 wakeup은 요청당 0.483012회입니다. 개별 작업자는 모두 잘린 `ForkJoinPool.co`로 기록되어 있어 일곱 작업자를 합산했습니다. Framework 대상에서 `ForkJoinPool-1-`, `HTTP-Dispatcher`, `VirtualThread-u` wakeup은 합계 33,041회, 요청당 0.106961회입니다. 가상 스레드 handler는 OS TID가 없어 별도 handler 스레드의 wakeup 횟수로 세지지 않습니다.

위 표의 `기타` 상세 comm별 사건 수와 요청당 비율은 다음과 같습니다.

| comm | Framework 소스 | Framework 대상 | Binding 소스 | Binding 대상 |
|---|---:|---:|---:|---:|
| `JFR Sampler Thr` | 967 (0.003149) | 992 (0.003211) | 590 (0.001140) | 557 (0.001116) |
| `VM Thread` | 146 (0.000475) | 37 (0.000120) | 25 (0.000048) | 17 (0.000034) |
| `G1 Refine#0` | 65 (0.000212) | 15 (0.000049) | 0 (0) | 53 (0.000106) |
| `GC Thread#0` | 55 (0.000179) | 52 (0.000168) | 25 (0.000048) | 17 (0.000034) |
| `GC Thread#1` | 62 (0.000202) | 52 (0.000168) | 33 (0.000064) | 18 (0.000036) |
| `GC Thread#2` | 57 (0.000186) | 54 (0.000175) | 27 (0.000052) | 23 (0.000046) |
| `GC Thread#3` | 58 (0.000189) | 48 (0.000155) | 21 (0.000041) | 16 (0.000032) |
| `GC Thread#4` | 55 (0.000179) | 65 (0.000210) | 23 (0.000044) | 18 (0.000036) |
| `GC Thread#5` | 56 (0.000182) | 56 (0.000181) | 26 (0.000050) | 20 (0.000040) |
| `GC Thread#6` | 57 (0.000186) | 60 (0.000194) | 24 (0.000046) | 20 (0.000040) |
| `GC Thread#7` | 56 (0.000182) | 52 (0.000168) | 28 (0.000054) | 20 (0.000040) |
| `ZLINKbg/Reaper` | 37 (0.000120) | 35 (0.000113) | 34 (0.000066) | 34 (0.000068) |
| `zlink-java-chan` | 36 (0.000117) | 35 (0.000113) | 0 (0) | 0 (0) |
| `Monitor Deflati` | 17 (0.000055) | 14 (0.000045) | 14 (0.000027) | 13 (0.000026) |
| `G1 Service` | 13 (0.000042) | 13 (0.000042) | 9 (0.000017) | 8 (0.000016) |
| `C2 CompilerThre` | 8 (0.000026) | 13 (0.000042) | 2 (0.000004) | 6 (0.000012) |
| `C1 CompilerThre` | 7 (0.000023) | 5 (0.000016) | 1 (0.000002) | 3 (0.000006) |
| `JFR Periodic Ta` | 7 (0.000023) | 9 (0.000029) | 6 (0.000012) | 6 (0.000012) |
| `Common-Cleaner` | 6 (0.000020) | 6 (0.000019) | 0 (0) | 0 (0) |
| `Reference Handl` | 5 (0.000016) | 5 (0.000016) | 0 (0) | 0 (0) |
| `JFR Recorder Th` | 3 (0.000010) | 7 (0.000023) | 3 (0.000006) | 3 (0.000006) |
| `Service Thread` | 3 (0.000010) | 3 (0.000010) | 4 (0.000008) | 4 (0.000008) |
| `idle-timeout-ta` | 2 (0.000007) | 2 (0.000006) | 0 (0) | 0 (0) |
| `HttpClient-1-Se` | 1 (0.000003) | 0 (0) | 1 (0.000002) | 0 (0) |
| `VM Periodic Tas` | 1 (0.000003) | 1 (0.000003) | 0 (0) | 1 (0.000002) |
| `bench-raw-comma` | 0 (0) | 0 (0) | 0 (0) | 3 (0.000006) |
| `zlink-req-time` | 0 (0) | 0 (0) | 0 (0) | 0 (0) |

표의 `기타`는 위 상세 표에서 주요 작업자 comm을 뺀 comm들의 합계입니다. 상세 표는 두 종류의 풀 이름을 포함해 전체 wakee comm을 보입니다.

## Wakeup waker → wakee 상위 쌍

아래는 Framework 원인 경로의 wakee로 제한한 pair이며, 각 비율은 해당 프로세스의 전체 완료 요청 수 기준입니다. source pair는 공통 풀 작업자 148,337건, target pair는 `ForkJoinPool-1-`, `HTTP-Dispatcher`, `VirtualThread-u` 합계 33,041건을 설명합니다.

| Framework 소스 waker comm → wakee comm | 사건 수 | 요청당 |
|---|---:|---:|
| `swapper` → `ForkJoinPool.co` | 82,535 | 0.268749 |
| `ForkJoinPool.co` → `ForkJoinPool.co` | 59,553 | 0.193916 |
| `zlink-jvm-raw-m` → `ForkJoinPool.co` | 6,191 | 0.020159 |
| `ZLINKbg/IO/2` → `ForkJoinPool.co` | 30 | 0.000098 |
| `JFR Sampler Thr` → `ForkJoinPool.co` | 19 | 0.000062 |
| `bench-phase-act` → `ForkJoinPool.co` | 4 | 0.000013 |
| `VM Thread` → `ForkJoinPool.co` | 4 | 0.000013 |
| 미확인 TID → `ForkJoinPool.co` | 1 | 0.000003 |

| Framework 대상 waker comm → wakee comm | 사건 수 | 요청당 |
|---|---:|---:|
| `swapper` → `ForkJoinPool-1-` | 32,992 | 0.106806 |
| `zlink-jvm-raw-m` → `ForkJoinPool-1-` | 18 | 0.000058 |
| `ZLINKbg/IO/0` → `ForkJoinPool-1-` | 8 | 0.000026 |
| `ZLINKbg/IO/1` → `ForkJoinPool-1-` | 6 | 0.000019 |
| 미확인 TID → `ForkJoinPool-1-` | 6 | 0.000019 |
| `JFR Sampler Thr` → `ForkJoinPool-1-` | 2 | 0.000006 |
| `ForkJoinPool-1-` → `ForkJoinPool-1-` | 2 | 0.000006 |
| `zlink-java-chan` → `ForkJoinPool-1-` | 2 | 0.000006 |
| `swapper` → `HTTP-Dispatcher` | 4 | 0.000013 |
| `swapper` → `VirtualThread-u` | 1 | 0.000003 |

source 공통 풀 작업자 중 관측된 waker comm으로는 `zlink-jvm-raw-m`이 요청당 0.020159회, 공통 풀 작업자끼리의 wakeup이 0.193916회입니다. target 대상 작업자 wakeup의 대부분은 `swapper`가 waker로 기록된 사건이며 요청당 0.106806회입니다. 이는 perf 이벤트의 waker 필드에 따른 집계라 애플리케이션 코드의 직접 호출자를 모두 식별한 결과는 아닙니다.

## Context switch

`prev` TID를 기준으로 집계했습니다. 이 값은 요청 완료나 특정 원인에 연결된 전환 수가 아니라 perf 구간 전체의 프로세스 스레드 context switch입니다.

| 구현 | 프로세스 | sched_switch prev 사건 | 요청당 |
|---|---|---:|---:|
| Framework | 소스 | 220,728 | 0.718731 |
| Framework | 대상 | 129,629 | 0.419653 |
| Binding | 소스 | 117,556 | 0.227059 |
| Binding | 대상 | 153,101 | 0.306744 |

## 새 구간의 스레드 CPU 시간

`window.json`의 스레드별 tick 차이를 10밀리초/tick으로 환산하고 완료 요청 수로 나눴습니다. 풀 합계는 각 풀 작업자의 tick을 합산했습니다.

| 구현·프로세스 | 스레드 또는 그룹 | 새 요청당 CPU | 이전 보고서 | 차이 |
|---|---|---:|---:|---:|
| Framework 소스 | 공통 ForkJoinPool 작업자 7개 합계 | 24.91 µs | 19.11 µs | +5.80 µs |
| Framework 대상 | `ForkJoinPool-1-` 작업자 3개 합계 | 20.52 µs | 17.38 µs | +3.14 µs |
| Framework 대상 | `ZLINKbg/IO/1` | 17.81 µs | 15.00 µs | +2.81 µs |
| Framework 소스 | `ZLINKbg/IO/2` | 17.52 µs | 14.04 µs | +3.48 µs |
| Framework 소스 | `bench-phase-act` | 17.13 µs | 13.46 µs | +3.67 µs |
| Framework 소스 | `ZLINKbg/IO/0` | 16.51 µs | 13.32 µs | +3.19 µs |
| Framework 소스 | `zlink-jvm-raw-m` | 16.09 µs | 12.45 µs | +3.64 µs |
| Framework 대상 | `zlink-jvm-raw-m` | 15.47 µs | 12.03 µs | +3.44 µs |
| Binding 소스 | `bench-phase-act` | 11.70 µs | 11.70 µs | 약 0 |
| Binding 소스 | `ZLINKbg/IO/0` | 10.84 µs | 10.84 µs | 약 0 |
| Binding 소스 | `ZLINKbg/IO/2` | 9.08 µs | 9.08 µs | 약 0 |
| Binding 대상 | `ZLINKbg/IO/0` | 10.52 µs | 10.52 µs | 약 0 |
| Binding 대상 | `ZLINKbg/IO/1` | 10.16 µs | 10.16 µs | 약 0 |
| Binding 대상 | `bench-raw-reque` | 9.22 µs | 9.22 µs | 약 0 |

Framework 새 구간의 공통 풀 작업자 합계는 이전보다 30.4% 높고, 대상 `ForkJoinPool-1-` 합계는 18.1% 높습니다. 주요 Framework 입출력·벤치 스레드 값도 이전보다 요청당 2.81~3.67마이크로초 높습니다. Binding 값은 이전 보고서와 반올림 범위에서 같습니다. 캡처별 실행 구간과 처리량이 다르므로 이 차이를 코드 변화나 단일 스레드의 고정 비용 변화로 해석할 수는 없습니다.

원인 1의 2.06마이크로초는 이전 분석에서 `ZLinkSpringHandlerFactory`가 포함된 JFR 실행 표본을 표본 간격으로 환산한 값입니다. 이번 `window.json`은 OS 스레드 CPU만 기록하므로 Spring 경로 CPU를 직접 재측정하지 않습니다. 대상 `ForkJoinPool-1-`의 20.52마이크로초는 여러 handler의 가상 스레드 실행을 포함할 수 있어 Spring 핸들러만의 CPU 비용으로 분리할 수 없습니다. 원인 2의 24.91마이크로초도 공통 풀 작업자의 후속 응답 디코딩과 완료 연속 작업을 포함하며, 제출 함수만의 실행 시간은 아닙니다.
