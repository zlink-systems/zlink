# fwb2-03 결과 — .NET server-driven bench runner

## 결과

`framework/bench/grpc/dotnet/`의 측정 주체를 standalone client에서 source process A로 옮겼다.
runner는 셀마다 target B와 source A를 새 process로 시작하고, warmup·active trigger와 settle을
차례로 수행한 뒤 B의 snapshot을 A의 셀 원본에 `target_stats`로 합친다. 세 구현과 네 패턴을
선택할 수 있으며, 요청된 smoke 셀은 모두 rc=0이었다.

- source A의 측정 구간은 outbound public call 직전부터 완료까지다.
- request 셀은 A의 완료 수와 B의 수신 수가 다르면 runner가 실패한다.
- `send-saturation`의 최종 처리량 입력은 settle 뒤 `target_stats.received`다.
- 셀 JSON은 집계기가 읽는 `with-grpc-cell-v1` 형식이며 `role`, `trigger`, `streams`,
`target_stats`를 셀에 기록한다. `trigger`에는 규격의 8개 필드와 실제 A trigger URL을 넣는다.
- Core, binding, Framework runtime, 공용 집계기, 규격 README, 언어별 README는 수정하지 않았다.
- 3-run 측정은 수행하지 않았다.

작업 시작 시 `main`의 HEAD는 `22c8dfe08d0dfb5f6e4dbceb983fd406a9a2aeaa`였다. 작업 도중
감독자가 문서·브리프, 집계기, versioning 문서와 trigger schema 커밋을 순서대로 반영했다.
최종 smoke 원본과 보고서 작성 시점 HEAD는
`59521bf2f4aec71f487506d2aa50472460bde07e`다. branch는 계속 `main`이었다. 마지막 trigger
schema 커밋이 확정한 `endpoint`·`receivedAtUnixMs` 필드를 writer와 최종 smoke에 반영했다.

## Process 구성

| 구현 | source A | A 포트 | target B | B 포트 | 사용 모듈 |
|---|---|---|---|---|---|
| `grpc-dotnet` | `WithGrpcBench.Client`가 `GrpcChannel` 하나와 logical stream 수만큼 unary stub을 사용한다 | trigger `5200`, stats `5201` | `WithGrpcBench.GrpcServer`가 `Echo`와 `Command`를 처리한다 | gRPC `5202`, stats `5203` | `Grpc.Net.Client`, ASP.NET Core gRPC |
| `zlink-dotnet` | `WithGrpcBench.Client`가 셀에 필요한 raw ROUTER socket만 만들고 B에 연결한다 | trigger `5205`, stats `5206` | `WithGrpcBench.ZLinkRawServer`가 request echo ROUTER와 command count ROUTER를 분리한다 | request `5207`, command `5208`, stats `5209` | published `Zlink` 0.17.6 |
| `zlink-framework-dotnet` | `WithGrpcBench.Client`가 `IZLinkRouteClient`로 channel request/send를 호출한다 | trigger `5212`, stats `5213` | `WithGrpcBench.ZLinkServer`가 typed request/send handler를 실행한다 | RouteMesh `5214`, stats `5215` | 저장소 `Zlink.Framework`, `Zlink.Framework.AspNetCore`, `Zlink.Framework.Codecs.Protobuf` |

Framework A의 RouteMesh listener는 기존 public 구성 방식대로 `tcp://127.0.0.1:0`을 사용하고,
고정된 비교 endpoint는 위 표의 B `5214`다. raw A는 B의 고정 endpoint에 연결한다. runner는 시작
전과 셀 종료 뒤 `5200-5219`의 LISTEN socket을 확인한다. 다른 listener가 있으면 포트를 바꾸지
않고 중단한다.

runner의 셀 순서는 다음과 같다.

1. `5200-5219` listener 부재 확인
2. B 시작과 stats 응답 확인
3. A 시작과 `ready=true` 확인
4. `phase=warmup` trigger와 A의 `phase=idle` 확인
5. `phase=active` trigger와 active 구간 완료 확인
6. A·B counter가 안정될 때까지 bounded settle
7. A 셀 JSON에 B snapshot을 `target_stats`로 병합하고 request count 대조
8. A·B process group 종료와 포트 해제 확인

## ServerSupport 참조 방식

`Client/WithGrpcBench.Client.csproj`가
`framework/languages/dotnet/perf/ZLink.Framework.Perf.ServerSupport`를 `ProjectReference`로 직접
참조한다. 그 프로젝트에 `BenchHttpApplication.cs`를 추가해 다음 책임을 한 곳에 두었다.

