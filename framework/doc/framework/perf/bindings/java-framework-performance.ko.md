# Java Framework Performance Plan

> 공통 정책: [ZLink Framework Performance Policy](../README.ko.md)
>
> 적용 범위: `framework/languages/java`

## 1. 목적

Java framework perf는 Spring/Java framework 사용 흐름이 C++, .NET, Node와 같은 의미로
측정되는지 확인한다. JVM warmup과 GC 영향이 있으므로 runner는 runtime option과 warmup
조건을 report에 명확히 남긴다.

## 2. 측정 원칙

- Java framework public API만 사용한다.
- framework 내부 구현 helper를 benchmark hot path에서 직접 호출하지 않는다.
- C++ fake backend와 비교할 때도 Java는 Java framework가 제공하는 공식 fake/test backend가
  생기기 전까지 real transport 또는 public testkit 경로만 사용한다.
- JVM 시작 비용은 측정 active 구간에 포함하지 않는다.
- warmup이 끝나기 전 수치는 최종 evidence로 쓰지 않는다.

## 3. Scenario 적용

공통 scenario 이름을 그대로 사용한다. 아직 Java framework에 없는 scenario는
`unsupported`로 기록한다.

우선순위:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `stream_send`
5. `spot_to_spot_request_reply`
6. `route_mesh_request_reply`
7. `http_handler_roundtrip`

이 목록은 구현 순서다. 최종 report는 공통 정책의 모든 scenario를 `complete` 또는
`unsupported`로 기록해야 한다. 우선순위 목록에 없다는 이유로 report에서 누락하면 안 된다.

## 4. JVM Metadata

Java report는 공통 schema 외에 아래 metadata를 남긴다.

```json
{
  "java_version": "...",
  "jvm": "...",
  "jvm_args": ["-server"],
  "gc": "...",
  "heap_initial_mb": 512,
  "heap_max_mb": 512,
  "warmup_mode": "duration"
}
```

권장 기본값:

- server VM을 사용한다.
- full matrix에서는 size마다 프로세스를 분리한다.
- heap과 GC option을 report에 기록한다.
- JIT 안정화를 위해 smoke보다 full matrix warmup을 길게 둔다.

## 5. Runner 위치

권장 위치:

```text
framework/languages/java/perf/run_benchmarks.sh
framework/languages/java/perf/results/
```

Gradle task를 추가할 경우 shell runner는 Gradle task를 호출하되, 최종 report schema는 공통
JSON 형식을 유지한다.

## 6. 금지 사항

- Java framework를 통하지 않고 binding/native API만 직접 호출해 framework 결과로 보고하지
  않는다.
- JIT warmup이 불충분한 수치를 full matrix 결과로 기록하지 않는다.
- GC pause나 timeout을 숨기기 위해 runner에서 결과를 보정하지 않는다.
- C++ 전용 fake backend 결과와 Java real transport 결과를 같은 `measurement_layer`와
  `backend` 값으로 비교하지 않는다.

## 7. 초기 구현 순서

1. 4KB smoke runner와 공통 JSON report writer를 만든다.
2. client-server request/reply와 send부터 연결한다.
3. stream과 spot scenario를 추가한다.
4. JVM metadata와 warmup policy를 안정화한다.
5. full payload matrix를 추가한다.
6. 동시성 프로파일 `serial`, `pipelined`, `concurrent`를 추가한다.
