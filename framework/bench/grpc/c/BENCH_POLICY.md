# zlink Comparison Benchmark Policy

> **적용 범위**: `bindings/c/bench/` — 라이브러리 비교 벤치마크 전체
> **Policy Version**: 0.1 (초안)
> **Date**: 2026-02-24
> **Scope**: 실사용 시나리오 기반의 다른 라이브러리/스택과의 성능 비교 벤치마크 정책
>
> 본 정책은 zlink 자체의 회귀 테스트(`bindings/c/perf`)가 아닌, **다른 라이브러리·스택과의 실사용 시나리오 기반 성능 비교**를 위한 벤치마크 규칙을 정의한다. 모든 bench suite는 **multi-client** 테스트만 수행한다.

---

## 1. perf와의 차이

| 항목 | perf (성능 테스트) | bench (비교 벤치마크) |
|------|-------------------|---------------------|
| 목적 | zlink 자체 성능 추적 및 회귀 감지 | 다른 라이브러리·스택과의 실사용 시나리오 비교 |
| 비교 대상 | 이전 버전 자신 (baseline) | 동일 패턴의 다른 라이브러리 구현 |
| 운영 모드 | Observe / Trend / Gate | Observe 전용 |
| 프로세스 모델 | single(1프로세스) + multi(server/client) | multi(server/client) 전용 |
| 결과 해석 | 회귀 여부 판정 | 라이브러리 간 상대 성능 차이 |

---

## 2. Bench Suite 개요

bench는 비교 목적에 따라 3개 suite로 구분한다. 각 suite는 비교군과 시나리오가 다르다.

| Suite | 디렉터리 | 비교 목적 | 비교군 |
|-------|----------|-----------|--------|
| **with_zmq** | `bindings/c/bench/with_zmq/` | 동일 API 계열(ZMTP) 내 소켓 패턴별 성능 비교 | zlink, libzmq |
| **with_routing** | `bindings/c/bench/with_routing/` | 라우팅 프로토콜 간 echo 성능 비교 | zlink (ROUTER), libzmq (ROUTER), gRPC (Bidirectional Streaming) |
| **with_stream** | `bindings/c/bench/with_stream/` | TCP 스트림 스택 간 대규모 동시 연결 성능 비교 | zlink, zlink-len32be, netzlink, netzlink-len32be, jvmzlink, jvmzlink-len32be, asio, cppserver, dotnet, zmq, netty |

### 2.1 with_zmq — 소켓 패턴 비교

**목적**: zlink과 libzmq가 동일한 소켓 패턴(DEALER, ROUTER, PUBSUB, STREAM 등)을 사용할 때의 성능 차이를 측정한다.

| 항목 | 값 |
|------|---|
| 비교군 | `zlink`, `libzmq` |
| 시나리오 | N:1 echo / one-way (multi-client) |
| transport | tcp, inproc, ipc |
| 기본 clients | 100 |
| 프로세스 모델 | 단일 프로세스 (server+clients 스레드) |

**비교군 특성**:

| 특성 | zlink | libzmq |
|------|-------|--------|
| API | zlink C API (`zlink.h`) | ZMQ C API (`zmq.h`) |
| 프로토콜 | ZMTP 호환 | ZMTP |
| 빌드 | CMake 프로젝트 빌드 | 사전 빌드 배포본 (`libzmq_dist/`) |

**지원 패턴**:

| 패턴 | 방향 | 소켓 타입 |
|------|------|-----------|
| MULTI_DEALER_DEALER | echo | DEALER ↔ DEALER |
| MULTI_DEALER_ROUTER | echo | DEALER ↔ ROUTER |
| MULTI_ROUTER_ROUTER | echo | ROUTER ↔ ROUTER |
| MULTI_PUBSUB | one-way | PUB → SUB |
| MULTI_STREAM | echo | STREAM ↔ STREAM |

### 2.2 with_routing — 라우팅 프로토콜 비교

**목적**: 서버 간 라우팅 echo 통신에서 ZMTP 기반(zlink/libzmq)과 HTTP/2 기반(gRPC)의 성능 차이를 비교한다.

