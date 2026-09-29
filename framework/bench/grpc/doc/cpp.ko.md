# C++ messaging bench

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 C++에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 C++ 고유의 값만 담는다.

## 1. 비교 대상

| 구현 | request | send/command | 사용 모듈 |
|---|---|---|---|
| `grpc-cpp` | `PrepareAsyncEcho` + `CompletionQueue` (unary) | `PrepareAsyncCommand`, `Empty` reply | vcpkg `gRPC::grpc++` |
| `zlink-cpp` | raw ROUTER `request(peer).message(...).async()` | command 전용 raw ROUTER `send(peer).message(...).async()` | packaged `zlink::cpp` |
| `zlink-framework-cpp` | `route_client_t::request_to_channel(...).async<BenchPayload>()` | `route_client_t::send_to_channel(...).async()` | 저장소 `zlink::framework`, `zlink::framework_codec_protobuf` |

모든 행은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
envelope와 protobuf body 두 part를 보내며 request와 command endpoint를 분리한다. framework 행은
RouteMesh용 `route_client_t`를 쓴다(`channel_client_t`는 ClientServer 채널용이다).

## 2. 실행 방법

입력은 규격 [§11](../README.ko.md#11-runner-입력과-결과-배치)의 환경 변수가 전부다. CLI 옵션과 위치 인자는 없다. runner는 `SKIP_BUILD=1`이 아니면 먼저 빌드한다. 빌드는 `VCPKG_ROOT`가 가리키는 vcpkg로 C++
Framework의 manifest(`framework/languages/cpp/vcpkg.json`)를 `bench` feature와 함께 받는다. gRPC와
protobuf도 이 manifest에서 오므로 한 process가 protobuf 하나만 쓴다. ZLink binding과 Core는 local
package root(기본 `.artifacts/wsl`)의 `install/zlink-cpp/<binding 버전>`과
`install/zlink-core/<Core 버전>`을 쓴다. 버전은 저장소의 `bindings/cpp/VERSION`과 `VERSION`이 정한다.
측정은 항상 perf 티켓 큐로 낸다.

```bash
# 전체 matrix — 항상 perf 티켓 큐로
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "cpp with-grpc" -- \
  bash framework/bench/grpc/cpp/run_local.sh

# 한 셀
IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 DURATION_SECONDS=2 \
  bash framework/bench/grpc/cpp/run_local.sh
```

warmup(5초, 10구간), request timeout·drain 상한·settle quiet 구간과 load gate(측정 시작 load average
2.0 미만)는 runner가 정하는 고정값이다.

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
| gRPC / protobuf | C++ Framework vcpkg manifest의 `bench` feature(버전은 원본에 기록) |
| ZLink binding / Core | local package(버전은 원본에 기록) |
| framework | 저장소 소스(public CMake target; 측정한 커밋을 원본에 기록) |
| source 포화 지표 | `submit_thread_cores`(`CLOCK_THREAD_CPUTIME_ID`); 상한 1 |
| latency 표본 상한 | A 200,000개, B 2,000,000개 |

## 5. 결과 위치

결과는 규격 §11의 배치(`<OUTPUT_DIR>/run<N>/<implementation>-<pattern>-<payload>/results.json`)로
쓴다. 셀 안에는 diagnostics인 `source.log`, `target.log`, `target-stats.json`와 `warmup-target-stats.json`도 남는다.
표와 판정은 집계기만 만든다. 전 언어를 한 번에 재고 보고서까지 만들 때는 `run_all.sh`를 쓴다(§11).
한 언어만 집계할 때는 다음처럼 C 결과와 함께 넘긴다.

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang cpp \
  --runs-glob 'framework/bench/grpc/log/<stamp>/c/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/cpp/run*' --format full
```

## 6. 알려진 제약

- `zlink-framework-cpp`는 request-serial에서 raw의 약 7%의 직렬화 비용이
  보인다(결정 기록 FB-052). 벤치 결함이 아니라 framework C++ runtime의 특성으로 다루며 3-run
  값으로 판정한다.
- `zlink-framework-cpp send-saturation`: warmup flood 뒤 RouteMesh send target이 사라져 active
  send가 전부 실패한다(결정 기록 FB-054, FB-012 계열). 결함 수정 전에는 그 행을 오류 셀로만
  기록하고 게재하지 않는다.
- framework HTTP host는 listener bind 실패를 start 결과로 돌려주지 않는다(FB-053). runner는 HTTP
  응답 readiness로 listener 시작을 확인한다.
- HTTP host의 여러 listen 포트에 같은 route가 노출된다. trigger URL은 규격의 trigger 포트만
  기록하고 runner도 그 URL만 쓴다.
- raw 비교는 ROUTER↔ROUTER만 허용한다. `request-backpressure`는 application in-flight 상한이
  없으므로 도달 깊이가 결과다.
