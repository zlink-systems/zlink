# Router Compare Benchmark

zlink, libzmq, gRPC 세 라이브러리의 서버간 echo 통신 성능을 비교하는 벤치마크입니다.

## 테스트 시나리오

| 시나리오 | 설명 |
|----------|------|
| **1:1 Echo** | 클라이언트 1개 ↔ 서버 1개, 단일 연결 성능 측정 |
| **N:1 Echo** | 클라이언트 N개 (기본 100) ↔ 서버 1개, 다중 연결 성능 측정 |

### 라이브러리별 프로토콜

| 라이브러리 | 패턴 | 설명 |
|-----------|------|------|
| **zlink** | ROUTER-ROUTER | zlink ROUTER 소켓 기반 양방향 echo |
| **libzmq** | ROUTER-ROUTER | libzmq ROUTER 소켓 기반 양방향 echo |
| **gRPC** | Bidirectional Streaming | gRPC 양방향 스트리밍 echo |

## zlink 서비스 poller 정책과의 관계

이 벤치는 `gateway`/`receiver`/`spot_*` 같은 서비스 인스턴스가 아니라
직접 소유한 ROUTER 소켓을 비교합니다. 그래서 최근 추가된
service-instance poller API(`zlink_poller_add_gateway`,
`zlink_poller_add_receiver`, `zlink_poller_add_spot_sub`,
`zlink_poller_add_spot_pub`) 적용 대상은 아닙니다.

즉 이 디렉터리의 zlink 벤치는 계속 generic socket benchmark로 보면 됩니다.

### 메시지 크기

기본: `64, 256, 1024, 65536, 131072, 262144` 바이트

### 측정 항목

- **Throughput** (msg/sec): 측정 기간 동안 수신한 총 메시지 수 / 실제 경과 시간
- **Latency** (us): 메시지 payload에 포함된 전송 타임스탬프 기반 평균 RTT

## 디렉토리 구조

```
benchwithroutercompare/
├── CMakeLists.txt                       # 빌드 설정
├── README.md                            # 이 파일
├── run_benchmarks.sh                    # Shell 실행 래퍼
├── run_comparison.py                    # Python 오케스트레이션
├── setup_grpc_dist.sh                   # gRPC 라이브러리 자동 복사 스크립트
├── proto/
│   └── echo.proto                       # gRPC 서비스 정의
├── common/
│   └── bench_router_compare_common.hpp  # 공통 유틸리티
├── zlink/
│   ├── bench_zlink_router_server.cpp    # zlink ROUTER echo 서버
│   └── bench_zlink_router_client.cpp    # zlink ROUTER echo 클라이언트
├── libzmq/
│   ├── bench_zmq_router_server.cpp      # libzmq ROUTER echo 서버
│   ├── bench_zmq_router_client.cpp      # libzmq ROUTER echo 클라이언트
│   └── libzmq_dist/                     # 프리빌드 libzmq
│       └── linux-x64/
│           ├── include/
│           └── lib/
└── grpc/
    ├── bench_grpc_echo_server.cpp       # gRPC echo 서버
    ├── bench_grpc_echo_client.cpp       # gRPC echo 클라이언트
    └── grpc_dist/                       # 프리빌드 gRPC (수동 설정 필요)
        └── linux-x64/
            ├── include/
            ├── lib/
            └── bin/
```

## 사전 준비

### 1. libzmq

`libzmq_dist/linux-x64/`에 이미 포함되어 있어 별도 작업 불필요.

### 2. gRPC (선택사항)

gRPC 벤치를 실행하려면 `grpc_dist/linux-x64/`에 프리빌드 라이브러리가 필요합니다.

```bash
# 시스템에 설치된 gRPC에서 자동 복사
./bindings/c/bench/with_routing/setup_grpc_dist.sh

# 또는 특정 경로 지정
GRPC_PREFIX=/opt/grpc ./bindings/c/bench/with_routing/setup_grpc_dist.sh
```

gRPC가 없으면 gRPC 벤치만 건너뛰고 zlink/libzmq 비교만 실행됩니다.

## 빌드

```bash
cmake -B core/build -DBUILD_BENCHMARKS=ON -DBUILD_SHARED=ON

# 전체 빌드 (gRPC 미설정 시 자동 skip)
cmake --build core/build

# 또는 특정 타겟만 빌드
cmake --build core/build --target bench_rc_zlink_server bench_rc_zlink_client
cmake --build core/build --target bench_rc_zmq_server bench_rc_zmq_client
# gRPC가 설정된 경우에만:
cmake --build core/build --target bench_rc_grpc_server bench_rc_grpc_client
```

gRPC가 설정되지 않은 경우 `bench_rc_grpc_*` 타겟은 등록되지 않습니다.
전체 빌드 시에는 자동으로 건너뛰지만, 해당 타겟을 명시적으로 지정하면 빌드가 실패합니다.

## 실행

### Shell 래퍼 (권장)

```bash
# 기본 실행 (3회 반복, 100 클라이언트)
./bindings/c/bench/with_routing/run_benchmarks.sh

# 빠른 테스트 (1회 반복)
./bindings/c/bench/with_routing/run_benchmarks.sh --runs 1

# 클라이언트 수 지정
./bindings/c/bench/with_routing/run_benchmarks.sh --runs 3 --clients 50

# zlink만 실행 (zmq/grpc는 캐시 사용)
./bindings/c/bench/with_routing/run_benchmarks.sh --zlink-only

# 캐시 갱신
./bindings/c/bench/with_routing/run_benchmarks.sh --refresh-cache

# 빌드 디렉토리 지정
./bindings/c/bench/with_routing/run_benchmarks.sh --build-dir core/build
```