| 항목 | 값 |
|------|---|
| 비교군 | `zlink`, `libzmq`, `gRPC` (선택) |
| 시나리오 | N:1 echo (ROUTER echo) |
| transport | tcp |
| 기본 clients | 100 |
| 프로세스 모델 | server/client 별도 프로세스 |

**비교군 특성**:

| 특성 | zlink | libzmq | gRPC |
|------|-------|--------|------|
| 패턴 | ROUTER-ROUTER | ROUTER-ROUTER | Bidirectional Streaming |
| 프로토콜 | ZMTP | ZMTP | HTTP/2 |
| IO 모델 | 이벤트 루프, N 소켓 순회 | 이벤트 루프, N 소켓 순회 | N 스레드, 독립 스트림 |
| 연결 | 클라이언트당 1 TCP | 클라이언트당 1 TCP | 클라이언트당 1 HTTP/2 채널 |

### 2.3 with_stream — TCP 스트림 스택 비교

**목적**: 대규모 동시 연결(10000 CCU) 환경에서 다양한 TCP 스트림 스택의 throughput, latency, 리소스 효율을 비교한다.

| 항목 | 값 |
|------|---|
| 비교군 | zlink, zlink-len32be, netzlink, netzlink-len32be, jvmzlink, jvmzlink-len32be, asio, cppserver, dotnet, zmq, netty |
| 시나리오 | N:1 TCP stream echo |
| transport | tcp |
| 기본 clients | 10000 |
| 프로세스 모델 | server/client 별도 프로세스 |
| 측정 phase | throughput phase, latency phase (분리 측정) |

**비교군 분류**:

| 분류 | 스택 | 언어/런타임 |
|------|------|------------|
| zlink 계열 | zlink, zlink-len32be | C (native) |
| zlink 바인딩 | netzlink, netzlink-len32be, jvmzlink, jvmzlink-len32be | .NET, JVM |
| 네이티브 | asio, cppserver | C++ |
| 매니지드 | dotnet, netty | .NET, JVM |
| 레거시 | zmq | C (libzmq) |

---

## 3. 디렉터리 구조

```text
bindings/c/bench/
├── BENCH_POLICY.md                                    # 비교 벤치마크 정책 (본 문서)
├── with_zmq/                                          # Suite 1: zlink vs libzmq
│   ├── CMakeLists.txt
│   ├── run_benchmarks.sh                              # 실행 스크립트
│   ├── common/                                        # 공통 헤더
│   │   ├── bench_common.hpp                           # libzmq용
│   │   ├── bench_common_zlink.hpp                     # zlink용
│   │   └── bench_common_multi.hpp                     # multi 설정
│   ├── multi/
│   │   ├── common/                                    # multi 프레임워크
│   │   ├── libzmq/                                    # libzmq 구현
│   │   └── zlink/                                     # zlink 구현
│   ├── libzmq/libzmq_dist/                            # libzmq 배포본
│   └── results/                                       # 결과
│       ├── tmp/
│       ├── report/
│       └── baseline/
├── with_routing/                                      # Suite 2: routing 비교
│   ├── CMakeLists.txt
│   ├── run_benchmarks.sh
│   ├── run_comparison.py
│   ├── common/bench_router_compare_common.hpp
│   ├── zlink/                                         # zlink ROUTER server/client
│   ├── libzmq/                                        # libzmq ROUTER server/client
│   ├── grpc/                                          # gRPC server/client (선택)
│   └── results/
└── with_stream/                                       # Suite 3: stream 스택 비교
    ├── run_benchmarks.sh
    ├── run_comparison.py
    ├── stacks/                                        # 스택별 server/client 구현
    │   ├── zlink/
    │   ├── asio/
    │   ├── cppserver/
    │   ├── dotnet/
    │   ├── zmq/
    │   ├── netty/
    │   └── ...
    └── results/
```

---

## 4. 프로세스 모델

모든 bench suite는 **multi-client** 테스트만 수행한다.

### 4.1 with_zmq

