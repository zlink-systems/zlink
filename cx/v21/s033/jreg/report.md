# Java Framework request-serial @1 KiB 회귀 조사

## 결과

같은 코드에서 2초 warmup은 약 4.6k msg/s, 30초 warmup은 약 9.4k msg/s였다. 과거 76a4 결과 9,309 msg/s는 30초 warmup으로 재현됐다. 측정한 제품 커밋 중 처리량을 절반으로 낮춘 커밋은 없다.

| commit | zlink-java msg/s (source CPU µs/msg) | Framework msg/s (source CPU µs/msg) | F/B | judged runs (B/F) |
|---|---:|---:|---:|---:|
| `76a4d78506` | 13,855 (30.9) | 4,628 (262.5) | 0.334 | 5/7 |
| `741a58bae2` | 13,827 (31.0) | 4,689 (244.9) | 0.339 | 5/5 |
| `d3fd955131` | 13,712 (30.8) | 4,806 (233.9) | 0.351 | 5/7 |
| `c820f2d3ba` main | 13,796 (31.3) | 4,778 (237.8) | 0.346 | 5/7 |

처리량은 run 중앙값, CPU는 `/proc/<pid>/task` TID 합집합 tick 차이를 완료 요청 수로 나눈 중앙값이다. 처음 5회에서 Framework 범위가 10%를 넘은 76a·d3·main에 2회씩 추가했다. 추가 전후 중앙값 변화는 각각 -4.1%, -3.7%, -1.7%라 안정으로 판정했다(추가 후에도 전체 범위는 부하 변동으로 37.7%, 42.4%, 48.2%). 741은 5회 범위 7.7%, binding은 모든 커밋에서 2.7–4.9%였다.

같은 `76a4d78506`에서 warmup만 30초로 바꾼 보조 측정은 9,382 / 9,423 / 9,033 msg/s(3회 중앙값 9,382, 범위 4.2%)였다. 과거 9,309와 0.8% 차이다. 2초 warmup 76a의 7회 중앙값 4,628과 비교해 처리량이 2.03배였다. 프로세스 CPU도 source/target 각각 262.5/223.4에서 69.7/61.6 µs/msg로 줄었다.

## 원인

- 원인 커밋: **없음**. 76a→741은 Framework +1.3%, 741→d3 +2.5%, d3→현재 main -0.6%로 재현된 하락이 없다. d3(#4245)의 전용 제출 thread나 per-stream metrics 병합은 request-serial 저하를 설명하지 않는다.
- 원인 설정: judged run 전 warmup 30초(과거 i3871)에서 2초(r4190)로 바뀐 측정 조건이다. [runner_common.sh:49](/root/worktree/zlink-v21-s033/jreg/framework/bench/grpc/runner_common.sh:49)는 기본값 2초를 둔다. 과거 결과의 `warmupSeconds`는 30.0이고 이번 기본 측정 결과는 2.0이다.
- 직렬 패턴 코드: [BenchDrivers.java:200](/root/worktree/zlink-v21-s033/jreg/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java:200)는 request-serial에서 `runWorkers(1)`을 부른다. [BenchDrivers.java:481](/root/worktree/zlink-v21-s033/jreg/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java:481)도 하나의 worker와 하나의 metrics stream만 만들므로, #4245의 직렬 요청 경로에 새 병렬 handoff가 추가되지 않았다.
- 성능 근거: 30초 warmup 후 source+target CPU는 131.3 µs/msg, 2초 warmup 후는 485.9 µs/msg였다. 동일 코드·동일 binding 1.23.0에서 차이가 나고, 긴 warmup 결과가 과거 수치를 복원하므로 제품 hot path 회귀가 아니다.

제안: Java serial benchmark 비교에는 warmup을 30초로 고정하고 결과 메타데이터에서 이를 유지한다. 제품 코드는 수정하지 않았다.

## Core 및 측정 조건

요청한 Core 1.24.0 prefix는 모든 커밋에 적용할 수 없었다. Java binding 빌드는 커밋과 일치하는 Core 버전을 검증한다. 76a의 `bindings/java/build.gradle`은 1.23.0을 요구하고, c820은 1.25.0을 요구한다. 따라서 실제 Framework classpath의 Maven binding/Core는 76a=1.23.0, 741/d3=1.24.0, c820=1.25.0이었다. 741/d3는 요청한 `/root/.cache/zlink/core-release/1.24.0/linux-x64` provenance와 일치한다. Gradle bench build 자체는 성공했지만 prefix 변수를 넘긴 것만으로 Maven 의존성의 Core 버전은 덮어쓰지 않았다.

- runner: `framework/bench/grpc/java/run_local.sh`; warmup 2초, judged 5초, 기본 조건에 맞춘 5회. 구현은 `zlink-java,zlink-framework-java`, request-serial, 1024 byte만 측정했다.
- 배치: source CPU 0–7, target CPU 8–15; lock 안에서 CPU 0–15 governor `performance`, 종료 trap에서 `powersave` 복구. 마지막 governor는 `powersave`다.
- 각 셀 raw에 시작/종료 uptime·load·free memory·CPU 주파수와 source/target TID snapshot이 있다. 전체 조건 로그 범위: 1분 load 1.60–2.88, free memory 3.77–5.33 GB, scaling_cur_freq 0.4–4.52 GHz. 커밋별 lock 시작/종료는 `raw/lock-windows/`에 기록했다.
- 빌드: 각 버전에서 `ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 bash framework/bench/grpc/java/build.sh` (shared measure lock); 모두 성공. Core 버전 차이는 위와 같다.

원시 결과·조건·CPU snapshots·빌드/driver 로그: `/root/jobs/fx/cx/v21/s033/jreg/raw/`. worktree는 원래 detached commit `d3fd955131`에 두었고, commit·push 및 제품 코드 변경은 없다.
