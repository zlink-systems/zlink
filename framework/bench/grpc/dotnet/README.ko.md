# .NET messaging local bench

이 bench는 server process A가 server process B 하나를 향하는 세 구현을 같은 workload로 비교한다.
공통 측정·결과 계약은 [`../README.ko.md`](../README.ko.md)가 소유한다.

| 구현 이름 | request/reply | command |
|---|---|---|
| `grpc-dotnet` | unary `Echo` | unary `Command`와 empty reply |
| `zlink-dotnet` | raw ROUTER request/reply | raw ROUTER send |
| `zlink-framework-dotnet` | `RequestToNode("bench", serverRid, ...)` | `SendToNode("bench", serverRid, ...)` |

framework 행은 channel 이름으로 node를 고르지 않고 `bench-server` RID를 직접 지정한다. target
server는 node request/send handler를 하나씩 등록하고, 세 행 모두 같은
`BenchPayload { bytes body = 1; }` protobuf body를 사용한다.

## 실행

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o <owner> -d '<설명>' -- \
  bash framework/bench/grpc/dotnet/run_local.sh
```

입력은 규격 [§11](../README.ko.md#11-runner-입력과-결과-배치)의 환경 변수가 전부다. 기본 실행은
payload 1024·4096 B에서 `request-serial`, `request-backpressure`, `send-saturation` 세 패턴을 돈다.
예를 들어 framework 행의 serial 셀만 실행하려면 다음처럼 지정한다.

```bash
IMPLEMENTATIONS=zlink-framework-dotnet PATTERNS=request-serial \
  bash framework/bench/grpc/dotnet/run_local.sh
```

`request-backpressure`에는 application in-flight 상한을 추가하지 않는다.

포트 범위는 `5200`~`5219`다. framework 행은 source trigger/stats `5212`/`5213`, target
RouteMesh/stats `5214`/`5215`를 사용한다. 결과는 기본적으로
`framework/bench/grpc/log/<stamp>/dotnet/run<N>/` 아래에 셀마다 기록된다(§11).

request 처리량은 정상 echo 완료 수(KOPS), `send-saturation`은 target이 active header로 받은
메시지 수(KMSG/s)를 기준으로 한다. 판정 기준 패턴은 `request-backpressure`다.
