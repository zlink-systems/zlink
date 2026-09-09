# C++ messaging bench

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 C++에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 C++ 고유의 값만 담는다.

## 1. 비교 대상

| 구현 | request | send/command | 사용 모듈 |
|---|---|---|---|
| `grpc-cpp` | `PrepareAsyncEcho` + `CompletionQueue` (unary) | `PrepareAsyncCommand`, `Empty` reply | 시스템 `libgrpc++` |
| `zlink-cpp` | raw ROUTER `request(peer).message(...).async()` | command 전용 raw ROUTER `send(peer).message(...).async()` | packaged `zlink::cpp` |
| `zlink-framework-cpp` | `route_client_t::request_to_channel(...).async<BenchPayload>()` | `route_client_t::send_to_channel(...).async()` | 저장소 `zlink::framework`, `zlink::framework_codec_protobuf` |

모든 행은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
envelope와 protobuf body 두 part를 보내며 request와 command endpoint를 분리한다. framework 행은
RouteMesh용 `route_client_t`를 쓴다(`channel_client_t`는 ClientServer 채널용이다).

## 2. 실행 방법

runner는 빌드하지 않는다. 먼저 빌드하고, 측정은 항상 perf 티켓 큐로 낸다.

```bash
# 빌드 — local package root(기본 .artifacts/wsl)에 install/zlink-cpp/0.17.6 과
# install/zlink-core/0.17.5 (릴리스 Core prefix로의 symlink여도 됨)가 있어야 한다.
cmake -S framework/bench/grpc/cpp -B framework/bench/grpc/cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build framework/bench/grpc/cpp/build --parallel 2

# 전체 matrix 1 run — 항상 perf 티켓 큐로
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "cpp with-grpc run 1/3" -- \
  bash framework/bench/grpc/cpp/run_local.sh cpp-s3-run1

# 한 셀
bash framework/bench/grpc/cpp/run_local.sh smoke --implementations zlink-framework-cpp \
  --patterns request-window --payload-sizes 1024 --duration-seconds 2
```

| 입력 | 기본값 | 의미 |
|---|---|---|
| 첫 인자 | `cpp-router-1` | run label(결과 디렉터리 이름) |
| `BUILD_DIR` | `cpp/build` | 사전에 빌드한 실행 파일 위치 |
| `--implementations` / `IMPLEMENTATIONS` | 세 구현 | 구현 목록 |
| `--patterns` / `PATTERNS` | 네 패턴 | 패턴 목록 |
| `--payload-sizes` / `PAYLOAD_SIZES` | `1024,4096` | body 크기(다른 값은 거부) |
| `--duration-seconds` / `DURATION_SECONDS` | 5 | active 초 |
| `--warmup` / `WARMUP_SECONDS` | 5 | warmup 초 |
| `--warmup-segments` / `WARMUP_SEGMENTS` | 10 | warmup throughput 관측 구간 수 |
| `REQUEST_WINDOW`, `SEND_CONCURRENCY` | 100, 8 | 다른 값은 거부 |
| `REQUEST_TIMEOUT_MS` / `DRAIN_BOUND_MS` | 30000 / 30000 | request timeout / drain·settle 상한 |
| `COMMAND_SETTLE_MS` | 200 | 수신·완료 count 안정 확인 구간 |
| `OUTPUT_DIR` / `--output-dir` | `log/cpp/<stamp>/<label>` | 결과 디렉터리 |
| `RUN_STAMP` | 현재 시각 | run 묶음 ID |
| `LOAD_GATE` | 2.0 | 측정 시작 load average 기준 |

## 3. 프로세스 구성

| 셀 | source A | A 포트(trigger/stats) | target B | B 포트 |
|---|---|---|---|---|
| `grpc-cpp` | `bench_cpp_client` — async unary stub | 5280/5281 | `bench_cpp_grpc_server` — synchronous `ServerBuilder` | gRPC 5282, stats 5283 |
| `zlink-cpp` | `bench_cpp_client` — ROUTER + coroutine/poller | 5285/5286 | `bench_cpp_zlink_server` — request·command ROUTER 분리 | request 5287, command 5288, stats 5289 |
| `zlink-framework-cpp` | `bench_cpp_client` — `route_client_t` | 5292/5293 | `bench_cpp_framework_server` — RouteMesh typed echo/count handler | RouteMesh 5294, stats 5295 |