### Python 직접 실행

```bash
python3 bindings/c/bench/with_routing/run_comparison.py \
  --runs 3 --clients 100
```

### 개별 바이너리 수동 실행

수동 실행 시 libzmq/gRPC 바이너리는 `LD_LIBRARY_PATH` 설정이 필요합니다.
(`run_comparison.py`는 자동으로 설정합니다.)

```bash
# zlink (빌드 디렉토리의 libzlink 사용)
LD_LIBRARY_PATH=core/build/lib \
  BENCH_PORT=29200 ./core/build/bin/bench_rc_zlink_server &
LD_LIBRARY_PATH=core/build/lib \
  BENCH_PORT=29200 BENCH_CLIENTS=1 BENCH_MULTI_DURATION_SECONDS=5 \
  ./core/build/bin/bench_rc_zlink_client zlink

# libzmq
LIBZMQ_LIB=bindings/c/bench/with_routing/libzmq/libzmq_dist/linux-x64/lib
LD_LIBRARY_PATH=${LIBZMQ_LIB} \
  BENCH_PORT=29200 ./core/build/bin/bench_rc_zmq_server &
LD_LIBRARY_PATH=${LIBZMQ_LIB} \
  BENCH_PORT=29200 BENCH_CLIENTS=1 BENCH_MULTI_DURATION_SECONDS=5 \
  ./core/build/bin/bench_rc_zmq_client libzmq
```

## 옵션

### Shell / Python 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `--runs N` | 3 | 설정당 반복 횟수 (중앙값 선택) |
| `--clients N` | 100 | N:1 테스트 클라이언트 수 |
| `--build-dir PATH` | 자동 탐색 | 빌드 디렉토리 경로 |
| `--zlink-only` | off | zlink만 실행, zmq/grpc는 캐시 사용 |
| `--refresh-cache` | off | 베이스라인 캐시 갱신 |
| `--run-cooldown-ms N` | 3000 | 실행 간 대기 시간 (ms) |

### 환경변수

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `BENCH_PORT` | 29200 | 서버 바인드 포트 |
| `BENCH_CLIENTS` | 100 | 클라이언트 수 |
| `BENCH_MSG_SIZES` | `64,256,1024,65536,131072,262144` | 메시지 크기 리스트 (콤마 구분) |
| `BENCH_MULTI_DURATION_SECONDS` | 5 | 측정 기간 (초) |
| `BENCH_MULTI_SETTLE_MS` | 500 | 연결 안정화 대기 (ms) |
| `BENCH_MULTI_DRAIN_MS` | 300 | 잔여 메시지 수신 대기 (ms) |
| `BENCH_INFLIGHT` | 1 | 클라이언트당 동시 전송 메시지 수 |
| `BENCH_IO_THREADS` | 4 | zmq/zlink IO 스레드 수 |
| `BENCH_HWM` | 300000 | zmq/zlink 고수위선 (HWM) |
| `BENCH_MAX_SOCKETS` | max(2048, clients+1024) | zmq/zlink 컨텍스트 최대 소켓 수 |

## 출력 형식

### CSV (바이너리 stdout)

```
RESULT,zlink,ROUTER_ECHO,tcp,64,throughput,123456.78
RESULT,zlink,ROUTER_ECHO,tcp,64,latency,45.67
```

### 마크다운 테이블 (run_comparison.py)

```
### 1:1 Echo (clients=1)
| Size   | Metric     | libzmq        | zlink         | gRPC          | zlk vs zmq | zlk vs grpc |
|--------|------------|---------------|---------------|---------------|------------|-------------|
| 64B    | Throughput | 12.34 Kmsg/s  | 13.56 Kmsg/s  |  8.90 Kmsg/s  |   +9.88%   |  +52.36%    |
| 64B    | Latency    |   81.23 us    |   73.67 us    |  112.30 us    |   +9.31%   |  +34.41%    |

### 100:1 Echo (clients=100)
| Size   | Metric     | libzmq        | zlink         | gRPC          | zlk vs zmq | zlk vs grpc |
...
```

## 캐시 시스템

libzmq/gRPC 결과는 `rc_cache_<platform>-<arch>.json`에 캐시됩니다.
`--zlink-only` 옵션 사용 시 이전 캐시된 zmq/grpc 결과와 비교하여 zlink만 빠르게 반복 테스트할 수 있습니다.
캐시가 없으면 zmq/grpc 결과는 N/A로 표시됩니다. 최초 실행 시에는 `--zlink-only` 없이 전체 실행하여 캐시를 생성하세요.

## 아키텍처 차이점

| 항목 | zmq / zlink | gRPC |
|------|-------------|------|
| IO 모델 | 단일 스레드 이벤트 루프, N개 소켓 순회 | N개 스레드, 각 스레드 독립 스트림 |
| 프로토콜 | 자체 프레이밍 (ZMTP) | HTTP/2 |
| 연결 수 | 클라이언트당 1 TCP 연결 | 클라이언트당 1 HTTP/2 채널 |
| inflight>1 | 배치 send → 배치 recv | 순차 Write → Read |
