# Stream Compare Benchmark (한국어)

## 목적

`benchwithstreamcompare`는 공통 벤치 흐름에서 `zlink`와 다른 stream 스택의
echo 성능을 비교하기 위한 벤치마크입니다.

핵심 목표:

- 하나의 공통 클라이언트로 라이브러리 성능 비교
- 순수 stream socket echo에 가까운 측정
- 동시 실행 방지로 측정 간 간섭 최소화
- 케이스당 1회 실행에서 throughput/latency 동시 측정

## 구성 요소

- `run_benchmarks.sh`: 전체 벤치 실행 스크립트
- `client/bench_stream_client.cpp`: 공통 벤치 클라이언트
- `stacks/*`: 스택별 서버 소스/프로젝트 파일
- `run_comparison.py`: 요약/랭킹 리포트 생성

지원 스택:

- `asio`
- `cppserver`
- `dotnet`
- `netzlink`
- `jvmzlink`
- `zlink`
- `zmq`
- `netty`

지원 사이즈:

- `64`
- `1024`
- `65536`

## 공정성 설계 포인트

- 모든 스택은 기본 서버 설정에서 framed echo 경로로 실행
- 모든 스택에 동일 클라이언트 바이너리 사용
- 클라이언트 wire format은 모든 스택에서 고정:
  `4-byte big-endian 길이 + payload`
- 스택은 순차 실행(동시 벤치 금지)
- 멀티 사이즈 실행 시 사이즈마다 재연결
  : 서버 재시작 + 클라이언트 재연결
- 실행 순서는 size 기준 우선이며, 각 size에서 지정한 run 수만큼 반복
  (`size -> run -> stack`)
- 사이즈 요약 출력은 해당 size의 모든 run 완료 후 1회 생성

## 요구 사항

- Linux 환경
- CMake + C++ 컴파일러
- Python 3
- .NET SDK (`dotnet`, `netzlink` 스택 사용 시)
- JDK 22 + Gradle 8.8+ (`netty`, `jvmzlink` 스택 사용 시)
- 선택한 스택에 따른 외부 의존성

고CCU(예: `--ccu 10000`)에서는 OS 튜닝이 중요합니다:

- 파일 디스크립터 제한 (`ulimit -n`)
- ephemeral port 범위 (`net.ipv4.ip_local_port_range`)
- backlog/TCP 메모리 관련 커널 설정

예시:

```bash
ulimit -n
cat /proc/sys/net/ipv4/ip_local_port_range
```

## 빠른 시작

저장소 루트에서 실행:

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh
```

특정 스택/사이즈 실행:

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh \
  --stack zlink,zmq,dotnet \
  --size 65536 \
  --ccu 10000 \
  --runs 3 \
  --warmup 3 \
  --duration 5
```

멀티 사이즈 순차 측정(사이즈별 재연결):

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh \
  --stack zlink \
  --size 64,1024,65536
