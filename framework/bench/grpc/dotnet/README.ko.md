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

실행 인자와 결과물 배치는 언어와 무관하게 같다. 계약은 규격 §3.1이고 runner는 그 밖의 인자를
거절한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o <owner> -d '<설명>' -- \
  bash framework/bench/grpc/dotnet/run_local.sh --skip-build \
    --output framework/bench/grpc/log/dotnet/<이름>/r1
```

기본 실행은 payload 4096 B에서 `request-serial`, `request-backpressure`,
`send-saturation` 세 패턴을 돈다. framework 행의 serial 셀만 실행하려면 다음처럼 지정한다.

```bash
bash framework/bench/grpc/dotnet/run_local.sh --skip-build \
  --implementation zlink-framework-dotnet --scenario request-serial \
  --output /tmp/dotnet-smoke
```

§3.1의 입력 밖에서 이 runner가 읽는 값은 `CONFIGURATION`(기본 `Release`) 하나다. 나머지는 고정값이다 — send concurrency 8, request window 100,
drain 상한 30초, 시나리오 종료 상한 300초.

`request-backpressure`에는 application in-flight 상한을 추가하지 않는다. runner가 결과 JSON
계약을 위해 request-window 값을 전달하더라도 이 값은 선택된 세 패턴의 제출 깊이를 제한하지
않는다.

포트 범위는 `5200`~`5219`다. framework 행은 source trigger/stats `5212`/`5213`, target
RouteMesh/stats `5214`/`5215`를 사용한다. 결과 배치는 §3.1이 정하고, `--output`을 주지 않으면
`framework/bench/grpc/log/dotnet/<실행 시각>/`에 쓴다.

request 처리량은 정상 echo 완료 수(KOPS), `send-saturation`은 target이 active header로 받은
메시지 수(KMSG/s)를 기준으로 한다. 판정 기준 패턴은 `request-backpressure`이며, 이전
`request-window` 값과 비교하지 않는다.