```text
┌─ 스크립트 ────────────────────────────────────────────┐
│  for lib in [libzmq, zlink]:                          │
│    spawn bench_binary(lib, pattern, transport, sizes)  │
│    ← RESULT lines (stdout)                            │
│    run_cooldown                                        │
└───────────────────────────────────────────────────────┘
```

- **단일 프로세스**: server+clients가 스레드로 동작하는 단일 바이너리
- 각 비교군 바이너리를 순차 실행하고 RESULT line을 수집

### 4.2 with_routing · with_stream

```text
┌─ server process ──────────┐    ┌─ client process ──────────────────┐
│  bind(endpoint)           │    │  connect(endpoint) × N clients    │
│  relay/echo               │◄──►│  send → recv (측정)               │
│  server 리소스 보고        │    │  RESULT: throughput, latency,     │
│  READY 프로토콜            │    │         client 리소스              │
└───────────────────────────┘    └───────────────────────────────────┘
```

- **별도 프로세스**: server/client를 독립 프로세스로 spawn
- 스크립트가 양쪽 프로세스를 관리하고 RESULT line을 합산

---

## 5. 결과 파일 형식

### 5.1 파일 구조

perf_multi와 동일한 META → RESULT → TABLE 3-영역 구조를 사용한다.

#### tmp/ · baseline/ (기계 파싱용)

```text
META,os,Linux 6.6.87.2-microsoft-standard-WSL2
META,cpu,AMD Ryzen 9 7950X
META,cores,32
META,build,Release
META,commit,abc1234
META,timestamp,2026-02-24T12:30:00+09:00
META,load_avg,0.52 0.48 0.45
META,suite,with_zmq
META,runs,3
META,clients,100
META,libs,libzmq;zlink
META,status,complete
META,expected,90
META,actual,90
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,throughput,142300.00
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,bandwidth,17.40
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,latency,48.10
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,client_cpu_pct,52.10
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,client_mem_mb,128.40
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,server_cpu_pct,35.10
RESULT,libzmq,MULTI_DEALER_DEALER,tcp,64,server_mem_mb,64.20
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,throughput,150000.00
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,bandwidth,18.31
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,latency,45.23
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,client_cpu_pct,48.20
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,client_mem_mb,112.30
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,server_cpu_pct,33.50
RESULT,zlink,MULTI_DEALER_DEALER,tcp,64,server_mem_mb,58.10
TABLE
## PATTERN: MULTI_DEALER_DEALER (echo)
### Transport: tcp
| Size     | Library |       Throughput | Bandwidth |      Latency | C.CPU% | C.Mem MB | S.CPU% | S.Mem MB | Diff(T) | Diff(L) |
|----------|---------|------------------|-----------|--------------|--------|----------|--------|----------|---------|---------|
| 64B      | libzmq  |   142.30 Kops/s  | 17.4 MB/s |    48.10 us  |  52.1  |  128.4   |  35.1  |   64.2   |         |         |
| 64B      | zlink   |   150.00 Kops/s  | 18.3 MB/s |    45.23 us  |  48.2  |  112.3   |  33.5  |   58.1   |  +5.4%  |  +6.0%  |
```

#### report/ (사람이 읽는 용도)

- **TABLE만** 저장한다. META/RESULT 라인은 포함하지 않는다.

### 5.2 RESULT line 형식

```text
RESULT,<lib>,<pattern>,<transport>,<size>,<metric>,<value>
```

| 필드 | 설명 | 예시 |
|------|------|------|
| `lib` | 라이브러리/스택 식별자 | `zlink`, `libzmq`, `gRPC`, `asio`, `netty` |
| `pattern` | 패턴명 | `MULTI_DEALER_DEALER`, `ROUTER_ECHO`, `STREAM_ECHO` |
| `transport` | transport | `tcp`, `inproc`, `ipc` |
| `size` | 메시지 크기(bytes) | `64`, `1024`, `65536` |
| `metric` | 메트릭명 | 아래 5.3 참조 |
| `value` | 수치 값 (소수점 2자리) | `150000.00` |

### 5.3 메트릭

#### Tier 1: 필수 (완료 판정 대상)

