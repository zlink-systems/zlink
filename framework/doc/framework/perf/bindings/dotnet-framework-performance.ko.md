# .NET Framework Performance Plan

> 공통 정책: [ZLink Framework Performance Policy](../README.ko.md)
>
> 적용 범위: `framework/languages/dotnet`

## 1. 목적

.NET framework perf는 현재 canonical framework의 사용자 흐름을 기준 성능으로 기록한다.
C++/Java/Node framework는 이 문서의 scenario 의미와 report schema를 기준으로 비교한다.
다만 canonical 기준은 말로만 남기지 않고 commit hash, package version, assembly path를
보고서에 기록해서 같은 기준을 다시 실행할 수 있어야 한다.

## 2. 측정 원칙

- `Zlink.Framework` public API만 사용한다.
- framework 내부 구현 type을 reflection으로 호출하지 않는다.
- ASP.NET Core hosting 비용을 포함하는 benchmark와 framework dispatch만 보는 benchmark를
  분리한다.
- JIT, GC, ReadyToRun 설정을 report metadata에 남긴다.
- commit hash, package version, assembly path를 report metadata에 남긴다.

## 3. Scenario 적용

.NET은 공통 scenario의 의미 기준 역할을 한다.

우선순위:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `dealer_mesh_request_reply`
5. `route_mesh_request_reply`
6. `stream_send`
7. `stream_request_reply`
8. `spot_to_spot_request_reply`
9. `spot_to_router_egress`
10. `router_to_spot_ingress`
11. `http_handler_roundtrip`

이 목록은 구현 순서다. .NET은 공통 scenario 의미의 기준 역할을 하므로 최종 report에서
공통 scenario가 누락되면 안 된다. 아직 측정 runner가 없는 scenario는 `unsupported`로
기록하고, 구현이 끝난 scenario는 `complete` 또는 `failed`로 기록한다.

## 4. Benchmark Runtime

둘 중 하나를 선택해 사용한다.

| 방식 | 용도 |
|------|------|
| custom runner | 다른 언어 runner와 같은 process model, report schema를 맞추기 쉬움 |
| BenchmarkDotNet | micro benchmark와 runtime 진단에 유리 |

언어 간 비교용 공식 report는 공통 JSON schema를 출력해야 한다. BenchmarkDotNet을 쓰더라도
공통 schema 변환 단계를 둔다.

## 5. .NET Metadata

```json
{
  "dotnet_version": "...",
  "runtime": "CoreCLR",
  "tiered_compilation": true,
  "ready_to_run": true,
  "gc_mode": "server",
  "benchmark_engine": "custom",
  "package_version": "...",
  "assembly_path": "..."
}
```

## 6. Runner 위치

권장 위치:

```text
framework/languages/dotnet/perf/run_benchmarks.sh
framework/languages/dotnet/perf/results/
```

## 7. 금지 사항

- framework 내부 runtime type을 reflection으로 직접 호출하지 않는다.
- ASP.NET Core HTTP 결과와 framework-only dispatch 결과를 같은 scenario로 섞지 않는다.
- BenchmarkDotNet summary만 남기고 공통 JSON report를 생략하지 않는다.
- interrupted result를 canonical .NET 기준으로 기록하지 않는다.

## 8. 초기 구현 순서

1. canonical scenario 이름과 DTO/payload generator를 정한다.
2. custom runner로 4KB smoke를 만든다.
3. ASP.NET Core HTTP handler roundtrip을 별도 scenario로 추가한다.
4. full payload matrix와 runtime metadata를 추가한다.
5. 동시성 프로파일 `serial`, `pipelined`, `concurrent`를 추가한다.
6. C++ fake backend 결과와 비교할 때는 `measurement_layer`와 `backend` 차이를 명확히 표시한다.
