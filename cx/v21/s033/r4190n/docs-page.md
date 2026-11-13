# #4190 재측정 및 #4162 A/A

측정 commit: `d3fd955131c1942dc46203fde365f1c690b3d0f6` (#4245 merge). 기존 측정 설정은 warmup 2초, active 5초, judged run 3회, 8 submit stream, payload 1 KiB·4 KiB다. 이번 C backpressure 보충은 두 크기별 10회 실행했다. 집계는 #4230 aggregator, `--min-runs 3` 기준이다.

## 처리량 및 기준 판정

처리량은 run 중앙값(msg/s)이다. 각 행의 기준 열은 aggregator 출력 그대로이며 `공개/통과`, `공개/실패`, `미판정/—`와 G5/G6 사유를 함께 적었다. 근거는 aggregate JSON의 구현별 행 및 judgement 행이다.

### C++
언어 판정: **미완료** — 2/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 7,704 | 13,743 | 8,822 | 0.642 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:28–72; r4190n/raw/aggregates/cpp.json:73–117; r4190n/raw/aggregates/cpp.json:118–162; r4190n/raw/aggregates/cpp.json:862–871 |
| request-serial | 4096 | 7,348 | 12,873 | 7,992 | 0.621 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:163–207; r4190n/raw/aggregates/cpp.json:208–252; r4190n/raw/aggregates/cpp.json:253–297; r4190n/raw/aggregates/cpp.json:872–881 |
| request-backpressure | 1024 | n/a | 151,793 | 47,540 | 0.313 · 공개/실패 · 양쪽 G5 통과 | 1.099 · 공개/통과 · 양쪽 G5 통과 | r4190n/raw/aggregates/cpp.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/cpp.json:343–387; r4190n/raw/aggregates/cpp.json:388–432; r4190n/raw/aggregates/cpp.json:882–891; r4190n/raw/aggregates/cpp.json:842–851 |
| request-backpressure | 4096 | n/a | 141,454 | 45,806 | 0.324 · 공개/실패 · 양쪽 G5 통과 | 1.356 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/cpp.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/cpp.json:478–522; r4190n/raw/aggregates/cpp.json:523–567; r4190n/raw/aggregates/cpp.json:892–901; r4190n/raw/aggregates/cpp.json:852–861 |
| send-saturation | 1024 | 24,393 | 592,105 | 112,939 | 0.191 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:568–612; r4190n/raw/aggregates/cpp.json:613–657; r4190n/raw/aggregates/cpp.json:658–702; r4190n/raw/aggregates/cpp.json:902–911 |
| send-saturation | 4096 | 23,987 | 255,994 | 110,435 | 0.431 · 미판정/— · 분모 G5 변동 44.4%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:703–747; r4190n/raw/aggregates/cpp.json:748–792; r4190n/raw/aggregates/cpp.json:793–837; r4190n/raw/aggregates/cpp.json:912–921 |

#### C++ CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 49.7/94.2 | 38.7/35.9 | 59.9/70.4 | 58.4/45.8 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:4, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:58, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:112, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 51.6/96.4 | 39.5/37.9 | 68.1/73.8 | 61.8/49.1 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:13, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:67, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:121, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 12.5/11.8 | 62.8/56.6 | 51.5/13.5 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:40, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:94, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 20.4/18.7 | 71.7/60.5 | 48.4/23.3 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:49, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:103, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 46.4/89.4 | 1.9/2.8 | 9.1/16.9 | 9.8/3.7 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:22, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:76, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:130, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 48.0/91.2 | 3.1/4.8 | 9.1/18.4 | 14.4/8.8 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:31, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:85, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:139, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### .NET
언어 판정: **미완료** — 1/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 7,261 | 10,186 | 6,153 | 0.604 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:28–72; r4190n/raw/aggregates/dotnet.json:73–117; r4190n/raw/aggregates/dotnet.json:118–162; r4190n/raw/aggregates/dotnet.json:862–871 |
| request-serial | 4096 | 6,340 | 9,465 | 5,943 | 0.628 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:163–207; r4190n/raw/aggregates/dotnet.json:208–252; r4190n/raw/aggregates/dotnet.json:253–297; r4190n/raw/aggregates/dotnet.json:872–881 |
| request-backpressure | 1024 | n/a | 105,162 | 47,116 | 0.448 · 공개/실패 · 양쪽 G5 통과 | 0.761 · 공개/실패 · 양쪽 G5 통과 | r4190n/raw/aggregates/dotnet.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/dotnet.json:343–387; r4190n/raw/aggregates/dotnet.json:388–432; r4190n/raw/aggregates/dotnet.json:882–891; r4190n/raw/aggregates/dotnet.json:842–851 |
| request-backpressure | 4096 | n/a | 97,997 | 38,287 | 0.391 · 공개/실패 · 양쪽 G5 통과 | 0.939 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/dotnet.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/dotnet.json:478–522; r4190n/raw/aggregates/dotnet.json:523–567; r4190n/raw/aggregates/dotnet.json:892–901; r4190n/raw/aggregates/dotnet.json:852–861 |
| send-saturation | 1024 | 23,231 | 436,668 | 194,202 | 0.445 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:568–612; r4190n/raw/aggregates/dotnet.json:613–657; r4190n/raw/aggregates/dotnet.json:658–702; r4190n/raw/aggregates/dotnet.json:902–911 |
| send-saturation | 4096 | 21,597 | 181,435 | 136,051 | 0.750 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:703–747; r4190n/raw/aggregates/dotnet.json:748–792; r4190n/raw/aggregates/dotnet.json:793–837; r4190n/raw/aggregates/dotnet.json:912–921 |

#### .NET CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 436.0/341.3 | 197.5/46.2 | 263.3/91.3 | 58.4/45.8 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:4, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:58, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:112, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 448.6/359.6 | 216.0/44.5 | 272.0/90.7 | 61.8/49.1 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:13, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:67, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:121, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 31.0/11.6 | 81.2/46.3 | 51.5/13.5 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:40, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:94, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 33.4/19.1 | 100.3/53.7 | 48.4/23.3 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:49, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:103, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 240.0/244.0 | 3.9/4.0 | 5.5/14.0 | 9.8/3.7 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:22, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:76, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:130, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 252.3/265.3 | 10.3/9.9 | 10.6/20.7 | 14.4/8.8 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:31, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:85, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:139, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### Java
언어 판정: **미완료** — 4/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 8,341 | 13,737 | 5,641 | 0.411 · 미판정/— · 분자 G5 변동 14.8%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:28–72; r4190n/raw/aggregates/java.json:118–162; r4190n/raw/aggregates/java.json:73–117; r4190n/raw/aggregates/java.json:862–871 |
| request-serial | 4096 | 7,958 | 13,151 | 4,535 | 0.345 · 미판정/— · 분자 G5 변동 15.0%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:163–207; r4190n/raw/aggregates/java.json:253–297; r4190n/raw/aggregates/java.json:208–252; r4190n/raw/aggregates/java.json:872–881 |
| request-backpressure | 1024 | n/a | 145,331 | 37,719 | 0.260 · 공개/실패 · 양쪽 G5 통과 | 1.052 · 공개/통과 · 양쪽 G5 통과 | r4190n/raw/aggregates/java.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/java.json:388–432; r4190n/raw/aggregates/java.json:343–387; r4190n/raw/aggregates/java.json:882–891; r4190n/raw/aggregates/java.json:842–851 |
| request-backpressure | 4096 | n/a | 104,767 | 35,645 | 0.340 · 미판정/— · 분자 G5 변동 13.0%(한도 10%) 초과 | 1.004 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/java.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/java.json:523–567; r4190n/raw/aggregates/java.json:478–522; r4190n/raw/aggregates/java.json:892–901; r4190n/raw/aggregates/java.json:852–861 |
| send-saturation | 1024 | 32,915 | 553,834 | 106,351 | 0.192 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:568–612; r4190n/raw/aggregates/java.json:658–702; r4190n/raw/aggregates/java.json:613–657; r4190n/raw/aggregates/java.json:902–911 |
| send-saturation | 4096 | 31,933 | 203,852 | 102,629 | 0.503 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:703–747; r4190n/raw/aggregates/java.json:793–837; r4190n/raw/aggregates/java.json:748–792; r4190n/raw/aggregates/java.json:912–921 |

#### Java CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 111.0/110.2 | 33.7/42.7 | 198.6/196.2 | 58.4/45.8 | r4190m/raw/bench/java/cpu-per-cell.json:4, r4190m/raw/bench/java/cpu-per-cell.json:112, r4190m/raw/bench/java/cpu-per-cell.json:58, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 111.7/114.0 | 35.7/44.8 | 198.7/209.4 | 61.8/49.1 | r4190m/raw/bench/java/cpu-per-cell.json:13, r4190m/raw/bench/java/cpu-per-cell.json:121, r4190m/raw/bench/java/cpu-per-cell.json:67, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 10.2/15.5 | 81.0/91.2 | 51.5/13.5 | r4190m/raw/bench/java/cpu-per-cell.json:94, r4190m/raw/bench/java/cpu-per-cell.json:40, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 17.0/24.1 | 85.9/96.7 | 48.4/23.3 | r4190m/raw/bench/java/cpu-per-cell.json:103, r4190m/raw/bench/java/cpu-per-cell.json:49, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 57.6/61.9 | 2.3/3.4 | 10.2/24.9 | 9.8/3.7 | r4190m/raw/bench/java/cpu-per-cell.json:22, r4190m/raw/bench/java/cpu-per-cell.json:130, r4190m/raw/bench/java/cpu-per-cell.json:76, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 59.5/64.0 | 7.6/9.4 | 11.1/27.3 | 14.4/8.8 | r4190m/raw/bench/java/cpu-per-cell.json:31, r4190m/raw/bench/java/cpu-per-cell.json:139, r4190m/raw/bench/java/cpu-per-cell.json:85, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### Node
언어 판정: **미완료** — 8/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 3,196 | 9,081 | 5,425 | 0.597 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:28–72; r4190n/raw/aggregates/node.json:118–162; r4190n/raw/aggregates/node.json:73–117; r4190n/raw/aggregates/node.json:862–871 |
| request-serial | 4096 | 3,041 | 8,812 | 5,080 | 0.576 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:163–207; r4190n/raw/aggregates/node.json:253–297; r4190n/raw/aggregates/node.json:208–252; r4190n/raw/aggregates/node.json:872–881 |
| request-backpressure | 1024 | n/a | 31,684 | 16,486 | 0.520 · 미판정/— · 분자·분모 클라이언트 포화(G6) | 0.229 · 미판정/— · 분자 클라이언트 포화(G6) | r4190n/raw/aggregates/node.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/node.json:388–432; r4190n/raw/aggregates/node.json:343–387; r4190n/raw/aggregates/node.json:882–891; r4190n/raw/aggregates/node.json:842–851 |
| request-backpressure | 4096 | n/a | 27,102 | 19,174 | 0.707 · 미판정/— · 분자·분모 G5 변동 11.9/12.2%(한도 10%) 초과 | 0.260 · 미판정/— · 분자·분모 G5 변동 12.2/38.9%(한도 10%) 초과 | r4190n/raw/aggregates/node.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/node.json:523–567; r4190n/raw/aggregates/node.json:478–522; r4190n/raw/aggregates/node.json:892–901; r4190n/raw/aggregates/node.json:852–861 |
| send-saturation | 1024 | 5,774 | 117,787 | 76,566 | 0.650 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:568–612; r4190n/raw/aggregates/node.json:658–702; r4190n/raw/aggregates/node.json:613–657; r4190n/raw/aggregates/node.json:902–911 |
| send-saturation | 4096 | 5,461 | 87,574 | 54,986 | 0.628 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:703–747; r4190n/raw/aggregates/node.json:793–837; r4190n/raw/aggregates/node.json:748–792; r4190n/raw/aggregates/node.json:912–921 |

#### Node CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 318.8/157.2 | 81.5/140.5 | 162.7/116.8 | 58.4/45.8 | r4190m/raw/bench/node/cpu-per-cell.json:4, r4190m/raw/bench/node/cpu-per-cell.json:112, r4190m/raw/bench/node/cpu-per-cell.json:58, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 345.2/167.7 | 83.3/147.7 | 159.2/120.1 | 61.8/49.1 | r4190m/raw/bench/node/cpu-per-cell.json:13, r4190m/raw/bench/node/cpu-per-cell.json:121, r4190m/raw/bench/node/cpu-per-cell.json:67, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 59.7/63.2 | 116.1/52.8 | 51.5/13.5 | r4190m/raw/bench/node/cpu-per-cell.json:94, r4190m/raw/bench/node/cpu-per-cell.json:40, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 65.8/70.9 | 89.1/69.5 | 48.4/23.3 | r4190m/raw/bench/node/cpu-per-cell.json:103, r4190m/raw/bench/node/cpu-per-cell.json:49, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 190.5/75.9 | 14.7/10.7 | 19.9/15.7 | 9.8/3.7 | r4190m/raw/bench/node/cpu-per-cell.json:22, r4190m/raw/bench/node/cpu-per-cell.json:130, r4190m/raw/bench/node/cpu-per-cell.json:76, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 207.2/79.5 | 32.5/22.0 | 30.2/25.4 | 14.4/8.8 | r4190m/raw/bench/node/cpu-per-cell.json:31, r4190m/raw/bench/node/cpu-per-cell.json:139, r4190m/raw/bench/node/cpu-per-cell.json:85, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

## C 기준 재측정 및 이전 누락 원인

r4190m의 1 KiB C run은 오류 0인 run1 한 개뿐이었다(127,492 msg/s, peak in-flight 15,783; `r4190m/raw/bench/c/run1/zlink-c-request-backpressure-1024/results.json:16,27,34`). run2·run3에는 각각 14,031·13,982 오류가 있었다(`r4190m/raw/bench/c/run2/zlink-c-request-backpressure-1024/results.json:34`, `r4190m/raw/bench/c/run3/zlink-c-request-backpressure-1024/results.json:34`). 4 KiB는 run1·run2 두 개가 오류 0(각 67,698·13,661 msg/s, peak 922·770), run3에는 205 오류가 있었다(`r4190m/raw/bench/c/run1/zlink-c-request-backpressure-4096/results.json:16,27,34`; `r4190m/raw/bench/c/run2/zlink-c-request-backpressure-4096/results.json:16,27,34`; `r4190m/raw/bench/c/run3/zlink-c-request-backpressure-4096/results.json:34`). 따라서 집계 유효 run 수는 1/2로 G5 최소 3회를 채우지 못해 B/C가 미판정이었다. 기존 raw는 처리량·오류·in-flight를 기록하지만 변동 원인은 설명하지 않는다.

새 보충 실행 10회/크기에서 오류는 모두 0이었다. 1 KiB 중앙값 138,108 msg/s, G5 변동 7.3%로 통과했다. 4 KiB 중앙값 104,342 msg/s지만 G5 변동 38.9%로 실패했다. 두 번의 보충 묶음 모두 4 KiB raw에 약 68–71k와 137–145k msg/s 두 군집이 있고, run logs는 `route ready=true`와 throughput/error만 남긴다. conditions 파일에는 uptime/load·메모리·주파수만 있고 (`r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/conditions-before.txt:1-8`), source log도 route 준비·처리량·오류만 남긴다 (`r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/source.log:1-2`). 따라서 그 차이의 원인은 raw에서 확정할 수 없다. 새 10회 중앙값과 G5 행은 `r4190n/raw/aggregates/cpp.json:299-340,434-476`이며, 4 KiB 분모가 G5를 통과하지 못해 네 언어 모두 해당 크기의 binding/C는 미판정이다.

새 보충 run별 처리량은 `r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run2/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run3/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run4/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run5/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run1/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run2/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run3/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run4/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run5/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run2/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run3/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run4/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run5/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run1/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run2/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run3/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run4/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run5/zlink-c-request-backpressure-4096/results.json:16`; 오류와 peak in-flight는 같은 파일의 각 `:27,34` 행에 있다. run별 조건은 해당 셀의 `conditions-before.txt`·`conditions-after.txt`에 있다.

## Kotlin 보충 셀

요청 직렬 @1 KiB, 3회; gRPC Kotlin 및 Framework Kotlin. 처리량은 중앙값이다.

| 구현 | msg/s | source/target CPU µs/msg | raw 근거 |
|---|---:|---:|---|
| gRPC Kotlin | 6,725 | 151.6/119.3 | CPU r4190m/raw/bench/kotlin/cpu-per-cell.json:4; throughput r4190m/raw/bench/kotlin/run1/grpc-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run2/grpc-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run3/grpc-kotlin-request-serial-1024/results.json:14 |
| Framework Kotlin | 4,358 | 286.3/252.4 | CPU r4190m/raw/bench/kotlin/cpu-per-cell.json:13; throughput r4190m/raw/bench/kotlin/run1/zlink-framework-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run2/zlink-framework-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run3/zlink-framework-kotlin-request-serial-1024/results.json:14 |

## A/A (#4162)

20개 `aa-clean` run raw에는 단일 `latency`, `latency_p95`, `latency_p99` 값만 있다. p50 필드와 샘플별 지연값은 없다. 따라서 per-run p50, p50 중앙값/범위 및 중앙값 대비 ±5% 판정은 **자료 부족으로 미판정**이다. 예: `f4162/raw/aa-clean-01.log:5-7`, `aa-clean-20.log:5-7`; 같은 형식의 `aa-clean-{01..20}.log` 전체를 확인했다.

참고로 단일 latency 값의 중앙값은 0.006109 ms, 범위는 0.003244–0.047212 ms다. 이 값은 p50으로 볼 수 없다. p95 중앙값/범위는 0.004688 / 0.003424–0.150528 ms, p99는 0.059520 / 0.016896–1.400832 ms다. 통계 입력은 `f4162/raw/aa-clean-{01..20}.log:5-7`; 각 run별 원시 행을 보존했다.

## r4190m 대비 바뀐 것

- 모든 언어 throughput 표에 빠졌던 request-backpressure 두 크기와 해당 aggregator 판정·사유를 추가했다.
- C backpressure 10회/크기를 추가했다. 1 KiB B/C는 이제 판정 가능하고, 4 KiB는 G5 변동으로 미판정이다.
- A/A p50을 기존 analyzer 결과에서 추정하지 않았다. 원시에 p50/샘플이 없음을 확인해 미판정으로 고쳤다.

## 측정 조건 및 결과 위치

- C 재측정 lock window: 첫 묶음 13:16:12–13:17:34 UTC (`raw/c-rerun2-lock-{before,after}.txt:1`), 두 번째 묶음 13:18:56–13:20:18 UTC (`raw/c-rerun3-lock-{before,after}.txt:1`). 두 창 모두 cpu0–15 governor `performance`, 종료 시 trap으로 `powersave` 복원. placement B(source 0–7, target 8–15).
- uptime/load, free memory, cpu0–15 `scaling_cur_freq`는 매 셀 `conditions-before.txt`/`conditions-after.txt`에 저장했다. fresh C run 폴더는 `raw/bench/c-backpressure-rerun2/`, `raw/bench/c-backpressure-rerun3/`; 각 결과 JSON의 `metadata`는 run 설정을 담는다. A/A 조건 전·후는 `f4162/raw/aa-clean-conditions-before.txt`, `aa-clean-conditions-after.txt`다.
- 언어 측정과 기존 CPU supplement는 r4190m raw를 그대로 참조한다. 네 새 aggregate는 fresh C backpressure와 기존 언어 run을 함께 넣어 재생성했다: `raw/aggregates/{cpp,dotnet,java,node}.{md,json}`.
- 원시 출력은 삭제하지 않았다. C 기준용 server/client만 `cpp-build-slot.sh`를 통해 `/root/jobs/fx/cx/v21/s033/r4190n/build/c`에 빌드했고 Framework runtime은 재빌드하지 않았다.

## 남은 문제

- C request-backpressure @4 KiB G5 변동폭 38.9%로 판단 불가; 네 언어의 binding/C @4 KiB도 aggregator가 미판정 처리한다.
- A/A p50은 보존된 원시 데이터에 없어 계산·판정할 수 없다.