| metric | 단위 | 설명 |
|--------|------|------|
| `throughput` | echo: `ops/s`, one-way: `msg/s` | 측정 구간 수신량 / 측정 시간 |
| `bandwidth` | MB/s | throughput × size [× 2 (echo)] / 1,000,000 |
| `latency` | us | 패턴별 divisor 적용 |

#### Tier 3: 정보성 (완료 판정 제외)

| metric | 출력 프로세스 | 단위 | 비고 |
|--------|-------------|------|------|
| `client_cpu_pct` | client | % | client 프로세스 CPU |
| `client_mem_mb` | client | MB | client 프로세스 RSS |
| `server_cpu_pct` | server | % | server 프로세스 CPU |
| `server_mem_mb` | server | MB | server 프로세스 RSS |

- with_zmq(단일 프로세스)는 `cpu_pct`, `mem_mb` 단일 메트릭을 사용한다.
- with_routing, with_stream(별도 프로세스)는 `client_*`, `server_*`로 분리한다.
- 리소스 메트릭 누락 시 완료 판정에 영향 없음.

### 5.4 META 필수 키

| 키 | 필수 | 설명 |
|----|------|------|
| `os` | MUST | OS 및 커널 버전 |
| `cpu` | MUST | CPU 모델명 |
| `cores` | MUST | 논리 코어 수 |
| `build` | MUST | 빌드 타입 (Release/Debug) |
| `commit` | MUST | git commit SHA |
| `timestamp` | MUST | 실행 시각 (ISO 8601) |
| `load_avg` | SHOULD | 실행 시점 load average |
| `suite` | MUST | bench suite 식별자 (`with_zmq`, `with_routing`, `with_stream`) |
| `runs` | MUST | 반복 횟수 |
| `clients` | MUST | 클라이언트 수 |
| `libs` | MUST | 비교 라이브러리 목록 (`;` 구분) |
| `status` | MUST | `complete` 또는 `partial` |
| `expected` | MUST | 예상 RESULT 라인 수 |
| `actual` | MUST | 실제 RESULT 라인 수 |

---

## 6. 출력 형식

### 6.1 Suite별 비교 테이블

> **구현 필수**: 모든 실행 스크립트는 RESULT line 외에 아래 형식의 비교 테이블을 **반드시 stdout에 출력하고, 결과 파일에도 TABLE 영역으로 기록**해야 한다.

#### with_zmq (단일 프로세스, 2-lib 비교)

```text
## PATTERN: MULTI_DEALER_DEALER (echo)

### Transport: tcp
| Size     | Library |       Throughput | Bandwidth |      Latency | CPU% | Mem MB | Diff(T) | Diff(L) |
|----------|---------|------------------|-----------|--------------|------|--------|---------|---------|
| 64B      | libzmq  |   142.30 Kops/s  | 17.4 MB/s |    48.10 us  | 52.1 |  128.4 |         |         |
| 64B      | zlink   |   150.00 Kops/s  | 18.3 MB/s |    45.23 us  | 48.2 |  112.3 |  +5.4%  |  +6.0%  |
| 1024B    | libzmq  |   115.60 Kops/s  | 225.7 MB/s|    55.40 us  | 54.3 |  135.2 |         |         |
| 1024B    | zlink   |   120.30 Kops/s  | 234.9 MB/s|    52.10 us  | 52.1 |  120.1 |  +4.1%  |  +6.0%  |

### Transport: inproc
| Size     | Library |       Throughput | Bandwidth |      Latency | CPU% | Mem MB | Diff(T) | Diff(L) |
|----------|---------|------------------|-----------|--------------|------|--------|---------|---------|
| 64B      | libzmq  |  5443.23 Kops/s  | 665.4 MB/s|     0.07 us  | 48.5 |   64.2 |         |         |
| 64B      | zlink   |  6087.39 Kops/s  | 744.1 MB/s|     0.06 us  | 45.1 |   58.3 | +11.8%  | +14.3%  |
```

#### with_routing (별도 프로세스, N-lib 비교)

