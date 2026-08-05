# ZLink Framework Performance Policy

이 문서는 ZLink framework 계층의 성능 측정 기준을 정리한다.
대상은 C++, Java, .NET, Node framework 구현이다.

기존 `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
`doc/perf/PERF_MULTI_TEST_POLICY.md`는 core와 bindings 성능 측정 기준이다.
이 문서는 그 기준을 대체하지 않는다. Framework 성능 측정은 core와 bindings
위에서 사용자가 실제로 호출하는 framework API, 실행 흐름, 샘플 구성을 측정한다.

## 목적

Framework 성능 측정의 목적은 언어별 framework가 같은 기능을 비슷한 의미로
제공하는지 비교할 수 있게 만드는 것이다. 단순히 빠른 숫자를 얻는 것이 아니라,
다음 세 가지를 구분해서 볼 수 있어야 한다.

- core와 binding transport 자체의 비용
- framework가 추가하는 dispatch, serialization, handler, DI, monitoring 비용
- 언어 runtime이 만드는 scheduling, JIT, GC, event loop 비용

따라서 framework 성능 측정은 반드시 어떤 계층을 측정했는지 보고서에 남긴다.

## 대상

언어별 세부 정책은 아래 문서에 둔다.

- [C++ framework perf](bindings/cpp-framework-performance.ko.md)
- [Java framework perf](bindings/java-framework-performance.ko.md)
- [.NET framework perf](bindings/dotnet-framework-performance.ko.md)
- [Node framework perf](bindings/node-framework-performance.ko.md)

## 공통 원칙

Framework perf는 사용자가 호출하는 공개 framework API를 기준으로 측정한다.
Benchmark만을 위해 private backend, native handle, 내부 queue에 직접 접근하면
framework 비용이 빠진 숫자가 되므로 정식 성능 근거로 사용하지 않는다.

측정 전에 smoke benchmark를 먼저 통과해야 한다. Full matrix는 시간이 오래 걸릴
수 있으므로 명시적으로 켜는 방식으로 둔다. Smoke 결과는 runner와 시나리오 배선이
깨지지 않았는지 확인하기 위한 최소 검증이고, 최종 성능 판단은 full matrix 또는
완료된 대표 benchmark 보고서를 기준으로 한다.

빌드 산출물과 runtime은 오래된 상태로 실행하면 안 된다. Runner는 실제 사용한
library, executable, package, runtime 경로와 버전을 출력해야 한다. 소스보다
오래된 산출물을 감지할 수 있는 언어에서는 즉시 실패해야 한다.

중단된 benchmark나 일부 시나리오만 실행된 보고서는 최종 성능 근거가 아니다.
보고서에는 `complete`, `partial`, `failed`, `unsupported` 상태를 명확히 남긴다.
`partial`은 결과 파일을 남길 수는 있지만 perf gate에서는 성공으로 처리하지 않는다.

## 측정 계층

Framework perf는 아래 계층을 구분한다.

| 계층 | 의미 | 용도 |
|------|------|------|
| core/binding baseline | 기존 core와 binding runner가 측정하는 transport 비용 | framework 결과의 참고 기준 |
| framework micro | handler dispatch, serializer, registry, builder 같은 작은 단위 비용 | framework 내부 hot path 확인 |
| framework fake backend | transport를 가짜 backend로 바꾸고 framework 경로만 측정 | framework overhead 분리 |
| real transport e2e | 실제 ZLink transport를 사용한 end-to-end 측정 | 사용자 체감 성능 판단 |
| sample scenario smoke | 샘플을 통해 대표 사용 흐름을 짧게 검증 | 회귀와 배선 오류 확인 |

언어별 구현 상태가 다를 수 있으므로 모든 계층을 한 번에 강제하지 않는다.
다만 보고서에는 어떤 계층을 측정했는지 반드시 기록한다.

`measurement_layer`와 `backend`는 서로 다른 의미다. `measurement_layer`는 어느
계층의 비용을 측정했는지를 나타내고, `backend`는 그 계층을 실행할 때 실제로 사용한
backend 종류를 나타낸다.

권장 `measurement_layer` 값은 다음과 같다.

- `core_binding_baseline`
- `framework_micro`
- `framework_fake_backend`
- `real_transport_e2e`
- `sample_scenario_smoke`

권장 `backend` 값은 다음과 같다.

- `zlink`: 실제 ZLink transport를 사용한다.
- `fake`: framework 테스트용 fake backend를 사용한다.
- `in_memory`: process 내부 memory backend를 사용한다.
- `none`: micro benchmark처럼 backend가 없다.

## 공통 시나리오 매트릭스

아래 시나리오는 언어별 framework가 가능한 한 같은 이름과 같은 의미로 제공해야 한다.
아직 구현되지 않은 시나리오는 성공으로 처리하지 말고 `unsupported`로 보고한다.

| 시나리오 | 측정 내용 |
|----------|-----------|
| `client_server_send` | client에서 server로 단방향 메시지를 보낸다 |
| `client_server_request_reply` | client가 요청을 보내고 server 응답을 받는다 |
| `fanout_publish_1` | subscriber 1개에 publish한다 |
| `fanout_publish_n` | subscriber 여러 개에 publish한다 |
| `dealer_mesh_request_reply` | dealer mesh에서 request/reply를 측정한다 |
| `route_mesh_send` | route mesh에서 단방향 route 전송을 측정한다 |
| `route_mesh_request_reply` | route mesh에서 request/reply를 측정한다 |
| `stream_send` | stream 경로로 단방향 메시지를 보낸다 |
| `stream_request_reply` | stream 경로에서 request/reply를 측정한다 |
| `bound_session_send` | bound session으로 단방향 메시지를 보낸다 |
| `stream_actor_relay` | stream actor relay 경로의 처리량과 지연 시간을 측정한다 |
| `spot_to_spot_send` | spot에서 다른 spot으로 단방향 메시지를 보낸다 |
| `spot_to_spot_request_reply` | spot 간 request/reply를 측정한다 |
| `spot_to_router_egress` | spot에서 router/channel 방향으로 나가는 경로를 측정한다 |
| `router_to_spot_ingress` | router/channel에서 spot으로 들어오는 경로를 측정한다 |
| `http_handler_roundtrip` | framework HTTP client로 HTTP handler를 호출한다 |

## Payload 크기

Payload 크기는 성능 병목을 다르게 드러낸다. 4KB 하나만 보면 중간 크기 메시지의
상태는 알 수 있지만, framework dispatch 비용과 큰 메시지 copy 비용은 놓치기 쉽다.
공통 full matrix는 아래 크기를 사용한다.

- `64B`: framework 호출, dispatch, scheduling 비용을 보기 위한 작은 메시지
- `1KB`: 일반 command나 작은 JSON payload에 가까운 메시지
- `4KB`: 대표 중간 크기 메시지
- `64KB`: serialization, copy, backpressure 영향을 보기 위한 큰 메시지

초기 smoke benchmark는 모든 시나리오를 빠르게 배선하기 위해 `4KB`만 사용할 수 있다.
최종 성능 비교나 회귀 판단에서는 payload 크기를 보고서에 명확히 남긴다.

## 동시성 프로파일

Framework perf는 payload 크기만으로 충분하지 않다. Framework 계층은 handler dispatch,
request tracking, actor scheduling, event loop, thread pool, backpressure의 영향을 받기
때문에 동시성 조건을 함께 고정해야 한다.

공통 full matrix는 아래 프로파일을 우선 사용한다.

| 프로파일 | `concurrency` | `in_flight` | 목적 |
|----------|---------------|-------------|------|
| `serial` | 1 | 1 | 단일 요청의 기본 지연 시간을 본다 |
| `pipelined` | 1 | 32 | 같은 logical client가 여러 요청을 동시에 기다릴 때의 비용을 본다 |
| `concurrent` | 16 | 16 | 여러 client, actor, worker가 동시에 호출할 때의 비용을 본다 |

Smoke benchmark는 `serial`만 사용할 수 있다. Full matrix나 회귀 판단용 benchmark는
최소한 `serial`과 `pipelined`를 포함한다. `concurrent`는 runtime별 scheduling 차이를
보는 데 중요하므로 정식 비교 보고서에 포함한다.

## 공통 지표

모든 언어 runner는 최소한 아래 지표를 같은 의미로 기록한다.

- `throughput_ops_per_sec`
- `latency_p50_us`
- `latency_p95_us`
- `latency_p99_us`
- `error_count`
- `timeout_count`
- `warmup_seconds`
- `duration_seconds`
- `iterations`
- `total_operations`
- `concurrency_profile`
- `concurrency`
- `in_flight`
- `warmup_iterations`
- `payload_bytes`
- `scenario`
- `measurement_layer`
- `backend`
- `status`

가능하면 CPU model, OS, compiler 또는 runtime version, commit hash, build type도 함께
기록한다. Managed runtime에서는 GC mode와 JIT 관련 설정을 남긴다.

`concurrency`는 동시에 실행되는 논리 client, actor, worker, request source 수를 뜻한다.
`in_flight`는 한 순간에 완료를 기다리는 요청 수를 뜻한다. 단방향 send 시나리오처럼
응답을 기다리지 않는 경우에도 runner가 내부 backpressure를 위해 제한한 outstanding
작업 수를 `in_flight`로 기록한다. 이 두 값이 없으면 같은 throughput 수치라도 비교
의미가 달라질 수 있다.

## 보고서 형식

Runner는 사람이 읽을 수 있는 요약과 기계가 읽을 수 있는 JSON 보고서를 함께 남긴다.
JSON 보고서의 최소 구조는 아래와 같다.

```json
{
  "framework": "zlink",
  "language": "cpp",
  "commit": "unknown",
  "run_id": "unknown",
  "status": "complete",
  "started_at": "2026-06-05T00:00:00Z",
  "ended_at": "2026-06-05T00:00:10Z",
  "host": {
    "os": "unknown",
    "cpu_model": "unknown",
    "cpu_count": 0
  },
  "build": {
    "type": "Release",
    "runtime_path": "path/to/runtime",
    "toolchain": "unknown"
  },
  "runtime": {
    "name": "unknown",
    "version": "unknown",
    "options": []
  },
  "results": [
    {
      "scenario": "client_server_request_reply",
      "measurement_layer": "real_transport_e2e",
      "backend": "zlink",
      "payload_bytes": 4096,
      "concurrency_profile": "serial",
      "concurrency": 1,
      "in_flight": 1,
      "iterations": 0,
      "total_operations": 0,
      "warmup_seconds": 0,
      "warmup_iterations": 0,
      "duration_seconds": 10,
      "throughput_ops_per_sec": 0,
      "latency_p50_us": 0,
      "latency_p95_us": 0,
      "latency_p99_us": 0,
      "error_count": 0,
      "timeout_count": 0,
      "status": "complete"
    }
  ]
}
```

## Runner 배치

언어별 runner는 다음 위치를 기본으로 한다.

- `framework/languages/<language>/perf/run_benchmarks.sh`

Smoke 실행은 기본 동작으로 둔다. Full matrix는 환경 변수나 명시 옵션으로 켠다.
예를 들어 `FRAMEWORK_PERF_FULL_MATRIX=1` 같은 이름을 사용할 수 있다.

Runner는 실행 전에 다음 내용을 출력한다.

- language와 framework package 경로
- 사용한 core 또는 binding runtime 경로
- build type
- 실행할 scenario와 payload 목록
- 보고서 출력 경로

## 상태와 Exit Code

Runner는 자동화에서 해석할 수 있도록 상태와 exit code를 일관되게 사용한다.

| 상태 | 의미 | Perf gate 처리 |
|------|------|----------------|
| `complete` | 선택된 모든 scenario가 끝까지 실행되었다 | 성공 |
| `partial` | 중단되었거나 일부 결과만 있다 | 실패 |
| `failed` | 실행 오류, timeout, 정책 위반이 있다 | 실패 |
| `unsupported` | 해당 언어에 아직 구현되지 않은 scenario다 | 실패 아님 |

Smoke 실행에서 필수 scenario가 `failed`나 `partial`이면 runner는 non-zero로 종료한다.
Full matrix에서 일부 scenario가 `unsupported`이면 보고서에는 남기되 non-zero 원인으로
삼지 않는다. 다만 `unsupported`가 공통 완료 기준에 남아 있으면 해당 언어의 perf
coverage는 완료된 것으로 보지 않는다.

## 성능 기준

Framework perf의 기준은 bindings perf를 기준선으로 잡는다. 다만 이 기준선은
framework가 bindings보다 빨라야 한다는 뜻이 아니다. Framework는 bindings 위에서
handler dispatch, serialization, routing, scheduling 같은 기능을 추가하므로, 목표는
bindings baseline 대비 추가 비용이 합리적인지 확인하는 것이다.

성능 기준은 두 가지로 나눈다.

| 기준 | 비교 대상 | 목적 |
|------|-----------|------|
| 만족 기준 | bindings baseline 대비 framework 결과 | framework overhead가 허용 범위 안인지 판단한다 |
| 회귀 기준 | 이전 stable framework baseline 대비 현재 결과 | 이미 안정화된 framework 성능이 나빠졌는지 판단한다 |

초기 만족 기준은 아래 값을 사용한다. 이 값은 full matrix 결과가 쌓이면 언어별로 다시
조정할 수 있다.

| 언어 | Throughput 기준 | Latency 기준 |
|------|-----------------|--------------|
| C++ | bindings baseline의 70% 이상 | p95가 bindings baseline의 1.5배 이하 |
| .NET | bindings baseline의 50% 이상 | p95가 bindings baseline의 2배 이하 |
| Java | bindings baseline의 50% 이상 | p95가 bindings baseline의 2배 이하 |
| Node | bindings baseline의 40% 이상 | p95가 bindings baseline의 2.5배 이하 |

회귀 기준은 이전 stable framework baseline과 비교한다. 아래 기준을 넘으면 perf gate는
실패로 처리한다.

| 항목 | 실패 기준 |
|------|-----------|
| Throughput | 10% 이상 하락 |
| p95 latency | 15% 이상 증가 |
| p99 latency | 25% 이상 증가 |
| error 또는 timeout | 1건 이상 발생 |
| `partial` result | 1건 이상 발생 |

회귀 판단은 단일 실행 결과만으로 하지 않는다. 같은 조건에서 최소 3회 실행한 뒤 중앙값을
비교한다. 실행 조건은 같은 scenario, 같은 payload, 같은 `concurrency_profile`, 같은
`measurement_layer`, 같은 `backend`여야 한다. 이 조건이 다르면 같은 수치처럼 보이더라도
서로 다른 benchmark로 취급한다.

## 단계별 정리

1. C++ framework부터 runner와 보고서 형식을 정리한다. C++은 현재 framework 작업이
   계속 진행 중이므로 micro, fake backend, real e2e를 함께 둔다.
2. Java, .NET, Node는 같은 보고서 schema와 scenario 이름을 먼저 맞춘다.
3. 모든 언어에서 smoke matrix를 맞춘 뒤 full matrix를 확장한다.
4. 안정적인 수치가 쌓이면 threshold 기반 회귀 gate를 별도 문서로 분리한다.

## POSD 기준

Perf 코드는 제품 코드보다 느슨하게 작성되기 쉽지만, framework perf도 POSD 기준을
따른다. Benchmark를 위해 public API를 우회하거나, scenario마다 같은 setup 지식을
복사하거나, 측정 계층을 숨기는 helper를 만들면 장기적으로 숫자를 해석하기 어려워진다.

공통 runner와 언어별 runner는 얕은 wrapper를 늘리는 방식이 아니라, 같은 개념을
한 곳에서 정의하고 언어별 차이만 아래로 숨기는 방식으로 설계한다.
