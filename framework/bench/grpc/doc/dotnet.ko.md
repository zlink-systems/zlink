# .NET messaging bench

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 .NET에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 .NET 고유의 값만 담는다.
네 언어 문서는 같은 절 번호를 쓴다.

## 1. 비교 대상

| 구현 | request | send/command | 사용 API |
|---|---|---|---|
| `grpc-dotnet` | `BenchServiceClient.EchoAsync` unary RPC | `BenchServiceClient.CommandAsync` unary RPC, `Empty` reply | `Grpc.Net.Client`, ASP.NET Core gRPC |
| `zlink-dotnet` | raw ROUTER `Request(peer).Message(...).Async()` | raw ROUTER `Send(peer).Message(...).Async()` | published `Zlink` binding |
| `zlink-framework-dotnet` | `RequestToChannel("bench", payload).Async<BenchPayload>()` | `SendToChannel("bench", payload).Async()` | `Zlink.Framework`, `Zlink.Framework.AspNetCore`, `Zlink.Framework.Codecs.Protobuf` |

세 구현은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
framework envelope와 protobuf body를 두 part로 보낸다.

## 2. 실행 방법

실행 인자와 결과물 배치는 언어와 무관하게 같다. 규격 §3.1이 그 계약이고, runner는 그 밖의
인자를 거절한다. 측정은 항상 perf 티켓 큐로 낸다.

```bash
# 전체 격자 1 run
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "dotnet with-grpc r1" -- \
  bash framework/bench/grpc/dotnet/run_local.sh --skip-build \
    --output framework/bench/grpc/log/dotnet/<이름>/r1

# 한 셀
bash framework/bench/grpc/dotnet/run_local.sh --skip-build --scenario request-serial \
  --implementation zlink-framework-dotnet --payload-sizes 1024 --duration-seconds 2 \
  --output /tmp/dotnet-smoke
```

여러 언어를 3 run씩 돌려 §7.2 판정까지 받으려면 `framework/bench/grpc/run_all.sh`를 쓴다.
runner는 run을 반복하지 않는다 — run 하나가 실행 하나다.

§3.1의 여섯 입력 밖에서 이 runner가 읽는 값은 아래뿐이다.

| 입력 | 기본값 | 의미 |
|---|---|---|
| `CONFIGURATION` | `Release` | build와 `dotnet run` 구성 |

고정값: request window 100, send concurrency 8, raw socket ROUTER, process 상한 300초,
drain 상한 30초, settle quiet 200ms. 빌드는 `WithGrpcBench.sln`을 Release로 만든다.

## 3. 프로세스 구성

| 구현 | source A | A 포트(trigger/stats) | target B | B 포트 |
|---|---|---|---|---|
| `grpc-dotnet` | `WithGrpcBench.Client` — `GrpcChannel` 하나, logical stream 수만큼 unary stub | 5200/5201 | `WithGrpcBench.GrpcServer` — `Echo`·`Command` | gRPC 5202, stats 5203 |
| `zlink-dotnet` | `WithGrpcBench.Client` — 셀에 필요한 raw ROUTER만 생성 | 5205/5206 | `WithGrpcBench.ZLinkRawServer` — request echo ROUTER와 command count ROUTER 분리 | request 5207, command 5208, stats 5209 |
| `zlink-framework-dotnet` | `WithGrpcBench.Client` — `IZLinkRouteClient` | 5212/5213 | `WithGrpcBench.ZLinkServer` — typed request/send handler | RouteMesh 5214, stats 5215 |

A의 trigger·stats·phase 규칙은 canonical perf runner의 `ZLink.Framework.Perf.ServerSupport`
(`BenchHttpApplication`)를 ProjectReference로 재사용한다. trigger client는 runner의 `curl`이며
부하를 만들지 않는다. 셀 순서는 규격 §10.4 그대로이고 셀마다 새 A/B process 쌍을 쓴다. runner는
시작 전과 셀 종료 뒤 5200-5219의 LISTEN 소켓을 확인하고, 있으면 포트를 바꾸지 않고 중단한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | .NET 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | 순차 Task loop 하나 |
| `request-window` | 1 | 100 | 하나의 logical window를 공유하는 Task 100개 |
| `request-backpressure` | 1 | 없음 | application in-flight 상한 없이 제출, 256회마다 `Task.Yield` |
| `send-saturation` | 8 | 1 | stream마다 Task 하나. **연결은 세 행 모두 하나다** — gRPC는 채널 하나를 stub 8개가 공유하고, raw는 ROUTER 하나를 stream 8개가 공유하며, framework는 RouteMesh socket 하나다 |

## 4. 언어별로 다르게 둔 값

| 항목 | 값 |
|---|---|
| warmup | 1000회 (smoke는 100회) |
| gRPC source | `GrpcChannel` 1개, `SocketsHttpHandler.MaxConnectionsPerServer=1`, `EnableMultipleHttp2Connections=false` |
| gRPC target | ASP.NET Core gRPC, Kestrel IPv4 loopback, HTTP/2, `AddGrpc()` 기본 구성 |
| .NET SDK / runtime | 8.0.130 / 8.0.30 (2026-09-09 측정) |
| gRPC package | `Grpc.Net.Client`, `Grpc.AspNetCore`, `Grpc.Core.Api` 2.62.0 |
| ZLink binding | published 0.17.6 |
| framework | 저장소 소스 ProjectReference(측정한 커밋을 원본에 기록) |

## 5. 결과 위치

```text
<OUTPUT>/
├── report.txt · runner.log
└── <implementation>-<pattern>-<payload>/
    ├── results.json        # with-grpc-cell-v1: role·trigger·streams·target_stats
    ├── report.txt          # 그 셀만의 표
    ├── source.log / target.log
    └── target-stats.json
```

3-run 집계:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' \
  --format full
```

## 6. 알려진 제약

- 네 패턴 × 세 구현에 `unsupported` 셀은 없다.
- raw 비교는 ROUTER↔ROUTER만 허용한다(`RAW_SOCKET=router`).
- framework A의 RouteMesh listener는 loopback port 0을 쓰고, B의 비교 endpoint는 5214로 고정한다.
- `request-backpressure`의 framework 셀은 admission 거절이 request 오류로 표면화돼(결정 기록
  FB-042·FB-047) 오류 셀로 기록되며 처리량 판정에 쓰지 않는다.