```text
## PATTERN: ROUTER_ECHO (echo)

### 100:1 Echo (clients=100)
| Size     | Library |       Throughput | Bandwidth |      Latency | C.CPU% | C.Mem MB | S.CPU% | S.Mem MB | zlk vs zmq | zlk vs grpc |
|----------|---------|------------------|-----------|--------------|--------|----------|--------|----------|------------|-------------|
| 64B      | libzmq  |    12.34 Kops/s  |  1.5 MB/s |    81.23 us  |  42.5  |   64.2   |  35.1  |   48.3   |            |             |
| 64B      | zlink   |    13.56 Kops/s  |  1.7 MB/s |    73.67 us  |  38.2  |   58.1   |  32.0  |   42.5   |   +9.9%    |   +52.4%    |
| 64B      | gRPC    |     8.90 Kops/s  |  1.1 MB/s |   112.30 us  |  55.3  |  128.4   |  48.2  |   96.5   |            |             |
```

#### with_stream (별도 프로세스, 다수 스택 비교, phase 분리)

```text
## Phase: throughput

### Size: 64B (clients=10000)
| Rank | Stack          |    Throughput |  Bandwidth |      Latency | S.CPU% | S.Mem MB | C.CPU% | C.Mem MB |
|------|----------------|--------------|------------|--------------|--------|----------|--------|----------|
|    1 | asio           | 404.56 Ktps  | 24.9 MB/s  |         N/A  | 293.70 |  1200.77 | 301.86 |  1285.34 |
|    2 | netty          | 401.22 Ktps  | 24.7 MB/s  |         N/A  | 249.15 |  2376.20 | 340.51 |  1288.66 |
|    3 | cppserver      | 397.14 Ktps  | 24.4 MB/s  |         N/A  | 220.43 |  2704.84 | 341.40 |  1285.13 |
|    4 | zmq            | 391.38 Ktps  | 24.1 MB/s  |         N/A  | 263.11 |  2388.64 | 260.98 |  1287.28 |
|    5 | zlink-len32be  | 390.52 Ktps  | 24.0 MB/s  |         N/A  | 279.14 |  1689.39 | 330.54 |  1288.20 |
|    6 | zlink          | 389.56 Ktps  | 23.9 MB/s  |         N/A  | 273.37 |  1676.57 | 322.09 |  1286.12 |
|    7 | dotnet         | 384.06 Ktps  | 23.6 MB/s  |         N/A  | 506.54 |  1061.52 | 332.08 |  1285.04 |

## Phase: latency

### Size: 64B (clients=10000)
| Rank | Stack          |    Throughput |  Bandwidth |    p95(us)   | S.CPU% | S.Mem MB | C.CPU% | C.Mem MB |
|------|----------------|--------------|------------|--------------|--------|----------|--------|----------|
|    1 | asio           | 403.51 Ktps  | 24.8 MB/s  |  25396.36 us | 292.56 |  1199.55 | 301.41 |  1284.99 |
|    2 | zlink-len32be  | 398.15 Ktps  | 24.5 MB/s  |  25931.91 us | 273.89 |  1694.65 | 330.87 |  1287.35 |
|    3 | netty          | 394.04 Ktps  | 24.3 MB/s  |  26146.72 us | 250.95 |  2374.23 | 341.15 |  1287.62 |
```

### 6.2 테이블 컬럼 설명

| 컬럼 | 단위 | 비고 |
|------|------|------|
| Library / Stack | — | 비교 대상 식별자 |
| Rank | — | with_stream 전용, 성능 순위 |
| Throughput | `Kops/s` (echo), `Kmsg/s` (one-way), `Ktps` (stream) | suite별 단위 |
| Bandwidth | `MB/s` | SI 기준 (1 MB = 1,000,000 bytes) |
| Latency | `us` | 마이크로초 (with_stream latency phase는 `p95(us)` 사용) |
| CPU% | `%` | 단일 프로세스 CPU (with_zmq) |
| Mem MB | `MB` | 단일 프로세스 RSS (with_zmq) |
| C.CPU% / C.Mem MB | `%` / `MB` | client 프로세스 (with_routing, with_stream) |
| S.CPU% / S.Mem MB | `%` / `MB` | server 프로세스 (with_routing, with_stream) |
| Diff(T) / Diff(L) | `%` | 비교 차이율 (with_zmq, with_routing) |