- `/bench/start`와 `/bench/stats` listener 배선
- trigger JSON의 exact field와 값 검증
- `runId/cellId/phase`별 중복 trigger acknowledgement
- `idle`, `warmup`, `active`, `failed` phase 전이
- source readiness와 제출·완료·오류·in-flight snapshot
- trigger 포트와 stats 포트의 분리

세 transport 구현은 trigger·HTTP state를 구현하지 않고 workload delegate와 counter snapshot만
제공한다. B는 기존 경량 stats host를 유지한다. gRPC/raw B가 `ServerSupport`를 참조하면
Framework assembly가 target process의 비교 조건에 포함되므로 B에는 이 참조를 추가하지 않았다.

검토한 대안은 다음과 같다.

| 대안 | 판정 |
|---|---|
| Client 안에 구현별 minimal API와 trigger state를 작성 | 같은 idempotency·phase 규칙이 세 transport에 반복되므로 사용하지 않았다 |
| 구현마다 A executable을 별도로 추가 | process 역할은 분명하지만 HTTP lifecycle, 계측, JSON writer가 반복되므로 사용하지 않았다 |
| 공용 A executable과 transport adapter, `ServerSupport`의 공통 HTTP controller | trigger와 결과 규칙의 소유자가 하나이므로 적용했다 |

## Logical stream 구성

| 패턴 | `streams.count` | `streams.inFlightPerStream` | .NET 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | 순차 Task loop 하나 |
| `request-window` | 1 | 100 | 하나의 logical stream window를 공유하는 Task 100개 |
| `request-backpressure` | 1 | `null` | application in-flight 상한 없이 request Task를 제출하고 completion pump에 주기적으로 양보 |
| `send-saturation` | 8 | 1 | logical stream마다 Task와 gRPC stub 또는 raw ROUTER socket 하나 |

`request-backpressure`의 256회 `Task.Yield`는 scheduler가 completion을 처리할 기회를 주며
in-flight admission을 제한하지 않는다. active 종료 뒤 미완료 작업은 30초 안에서 기다리고,
상한에 도달하면 `abandoned`를 기록한다.

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `framework/languages/dotnet/perf/ZLink.Framework.Perf.ServerSupport/BenchHttpApplication.cs` | 공용 trigger DTO, 중복 방지, phase controller, source HTTP endpoint를 추가했다 |
| `framework/bench/grpc/dotnet/Client/Program.cs` | Client를 source A로 재구성하고 transport adapter, 네 패턴, payload header 검증, 계측, S2S 셀 writer를 구현했다 |
| `framework/bench/grpc/dotnet/Client/WithGrpcBench.Client.csproj` | canonical `ServerSupport` project reference를 추가했다 |
| `framework/bench/grpc/dotnet/Shared/BenchServerMetrics.cs` | B snapshot에 `ready`, `received`, `completed`, in-flight 호환 필드를 제공했다 |
| `framework/bench/grpc/dotnet/GrpcServer/Program.cs` | 기본 포트를 5202/5203으로 바꾸고 request도 수신 계측하며 gRPC listener를 IPv4 loopback으로 제한했다 |
| `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs` | 기본 포트를 5207/5208/5209로 바꾸고 request 수신도 계측했다 |
| `framework/bench/grpc/dotnet/ZLinkServer/Program.cs` | 기본 포트를 5214/5215로 바꾸고 typed request handler에서 수신을 계측했다 |
| `framework/bench/grpc/dotnet/run_local.sh` | 포트 preflight, 셀 matrix, B→A 시작, HTTP trigger, settle, JSON 병합, count 검증, process 종료를 구현했다 |

수정 전/후 규칙 수: lifecycle·trigger·stream·result 소유권을 단위로 세면 **7 → 4**다. 수정 전에는
runner의 전체 B 시작, Client의 전체 transport 시작, Client matrix 선택, 패턴별 warmup/reset,
패턴별 stats 조회, transport별 drain, run 단위 결과 작성 규칙이 분산돼 있었다. 수정 후에는
runner가 셀 lifecycle과 target 병합, `ServerSupport`가 trigger phase, source workload가
pattern-to-stream, B가 수신 snapshot을 각각 한 번만 소유한다.

## 언어별 문서 입력값

감독자가 계획 §5의 절 1~6으로 옮길 수 있도록 현재 구현값을 정리한다.

### 1. 비교 대상

| 구현 | request | send/command |
|---|---|---|
| `grpc-dotnet` | `BenchServiceClient.EchoAsync` unary RPC | `BenchServiceClient.CommandAsync` unary RPC와 `Empty` reply |
| `zlink-dotnet` | raw ROUTER `Request(peer).Message(...).Async()` | raw ROUTER `Send(peer).Message(...).Async()` |
| `zlink-framework-dotnet` | `RequestToChannel("bench", payload).Async<BenchPayload>()` | `SendToChannel("bench", payload).Async()` |

