# C++ Framework Performance Plan

> 공통 정책: [ZLink Framework Performance Policy](../README.ko.md)
>
> 적용 범위: `framework/languages/cpp`

## 1. 목적

C++ framework perf는 `.NET` framework와 같은 사용자 흐름을 C++20 API로 제공할 때,
framework 계층이 만드는 비용을 분리해 측정한다. C++은 현재 framework 구현 검증이 더
활발하므로 공통 scenario 외에 micro benchmark와 fake backend benchmark를 추가로 둔다.

## 2. 측정 계층

| `measurement_layer` | 목적 | Runner 포함 |
|---------------------|------|-------------|
| `framework_micro` | serializer, envelope, handler registry, DI scope 같은 내부 framework 비용 확인 | C++ 전용 |
| `framework_fake_backend` | transport 없이 framework runtime path 비용 확인 | C++ 전용 우선 |
| `real_transport_e2e` | 실제 zlink socket 기반 사용자 체감 성능 확인 | 공통 |
| `sample_scenario_smoke` | Bingo/TicTacToe 구조가 비정상적으로 느리지 않은지 확인 | C++ 전용 smoke |

fake backend는 transport만 대체한다. public fluent builder, handler registration, runtime
dispatch, serializer, monitoring path는 실제 framework 경로를 통과해야 한다.

## 3. C++ Scenario

공통 scenario는 모두 지원 대상으로 둔다. 초기 smoke는 4KB payload와 대표 happy path로
시작한다.

| Scenario | Smoke | Full |
|----------|-------|------|
| `client_server_send` | 필요 | 필요 |
| `client_server_request_reply` | 필요 | 필요 |
| `fanout_publish_1` | 필요 | 필요 |
| `fanout_publish_n` | 선택 | 필요 |
| `dealer_mesh_request_reply` | 필요 | 필요 |
| `route_mesh_send` | 필요 | 필요 |
| `route_mesh_request_reply` | 필요 | 필요 |
| `stream_send` | 필요 | 필요 |
| `stream_request_reply` | 필요 | 필요 |
| `bound_session_send` | 필요 | 필요 |
| `stream_actor_relay` | 필요 | 필요 |
| `spot_to_spot_send` | 필요 | 필요 |
| `spot_to_spot_request_reply` | 필요 | 필요 |
| `spot_to_router_egress` | 필요 | 필요 |
| `router_to_spot_ingress` | 필요 | 필요 |
| `http_handler_roundtrip` | 필요 | 필요 |

`spot_actor_dispatch`처럼 C++ 내부 병목을 보기 위한 항목은 공통 scenario가 아니라
C++ extension scenario로 둔다. Extension scenario는 공통 비교표에 섞지 않고
`measurement_layer`를 `framework_micro` 또는 `framework_fake_backend`로 기록한다.

## 4. Embedded HTTP Server Gate

C++ framework는 내장 HTTP server를 제공하므로 `http_handler_roundtrip` 외에 HTTP server
자체 성능 gate를 둔다. 이 gate는 아래 spec의 성능 기준을 따른다.

- [C++ embedded HTTP server spec](../../common/spec/server/languages/cpp/61-embedded-http-server.ko.md)

HTTP handler e2e와 HTTP server perf는 목적이 다르다.

- HTTP handler e2e는 public consumer 검증이므로 `zlink::http_client`로 호출한다.
- HTTP server perf는 client 구현 비용을 빼야 하므로 같은 load generator로 zlink, Drogon,
  Oat++ server를 모두 호출한다.
- load generator는 runner가 하나로 고정하고, report에 도구 이름, version, command line,
  thread 수, connection 수, duration, warmup을 남긴다.

HTTP server perf scenario:

| Scenario | Payload | 비교 기준 |
|----------|---------|-----------|
| `http_server_empty_route` | 0B | Drogon/Oat++ 동등 route 중 더 빠른 baseline |
| `http_server_json_4kb` | 4KB JSON | Drogon/Oat++ 동등 route 중 더 빠른 baseline |
| `https_server_json_4kb` | 4KB JSON | 같은 TLS 조건의 Drogon/Oat++ baseline |
| `http_server_keep_alive` | 1KB JSON | 같은 keep-alive profile의 Drogon/Oat++ baseline |

처리량 기준은 plain HTTP route와 JSON route에서 baseline 대비 10% 이내 하락이다.
HTTPS JSON route는 같은 TLS 조건에서 baseline 대비 15% 이내 하락이어야 한다.
p95 latency가 baseline 대비 15%를 초과해 악화되면 실패로 본다.