trigger·stats·phase 규칙은 `common/bench_stats_server.hpp`(Framework public HTTP hosting) 한 곳이
소유한다. trigger client는 runner의 `curl`이다. 셀 순서는 규격 §10.4이고 셀마다 새 A/B process
쌍을 쓴다. A는 HTTP listener 두 개가 실제로 응답한 뒤 outbound transport를 시작한다(이 머신의
`ip_local_port_range=1024 65535`에서 outbound socket이 고정 HTTP 포트를 선점하는 문제를 막기
위해서다). framework A는 HTTP용 app과 channel용 app을 한 process에 두고 공통 lifecycle이 순차로
시작한다. Object role은 A/B 모두 `None`, 고정 RID와 manual peer 연결이다. runner는 시작 전과
셀 종료 뒤 5280-5299의 LISTEN 소켓을 확인한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | application thread 하나의 순차 submit/completion |
| `request-window` | 1 | 100 | gRPC async call / raw coroutine / framework task의 미완료 집합 100 |
| `request-backpressure` | 1 | 없음 | 제출마다 completion pump에 기회를 주며 상한 없이 제출 |
| `send-saturation` | 8 | 1 | gRPC는 stream당 stub, raw는 coroutine slot, framework는 task slot |

세 driver 모두 submit/completion 관측은 application thread 하나에서 한다. framework task의 완료는
public `await_ready()` polling으로 관측하며 그 CPU는 `submit_thread_cores`에 포함한다. transport
completion을 직접 drain하거나 두 번째 poller를 두지 않는다.

## 4. 언어별로 다르게 둔 값

| 항목 | 값 |
|---|---|
| warmup | 5초, 10구간. trigger 원본의 `warmup`은 ms 단위(`5000`) |
| gRPC source | channel 하나, logical stream당 unary stub 하나, application thread의 `CompletionQueue` |
| gRPC target | synchronous `ServerBuilder` 기본값, insecure loopback |
| compiler | GNU C++ 13.3.0, C++20, Release `-O3` |
| gRPC / protobuf | 시스템 1.51.1 / 3.21.12 |
| ZLink binding / Core | local package 0.17.6 / release 0.17.5 |
| framework | 저장소 소스(public CMake target; 측정한 커밋을 원본에 기록) |
| source 포화 지표 | `submit_thread_cores`(`CLOCK_THREAD_CPUTIME_ID`); 상한 1 |
| latency 표본 상한 | A 200,000개, B 2,000,000개 |

## 5. 결과 위치

```text
framework/bench/grpc/log/cpp/<stamp>/<label>/
├── runner.log · report.txt · load-gates.txt
└── <implementation>-<pattern>-<payload>/
    ├── results.json            # with-grpc-cell-v1: role·trigger·streams·target_stats
    ├── source.log / target.log
    ├── warmup-target-stats.json
    └── target-stats.json       # { "snapshot": <B stats 응답> }
```

두 stats 파일은 진단 snapshot이라 `snapshot` container로 감싼다(root에 `role=target`을 두면
집계기가 독립 셀로 오인한다). `results.json`의 `completed_at_close`·`server_received_at_close`는
A가 active 경계에서 본 값이고, `completed`와 `target_stats.received`는 settle 뒤 최종값이다.
표의 send `KMSG/s`는 규격대로 settle 뒤 B의 active-header 수신 수 / `durationMs`이며, drain이
active의 10%를 넘는 행은 `drain ms`와 소비율을 함께 읽는다(결정 기록 FB-051).

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang cpp --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' --format full
```

## 6. 알려진 제약

- `zlink-framework-cpp`는 request-serial에서 raw의 약 7%, window에서 요청당 ~1 ms의 직렬화가
  보인다(결정 기록 FB-052). 벤치 결함이 아니라 framework C++ runtime의 특성으로 다루며 3-run
  값으로 판정한다.
- framework HTTP host는 listener bind 실패를 start 결과로 돌려주지 않는다(FB-053). runner는 HTTP
  응답 readiness로 listener 시작을 확인한다.
- HTTP host의 여러 listen 포트에 같은 route가 노출된다. trigger URL은 규격의 trigger 포트만
  기록하고 runner도 그 URL만 쓴다.
- raw 비교는 ROUTER↔ROUTER만 허용한다. `request-backpressure`는 application in-flight 상한이
  없으므로 도달 깊이가 결과다.