세 구현은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header를 사용한다. raw는 framework
envelope와 protobuf body를 두 part로 전송한다.

### 2. 실행 방법

전체 matrix:

```bash
./framework/bench/grpc/dotnet/run_local.sh
```

한 셀:

```bash
PAYLOAD_SIZES=1024 ./framework/bench/grpc/dotnet/run_local.sh \
  --scenario request-window \
  --implementation zlink-framework-dotnet
```

| 입력 | 기본값 | 동작 |
|---|---|---|
| `PAYLOAD_SIZES` | `1024,4096` | 실행할 payload 목록. 두 값 이외에는 preflight 실패 |
| `DURATION_SECONDS` | `5` | active 시간 |
| `WARMUP` | `1000` | active 전에 수행할 warmup 호출 수 |
| `REQUEST_WINDOW` | `100` | `request-window`의 합계 in-flight. 다른 값은 preflight 실패 |
| `SEND_CONCURRENCY` | `8` | `send-saturation` stream 수. 다른 값은 preflight 실패 |
| `TIMEOUT_SECONDS` | `300` | process/operation 상한. 다른 값은 preflight 실패 |
| `COMMAND_SETTLE_MS` | `200` | counter가 안정됐다고 판정하는 최소 quiet 구간 |
| `DRAIN_BOUND_MS` | `30000` | settle 상한 |
| `SKIP_BUILD` | `0` | `1`이면 runner 안의 solution build 생략 |
| `OUTPUT` | `framework/bench/grpc/log/dotnet/with_grpc_dotnet_<stamp>` | run root |
| `CONFIGURATION` | `Release` | build와 `dotnet run` 구성 |
| `--scenario` | `all` | `all`, `request`, 네 exact pattern, 기존 `send`/`command` alias |
| `--implementation` | `all` | `all` 또는 세 exact 구현 이름 |

### 3. Process 구성

위의 “Process 구성” 표와 셀 순서를 사용한다. trigger client는 runner의 `curl`이고 workload를
생성하지 않는다. 각 셀은 새 A/B process pair를 사용한다.

### 4. 언어별 값

| 항목 | 값 |
|---|---|
| warmup | 기본 1000회. smoke는 100회 |
| active | 기본 5초. smoke는 2초 |
| gRPC source | `GrpcChannel` 1개, `SocketsHttpHandler.MaxConnectionsPerServer=1`, `EnableMultipleHttp2Connections=false`, logical stream 수만큼 stub |
| gRPC target | ASP.NET Core gRPC, Kestrel IPv4 loopback, HTTP/2, `AddGrpc()` 기본 server 구성 |
| .NET SDK/runtime | 최종 smoke에서 SDK `8.0.130`, runtime `8.0.30` |
| gRPC package | `Grpc.Net.Client`, `Grpc.AspNetCore`, `Grpc.Core.Api` `2.62.0` |
| ZLink binding | published package/assembly `0.17.6` / `0.17.6.0` |
| Framework | 저장소 source project reference, 최종 smoke HEAD `59521bf2f4` |
| 실행 OS/CPU | Ubuntu 24.04.4 LTS, Intel Core Ultra 7 265K |

### 5. 결과 위치

```text
framework/bench/grpc/log/dotnet/with_grpc_dotnet_<stamp>/
├── with_grpc_dotnet_<stamp>.txt
└── <implementation>-<pattern>-<payload>/
    ├── results.json
    ├── report.txt
    ├── source.log
    ├── target.log
    └── target-stats.json
```