```

## 실행 옵션

```text
--stack <asio|cppserver|dotnet|netzlink|jvmzlink|zlink|zmq|netty|all|csv>
--size <64|1024|65536|all|csv>
--build-dir PATH            기본값: core/build
--reuse-build               기존 빌드 디렉토리 재사용(configure/build 생략)
--clean-build               빌드 디렉토리 삭제 후 클린 빌드
--ccu <N>                    기본값: 1000
--runs <N>                   기본값: 1 (size별 반복)
--warmup <sec>               기본값: 3
--duration <sec>             기본값: 5
--client-io-threads <N>      기본값: 4
--server-io-threads <N>      기본값: 4
--resource-sample-ms <N>     기본값: 500
--server-start-timeout <sec> 기본값: 40
--stack-gap <sec>            기본값: 5
```

지원 환경변수:

- `RESULT_DIR`: 결과 출력 디렉토리 지정
- `HOST`: 벤치 대상 호스트 (기본값 `127.0.0.1`)
- `BASE_PORT`: 스택 실행 시작 포트 (기본값 `22000`)
- `NETTY_JAVA_HOME`: `netty`, `jvmzlink` 스택에서 사용할 JDK 22 경로
- `NETTY_GRADLE_BIN`: `netty`, `jvmzlink` 스택에서 사용할 Gradle 실행 파일 경로

참고:

- `/tmp/bench_streamcompare.lock` 파일락으로 동시 실행 방지
- 기본 빌드 모드는 incremental이며, `--reuse-build`와 `--clean-build`는 상호 배타적
- `--reuse-build`는 기존 빌드 디렉토리/바이너리가 이미 있어야 동작
- `--clean-build`는 core 빌드 디렉토리와 스택 로컬 빌드 산출물을 삭제 후 재빌드
- 일부 스택 빌드 실패 시 전체 중단 대신 `skip`으로 기록
- `netty`는 JDK 22 이상이 필요하며 탐색 우선순위는
  `NETTY_JAVA_HOME -> JAVA_HOME -> PATH java` 순서
- `netty`는 Gradle 8.8+가 필요하며 시스템 `gradle`이 오래된 경우
  러너가 `bindings/c/bench/with_stream/stacks/netty/.gradle-tools/` 아래에
  Gradle `8.10.2`를 자동 다운로드해서 사용
- `jvmzlink`는 `netty`와 동일한 Java/Gradle 탐색 경로를 사용하고,
  실행 전 `bindings/java` jar를 먼저 빌드해 서버 앱을 패키징
- `zlink` 스택은 native STREAM 서버 바이너리를 직접 실행

## 결과 파일

기본 결과 경로:

- `bindings/c/bench/with_stream/results/<timestamp>/`

생성 파일:

- `metrics.csv`: 케이스별 원시 측정치
- `summary.json`: 요약 통계
- `comparison.md`: 사람이 읽기 쉬운 리포트
- `skipped_stacks.csv`: 제외된 스택과 사유
- `logs/*_client.log`, `logs/*_server.log`: 스택별 로그
- `logs/*_client_resource.csv`, `logs/*_server_resource.csv`: 프로세스 사용량 샘플
- `logs/*_system_resource.csv`: 스택 실행 구간의 호스트 전체 사용량 샘플

`metrics.csv` 주요 필드:

- `throughput_bps`, `throughput_mib_s`
- `p50_us`, `p95_us`, `p99_us`
- `connect_ok`, `connect_fail`
- `send_err`, `recv_err`, `timeout`
- `pass_fail`
- `client_avg_cpu_pct`, `client_peak_cpu_pct`
- `client_avg_rss_kb`, `client_peak_rss_kb`, `client_peak_hwm_kb`
- `server_avg_cpu_pct`, `server_peak_cpu_pct`
- `server_avg_rss_kb`, `server_peak_rss_kb`, `server_peak_hwm_kb`
- `system_avg_cpu_pct`, `system_peak_cpu_pct`
- `system_avg_mem_used_kb`, `system_peak_mem_used_kb`
- `system_avg_mem_used_pct`, `system_peak_mem_used_pct`

해석 주의:

- `(stack, size, run)` 단위에서 한 번의 실행으로 throughput/latency를 함께
  측정합니다. 리포트는 호환성을 위해 throughput/latency 섹션을 유지하지만,
  두 섹션 모두 동일 측정행을 사용합니다.

## PASS 기준

아래 조건을 모두 만족하면 `PASS`:

- 연결 성공
- `send/recv/timeout` 오류 0
- throughput 양수

## 사이즈 간 오염 점검 방법

1. 멀티 사이즈 1회 실행
2. 동일 옵션으로 사이즈별 단독 실행
3. throughput 차이와 오류 카운터 비교
4. 필요 시 `--runs` 증가 후 median 기준 비교

## 현재 제한 사항

- 출력 호환을 위해 `phase` 라벨은 유지하지만, 실제 수집은 단일 패스입니다.
