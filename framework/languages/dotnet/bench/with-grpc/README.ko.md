# .NET messaging local bench

이 bench는 로컬 단일 client/server 환경에서 세 구현을 같은 항목으로 비교한다.

| 구현 이름 | request/reply | command |
|-----------|---------------|---------|
| `grpc-dotnet` | unary RPC | unary RPC returning empty reply |
| `zlink-dotnet` | raw request callback | `Dealer.Send().Submit()` |
| `zlink-framework-dotnet` | `RequestToChannel(...).Async<TReply>()` | `SendToChannel(...).SubmitAsync()` |

payload 크기는 protobuf `bytes body` 전체 크기 기준으로 `1024`, `4096` bytes를 기본으로
실행한다. 앞 29 bytes는 측정 header로 사용한다. 공통 기준은
`framework/doc/framework/common/bench/with-grpc-local.ko.md`를 따른다.

## 실행

```bash
./framework/languages/dotnet/bench/with-grpc/run_local.sh
```

주요 환경 변수:

| 변수 | 기본값 | 의미 |
|------|--------|------|
| `PAYLOAD_SIZES` | `1024,4096` | payload 크기 목록 |
| `REQUEST_WINDOW` | `100` | `request-window` 측정에서 유지할 최대 미완료 request 수 |
| `SEND_CONCURRENCY` | `8` | send/command 측정에서 동시에 보내는 worker 수 |
| `WARMUP` | `1000` | warmup 호출 수 |
| `DURATION_SECONDS` | `5` | measured active 구간 시간 |
| `COMMAND_SETTLE_MS` | `200` | 단방향 command 전송 뒤 server stats 반영을 기다리는 시간 |
| `TIMEOUT_SECONDS` | `300` | 한 시나리오가 끝나기를 기다리는 최대 시간 |

특정 패턴만 실행하려면 client 인자를 그대로 넘긴다.

```bash
./framework/languages/dotnet/bench/with-grpc/run_local.sh --scenario request-window
```

기본 포트:

- gRPC: `http://127.0.0.1:5071`
- ZLink framework channel: `tcp://127.0.0.1:5072`
- ZLink framework stats HTTP: `http://127.0.0.1:5073`
- gRPC stats HTTP: `http://127.0.0.1:5074`
- ZLink binding raw DEALER/ROUTER: `tcp://127.0.0.1:5075`
- ZLink binding stats HTTP: `http://127.0.0.1:5076`
- ZLink binding command DEALER/ROUTER: `tcp://127.0.0.1:5077`

결과는 콘솔 표, perf 형식의 `RESULT,current,...` 라인,
`log/with_grpc_dotnet_YYYYMMDD_HHMMSS/` 아래에 기록된다. report 파일 이름은
`with_grpc_dotnet_YYYYMMDD_HHMMSS.txt`처럼 실행 시각을 포함한다.
`results.json`은 실행 환경과 설정을 담은 `metadata`와 측정값 목록인 `results`로 구성된다.

## 측정 단위

`request`는 echo reply가 돌아온 완료 수를 기준으로 `Throughput`을 `KOPS`로 표시한다.
`1 KOPS`는 초당 1,000건의 request/reply 완료를 뜻한다. 콘솔 표와 `report.txt`는
perf와 같은 `Throughput`, `Bandwidth`, `Lat.Mean(ms)`, `Lat.P95(ms)`, `Lat.P99(ms)`
컬럼을 사용한다.

`request`는 두 모드로 해석한다.

- `inflight=1`: 한 요청의 reply가 오기 전에는 다음 요청을 보내지 않는 sequential 측정이다.
- `request-window=N`: `N`개 task/slot이 동시에 request를 유지한다. reply를 받은 slot은 active phase
  시간이 끝나기 전까지 다음 request를 바로 보낸다. measured 시간이 끝나면 새 request를 더 보내지
  않고 이미 보낸 request의 reply를 모두 받은 뒤 completed echo 수로 `KOPS`를 계산한다.

`command`는 `SEND_CONCURRENCY`개 worker가 동시에 command를 보낸다.
처리량은 server가 active phase에서 받은 메시지 수를 기준으로 `KMSG/s`로 표시한다.
`1 KMSG/s`는 초당 1,000개 메시지를 뜻한다. ZLink `send`는 reply가 없으므로 client의
`SubmitAsync()` 완료 수만으로 처리량을 계산하지 않는다.

## 측정 방식

payload 앞에는 `bindings/c/perf`와 같은 의미의 29-byte metric header를 넣는다. header에는
run id, phase, payload size, sequence, send timestamp를 기록한다.

- `request`: echo reply에 돌아온 header를 client가 검증하고, completed echo 수로 `KOPS`를 계산한다.
- `command`: server가 active phase header를 가진 메시지를 받은 수로 `KMSG/s`를 계산하고,
  header timestamp로 server-side 수신 latency를 기록한다.

## 해석 주의

`request`는 양쪽 모두 reply를 기다린다. `command`에서 gRPC empty unary는 empty reply까지
기다리지만, ZLink `send`는 reply 없는 제출 경로다. 두 결과는 따로 해석한다.

성능 판정 기준은 gRPC가 아니라 같은 ZLink 계층이다. `zlink-dotnet`은 같은 조건의 `zlink-c`
request-window 결과 대비 80% 이상을 목표로 보고, `zlink-framework-dotnet`은 `zlink-dotnet`
대비 80% 이상을 목표로 본다. C report의 `RESULT` throughput은 KOPS이고, .NET report의
`RESULT` throughput은 초당 완료 수이므로 C와 비교할 때는 .NET 값을 1000으로 나누어 맞춘다.