`results.json`은 `schema=with-grpc-cell-v1`과 `cells[]`를 가지며 runner가 같은 파일의 source 셀에
`target_stats`를 병합한다. 3-run을 집계할 때는 각 run root의 셀 디렉터리를 모두 넘긴다.

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang dotnet \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-3>/*' \
  --json-out aggregate.json
```

### 6. 알려진 제약

- .NET의 네 패턴과 세 구현에 `unsupported` 셀은 없다.
- `RAW_SOCKET`은 비교 계약에 따라 `router`만 허용한다.
- Framework A의 RouteMesh listener는 loopback port 0을 사용하며 B의 비교 endpoint는 5214로
  고정한다.
- 이 작업은 smoke만 수행했다. 공개 비교값과 G5 판정에는 감독자가 실행할 3-run이 필요하다.

## Build와 정적 검증

| 검증 | 결과 |
|---|---|
| `ZLINK_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl dotnet build framework/bench/grpc/dotnet/WithGrpcBench.sln -c Release` | 최종 rc=0, 오류 0, 기존 SourceLink 경고 3건 |
| `bash -n framework/bench/grpc/dotnet/run_local.sh` | rc=0 |
| `git diff --check` | rc=0 |
| fwb2-02 `cells_from_cell_json` + `merge_server_driven_cells`로 최종 원본 5개 parse | 모두 `complete` |

첫 solution build는 `ZLink.Framework.Perf` namespace와 binding의 `Systems.Zlink.Zlink` 형식명이
충돌해 rc=1이었다. binding 형식명을 완전 한정한 뒤 focused build와 최종 solution build가
통과했다.

## Smoke 검증

모든 smoke는 `bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-03 ...`으로 제출해 완료를
기다렸다. 아래 표는 request target이 count만 기록하고 send target이 latency도 기록하는 최종
tree에서 다시 실행한 티켓만 싣는다.

| 셀 | ticket | rc | RESULT throughput | source 완료 / target 수신 | 오류 | stream | drain |
|---|---|---:|---|---:|---:|---|---:|
| `grpc-dotnet request-serial@1024` | `2-1788938493-36141-sol-fwb2-03-latest_spec4_trigger_fields_final_smoke_` | 0 | `3722.000` | `7444 / 7444` | `0 / 0` | `1×1` | 274ms |
| `zlink-dotnet request-serial@1024` | 같은 ticket | 0 | `7321.500` | `14643 / 14643` | `0 / 0` | `1×1` | 275ms |
| `zlink-framework-dotnet request-serial@1024` | 같은 ticket | 0 | `1339.500` | `2679 / 2679` | `0 / 0` | `1×1` | 272ms |
| `zlink-dotnet send-saturation@1024` | `2-1788938521-38556-sol-fwb2-03-latest_spec4_trigger_fields_final_smoke_` | 0 | `648910.500` | A boundary `1297821` / settle B `1452765` | `0 / 0` | `8×1` | 571ms |
| `zlink-framework-dotnet request-window@1024` | `2-1788938542-40299-sol-fwb2-03-latest_spec4_trigger_fields_final_smoke_` | 0 | `2995.000` | `5990 / 5990` | `0 / 0` | `1×100` | 277ms |

각 셀은 RESULT 9개를 출력했고 JSON에 `role`, `trigger`, `streams`, `target_stats`가 있었다.
send의 표에 적은 RESULT는 A가 active 종료 직후 관찰한 B boundary 값이다. 최종 JSON과 집계기는
규격대로 settle 뒤 `target_stats.received`를 사용한다.

수정 중 ticket `2-1788936914-6383-...`과 `2-1788937102-11948-...`은 첫 gRPC 셀을 완료한 뒤
TIME_WAIT socket을 listener로 오판하여 rc=1이었다. 포트 검사를 bind 시도에서 LISTEN 조회로
고친 뒤 최종 티켓은 셀 사이 종료·재시작을 포함해 rc=0이었다. 남은 smoke 실패는 없다.

## Runtime 결함

발견한 Core, binding, Framework runtime 결함은 없다. runtime source는 수정하지 않았다.

## BLOCKERS

없음.

## 감독자 재검증 명령

build 전 `/proc/loadavg`의 첫 값이 10 미만인지 확인한 뒤 다음 명령을 실행한다.

```bash
cut -d' ' -f1 /proc/loadavg
ZLINK_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl \
  dotnet build framework/bench/grpc/dotnet/WithGrpcBench.sln -c Release
bash -n framework/bench/grpc/dotnet/run_local.sh
git diff --check
```

smoke는 다음 ticket 명령을 그대로 사용한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-03 \
  -d "review: dotnet server-driven request-serial@1024" -- \
  env SKIP_BUILD=1 DURATION_SECONDS=2 WARMUP=100 PAYLOAD_SIZES=1024 \
  OUTPUT=/tmp/zlink-sol-fwb2-03/review-request-serial \
  bash framework/bench/grpc/dotnet/run_local.sh \
  --scenario request-serial --implementation all

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-03 \
  -d "review: dotnet raw send-saturation@1024" -- \
  env SKIP_BUILD=1 DURATION_SECONDS=2 WARMUP=100 PAYLOAD_SIZES=1024 \
  OUTPUT=/tmp/zlink-sol-fwb2-03/review-raw-send \
  bash framework/bench/grpc/dotnet/run_local.sh \
  --scenario send-saturation --implementation zlink-dotnet

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-03 \
  -d "review: dotnet framework request-window@1024" -- \
  env SKIP_BUILD=1 DURATION_SECONDS=2 WARMUP=100 PAYLOAD_SIZES=1024 \
  OUTPUT=/tmp/zlink-sol-fwb2-03/review-framework-window \
  bash framework/bench/grpc/dotnet/run_local.sh \
  --scenario request-window --implementation zlink-framework-dotnet
```