### 6.3 Diff 컬럼 규칙

- baseline(libzmq) 행의 Diff 컬럼은 공백으로 둔다.
- **Diff(T)**: `(zlink - baseline) / baseline × 100` — 양수 = zlink이 throughput 높음
- **Diff(L)**: `(baseline - zlink) / baseline × 100` — 양수 = zlink이 latency 낮음 (빠름)
- with_stream은 Diff 대신 **Rank**를 사용하여 전체 순위를 표시한다.

### 6.4 Bandwidth 계산

| 방향 | 계산식 |
|------|--------|
| echo (`ops/s`) | `throughput × msg_size × 2 / 1,000,000` |
| one-way (`msg/s`) | `throughput × msg_size / 1,000,000` |
| stream (`tps`) | `throughput × msg_size / 1,000,000` |

### 6.5 진행 로그

```text
## PATTERN: MULTI_DEALER_DEALER

  > Benchmarking libzmq for MULTI_DEALER_DEALER...
    Testing tcp | 64B,256B,1024B: 1 2 3 Done
    Testing inproc | 64B,256B,1024B: 1 2 3 Done

  > Benchmarking zlink for MULTI_DEALER_DEALER...
    Testing tcp | 64B,256B,1024B: 1 2 3 Done
    Testing inproc | 64B,256B,1024B: 1 2 3 Done
```

### 6.6 실패 요약

```text
## Failures
- MULTI_STREAM libzmq tcp 65536B: timeout
- ROUTER_ECHO gRPC tcp 262144B: no_data
```

---

## 7. 측정 기준

### 7.1 공통

| 항목 | 규칙 |
|------|------|
| 측정 모델 | time-based: duration 구간의 수신량 기반 |
| throughput | `recv_count / duration_seconds` |
| latency | duration phase에서 동시 측정 (suite별 divisor 적용) |
| 대표값 | median (runs > 1) |
| 기본 runs | 3 |

### 7.2 패턴 방향 분류

| 방향 | 단위 | 패턴 |
|------|------|------|
| echo | `ops/s` | MULTI_DEALER_DEALER, MULTI_DEALER_ROUTER, MULTI_ROUTER_ROUTER, ROUTER_ECHO |
| one-way | `msg/s` | MULTI_PUBSUB |
| stream | `tps` | MULTI_STREAM, STREAM_ECHO (with_stream) |

### 7.3 Phase (with_stream)

with_stream은 throughput과 latency를 **별도 phase**로 분리 측정한다.

| Phase | 목적 | latency 측정 |
|-------|------|-------------|
| throughput | 최대 처리량 측정 | 비활성 |
| latency | p95 레이턴시 측정 | 샘플링 활성 (`--latency-sample-rate`) |

### 7.4 비교 공정성 원칙

- **동일 측정 환경**: 동일 머신, 동일 세션에서 모든 비교군을 순차 실행
- **동일 소켓 옵션**: TCP_NODELAY, LINGER, HWM 등 양쪽 동일 설정
- **동일 버퍼 관리**: 사전 할당, 재사용 패턴 동일 적용
- **스택 전환 gap**: with_stream은 스택 전환 시 OS 리소스 정리 대기 (`--stack-gap`)

---

## 8. 실행 스크립트

### 8.1 Suite별 실행

| Suite | 스크립트 | 위치 |
|-------|----------|------|
| with_zmq | `run_benchmarks.sh` | `bindings/c/bench/with_zmq/` |
| with_routing | `run_benchmarks.sh` | `bindings/c/bench/with_routing/` |
| with_stream | `run_benchmarks.sh` | `bindings/c/bench/with_stream/` |