이 gate의 CTest label은 `framework-http-perf`다. 긴 full matrix는 기본 regression에 넣지 않고,
짧은 smoke와 baseline 비교 가능 여부를 먼저 확인한다. 최종 완료 판단에는 complete report가
필요하며, interrupted 또는 partial report는 성공으로 처리하지 않는다.
같은 report 안에서 zlink, Drogon, Oat++에 서로 다른 load generator를 섞은 결과도 성공으로
처리하지 않는다.

## 5. C++ Micro Benchmark

micro benchmark는 최종 언어 간 비교표에 섞지 않는다. C++ framework 내부 병목을 찾는
진단용이다.

| Benchmark | 측정 대상 |
|-----------|-----------|
| `envelope_encode_decode` | envelope header/body encode, decode |
| `serializer_json_roundtrip` | registered serializer lookup과 JSON roundtrip |
| `handler_registry_dispatch` | typed handler lookup과 invocation |
| `di_scope_resolve` | scoped dependency resolve 비용 |
| `call_object_submit` | fluent call object 구성과 submit 비용 |
| `monitoring_publish_filter` | typed monitoring event filter와 handler dispatch |
| `http_route_match_validation` | HTTP method/path matching과 request validation |
| `http_response_write_empty` | empty response write와 header formatting |
| `http_json_binding_4kb` | 4KB JSON DTO parse와 serialize |

micro benchmark도 public 또는 contract-level API를 우선 사용한다. runtime detail을 직접
호출해야 할 때는 C++ 전용 진단으로 표시하고 공통 report와 분리한다.

## 6. Payload Size

공통 payload size를 그대로 사용한다.

- 64B
- 1KB
- 4KB
- 64KB

초기 구현은 `4KB` smoke만 허용한다. full matrix 전에는 반드시 4개 size를 모두 추가한다.

## 7. Runner 위치

권장 위치:

```text
framework/languages/cpp/perf/run_benchmarks.sh
framework/languages/cpp/perf/src/
framework/languages/cpp/perf/results/
```

CMake target은 `framework/languages/cpp/CMakeLists.txt`에서 관리한다. CTest에는 긴 full
matrix를 넣지 않고 짧은 smoke만 label로 연결한다.

권장 CTest label:

```text
framework-perf
framework-perf-smoke
framework-perf-cpp
framework-http-perf
```

## 8. Artifact 검증

runner는 최소한 아래를 확인한다.

- `framework/languages/cpp/build`가 존재한다.
- benchmark binary가 source보다 오래되지 않았다.
- real transport benchmark는 현재 core runtime과 링크된 framework 산출물을 사용한다.
- stale artifact가 의심되면 실패하고 재빌드를 요구한다.

## 9. Report Metadata

C++ report는 공통 schema 외에 아래 metadata를 추가할 수 있다.

```json
{
  "cpp_standard": "c++20",
  "build_type": "Release",
  "measurement_layer": "framework_fake_backend",
  "backend": "fake",
  "compiler": "gcc",
  "compiler_version": "..."
}
```

`measurement_layer` 값은 공통 정책의 값을 사용한다. C++ extension scenario의 경우에도
`framework_micro`, `framework_fake_backend`, `real_transport_e2e`,
`sample_scenario_smoke` 중 하나를 사용한다. `backend` 값은 실행 backend를 나타내며
`zlink`, `fake`, `in_memory`, `none` 중 하나를 우선 사용한다.

HTTP server perf gate report는 아래 field를 추가한다.

```json
{
  "baseline_framework": "drogon",
  "baseline_version": "unknown",
  "baseline_commit": "unknown",
  "load_generator": "unknown",
  "connection_count": 0,
  "thread_count": 0,
  "tls": false
}
```

## 10. 금지 사항

- benchmark 전용 public shortcut API를 만들지 않는다.
- fake backend benchmark에서 framework dispatch path를 우회하지 않는다.
- C API를 hot path에서 직접 호출해 framework 수치처럼 보고하지 않는다.
- interrupted result를 C++ framework perf 상태로 기록하지 않는다.
- sample 성공 여부만으로 framework 성능이 충분하다고 판단하지 않는다.
- HTTP server perf gate를 `zlink::http_client` 수치만으로 판단하지 않는다.

## 11. 초기 구현 순서

1. `4KB` smoke runner와 JSON report writer를 만든다.
2. fake backend로 channel request/reply, route request/reply, stream send, spot actor
   dispatch를 먼저 연결한다.
3. HTTP handler roundtrip은 `zlink::http_client`를 사용한다.
4. micro benchmark를 추가해 framework 내부 비용을 분리한다.
5. embedded HTTP server perf smoke와 Drogon/Oat++ baseline fixture를 추가한다.
6. payload size 4종과 full matrix를 추가한다.
7. 동시성 프로파일 `serial`, `pipelined`, `concurrent`를 추가한다.
8. Java, .NET, Node report와 같은 schema로 비교표를 생성한다.