```bash
# with_zmq: 전체 패턴
bindings/c/bench/with_zmq/run_benchmarks.sh --pattern ALL

# with_zmq: 특정 패턴
bindings/c/bench/with_zmq/run_benchmarks.sh --pattern MULTI_DEALER_DEALER,MULTI_STREAM

# with_routing: 기본 실행
bindings/c/bench/with_routing/run_benchmarks.sh --runs 3 --clients 100

# with_routing: zlink만 (나머지 캐시 사용)
bindings/c/bench/with_routing/run_benchmarks.sh --zlink-only

# with_stream: 전체 스택
bindings/c/bench/with_stream/run_benchmarks.sh --stack all --size all --runs 3

# with_stream: 특정 스택/크기
bindings/c/bench/with_stream/run_benchmarks.sh --stack zlink,zmq,asio --size 1024 --runs 3
```

### 8.2 with_zmq 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--pattern NAME` | 측정할 패턴 (쉼표 구분) | 전체 |
| `--libs LIST` | 비교 라이브러리 | `libzmq,zlink` |
| `--runs N` | 반복 횟수 | 3 |
| `--build-dir PATH` | 빌드 디렉터리 | 자동 탐색 |
| `--reuse-build` | 기존 빌드 재사용 | on |
| `--pin-cpu` | CPU pinning | off |
| `--result` | report/ 저장 | off |
| `--save [VER]` | baseline/ 저장 | — |
| `--msg-sizes LIST` | 메시지 크기 | 표준 6종 |
| `--transports LIST` | transport | `tcp,inproc,ipc` |

### 8.3 with_routing 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--runs N` | 반복 횟수 | 3 |
| `--clients N` | 클라이언트 수 | 100 |
| `--build-dir PATH` | 빌드 디렉터리 | 자동 탐색 |
| `--zlink-only` | zlink만 실행, 나머지 캐시 | off |
| `--refresh-cache` | 캐시 갱신 | off |
| `--run-cooldown-ms N` | run 간 대기 | 3000 |

### 8.4 with_stream 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--stack LIST` | 측정 스택 (쉼표 구분 또는 `all`) | all |
| `--size LIST` | 메시지 크기 (쉼표 구분 또는 `all`) | `64,1024,65536` |
| `--phases` | 측정 phase (`both`, `throughput`, `latency`) | both |
| `--ccu N` | 동시 연결 수 | 10000 |
| `--inflight N` | 클라이언트당 in-flight | 10 |
| `--runs N` | 반복 횟수 | 1 |
| `--warmup SEC` | warmup 시간 | 3 |
| `--duration SEC` | 측정 시간 | 5 |
| `--drain-ms N` | drain 대기 | 300 |
| `--latency-sample-rate N` | latency 샘플링 비율 | 100 |
| `--stack-gap SEC` | 스택 전환 대기 | 5 |

---

## 9. 결과 저장

### 9.1 저장 규칙

| 항목 | 규칙 |
|------|------|
| 파일명 형식 | `bench_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt` |
| 날짜 디렉터리 | 사용하지 않음 (파일명에 날짜/시간 포함) |
| 저장 단위 | 스크립트 1회 실행 = 1개 결과 파일 |

### 9.2 저장 옵션

| 동작 | 옵션 | 저장 위치 | 형식 | 조건 |
|------|------|-----------|------|------|
| 임시 저장 | (항상) | `tmp/` | META + RESULT + TABLE | complete/partial 무관 |
| 레포트 | `--result` | `report/` | TABLE만 | complete만 |
| baseline | `--save [VER]` | `baseline/` | META + RESULT + TABLE | complete만 |

### 9.3 보존 정책

| 디렉터리 | 최대 파일 수 | 초과 시 처리 |
|-----------|-------------|-------------|
| `tmp/` | 100 | 파일명 사전순 오래된 것부터 삭제 |
| `report/` | 100 | 파일명 사전순 오래된 것부터 삭제 |
| `baseline/` | 100 | 파일명 사전순 오래된 것부터 삭제 (`latest.txt` 제외) |

### 9.4 완료 판정

```text
expected = 전체 조합 수 (lib × pattern × transport × size × 3메트릭) - unsupported - skip
actual   = 성공적으로 출력된 RESULT 라인 수
status   = (expected == actual) ? "complete" : "partial"
```

---

## 10. 테스트 유효성

### 10.1 결과 상태

| 상태 | 판정 기준 | 집계 |
|------|-----------|------|
| success | exit code 0 + RESULT line 존재 | 유효 |
| unsupported | `UNSUPPORTED,<lib>,<pattern>,<transport>` + exit 0 | 제외 |
| skip | `SKIP,<lib>,<pattern>,<transport>,<reason>` + exit 0 | 제외 |
| fail | exit ≠ 0, timeout, 또는 RESULT 미출력 | 무효 |

### 10.2 종료 코드

| 코드 | 의미 |
|------|------|
| 0 | 성공 (complete) |
| 1 | 실행 오류 (빌드 실패, 바이너리 미존재, partial에서 `--save`) |

---

## 11. 환경 변수

### 11.1 공통

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `BENCH_DEBUG` | 디버그 로그 | unset |
| `BENCH_IO_THREADS` | context I/O threads | 0 (with_routing/with_stream: 4) |
| `BENCH_MSG_SIZES` | 테스트 size 목록 | 표준 6종 |
| `BENCH_FAIL_FAST` | 실패 시 즉시 중단 | 0 |

### 11.2 with_zmq 전용

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `BENCH_MSG_COUNT` | single 메시지 수 | 크기별 자동 |
| `BENCH_LAT_COUNT` | latency 왕복 수 | 500 |
| `BENCH_WARMUP_COUNT` | warmup 메시지 수 | 1000 |
| `BENCH_MULTI_CLIENTS` | 클라이언트 수 | 100 |
| `BENCH_MULTI_INFLIGHT` | in-flight 수 | 30 |
| `BENCH_MULTI_CONNECT_CONCURRENCY` | 동시 연결 수 | 128 |
| `BENCH_MULTI_WARMUP_SECONDS` | warmup 시간 | 3 |
| `BENCH_MULTI_DURATION_SECONDS` | 측정 시간 | 10 |
| `BENCH_MULTI_DRAIN_MS` | drain 대기 | 300 |

### 11.3 with_routing 전용

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `BENCH_PORT` | 서버 포트 | 29200 |
| `BENCH_CLIENTS` | 클라이언트 수 | 100 |
| `BENCH_MULTI_DURATION_SECONDS` | 측정 시간 | 5 |
| `BENCH_MULTI_SETTLE_MS` | 연결 안정화 대기 | 500 |
| `BENCH_MULTI_DRAIN_MS` | drain 대기 | 300 |
| `BENCH_INFLIGHT` | in-flight 수 | 1 |
| `BENCH_HWM` | 소켓 HWM | 300000 |
| `BENCH_MAX_SOCKETS` | 최대 소켓 수 | auto |

### 11.4 with_stream 전용

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `BENCH_MULTI_CLIENTS` | 동시 연결 수 (CCU) | 10000 |
| `BENCH_MULTI_INFLIGHT` | in-flight 수 | 10 |
| `BENCH_MULTI_WARMUP_SECONDS` | warmup 시간 | 3 |
| `BENCH_MULTI_DURATION_SECONDS` | 측정 시간 | 5 |
| `BENCH_MULTI_DRAIN_MS` | drain 대기 | 300 |
| `BENCH_MULTI_SIZE_TRANSITION_DRAIN_MS` | size 전환 drain | drain 값과 동일 |

---

## 12. 구현 제약

### 12.1 측정 경로 lock 사용 금지

| 구분 | 허용 | 금지 |
|------|------|------|
| hot path (send/recv 루프) | `std::atomic`, lock-free | `std::mutex`, `std::condition_variable` |
| cold path (setup/teardown) | 제한 없음 | — |

### 12.2 불필요한 메모리 할당 금지

duration phase에서 벤치마크 인프라의 `malloc`/`new` 호출이 0에 수렴해야 한다. 모든 비교군에 동일하게 적용한다.

### 12.3 비교 공정성

- 성능에 영향을 주는 소켓 옵션(TCP_NODELAY, LINGER, HWM 등)을 모든 비교군에 동일 설정
- 동일 send/recv 패턴(blocking/non-blocking, batch 크기) 적용
- 사전 할당 버퍼 재사용 패턴 동일 적용
