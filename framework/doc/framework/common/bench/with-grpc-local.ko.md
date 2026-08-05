# .NET messaging local bench 규격

이 문서는 로컬 개발 머신에서 gRPC .NET, ZLink raw binding .NET, ZLink framework .NET의
상대 비용을 같은 형식으로 비교하기 위한 기준이다. 운영 환경의 mesh, TLS, L7 load balancer,
multi-node 분배, 네트워크 지연을 대표하지 않는다.

## 1. 비교 대상

비교 대상은 아래 세 구현이다. report 표와 `RESULT` 라인에서도 같은 이름을 사용한다.

| 구현 이름 | 의미 |
|-----------|------|
| `grpc-dotnet` | ASP.NET Core gRPC unary RPC |
| `zlink-dotnet` | framework를 거치지 않는 raw .NET binding의 DEALER/ROUTER TCP 경로 |
| `zlink-framework-dotnet` | framework channel messaging의 client/server channel |

기본 실행 순서도 `grpc-dotnet`, `zlink-dotnet`, `zlink-framework-dotnet` 순서다. 같은 payload
크기와 같은 active duration에서 세 구현을 같은 패턴으로 실행하므로, 표에서는 한 패턴 아래에
세 구현이 나란히 출력된다.

## 2. 측정 패턴

처음 범위는 두 payload 크기와 세 패턴만 사용한다. payload 크기는 `1024`, `4096` bytes를
기본값으로 한다. payload 크기는 protobuf `bytes body` 또는 raw ZLink message body의 전체
크기다. 앞 29 bytes는 측정 header로 사용하고, 나머지를 business payload 영역으로 채운다.
gRPC HTTP/2 frame, protobuf field overhead, ZLink envelope, ZMP header는 이 크기에 포함하지
않는다.

| 패턴 이름 | gRPC .NET | ZLink binding .NET | ZLink framework .NET | 해석 |
|-----------|-----------|--------------------|----------------------|------|
| `request-serial` | unary RPC | raw request callback | `RequestToChannel(...).Async<TReply>()` | 요청 하나를 보내고 reply 완료 뒤 다음 요청을 보낸다 |
| `request-window` | unary RPC | raw request callback | `RequestToChannel(...).Async<TReply>()` | 최대 `request_window`개의 미완료 request를 유지한다 |
| `send-saturation` | unary RPC returning empty reply | raw `Dealer.Send().Submit()` | `SendToChannel(...).Submit()` | reply payload가 없는 command/send 경로를 비교한다 |

`request-serial`은 한 요청의 왕복 지연이 처리량을 결정한다. 이 값은 “한 번에 하나만 처리하는”
사용 패턴의 비용을 보기 위한 값이다.

`request-window`는 ZLink request가 reply를 기다리지 않고 다음 request를 제출할 수 있다는 점을
반영한다. client는 active phase 동안 최대 `request_window`개의 미완료 request를 유지한다.
reply 하나가 도착하면 해당 slot에서 다음 request를 바로 보낸다. 기본 `request_window`는 `100`이다.

`send-saturation`은 request/reply와 섞어 평균 내지 않는다. gRPC empty unary는 empty reply까지
기다리지만, ZLink send는 reply 없는 제출 경로다. 이 패턴은 command/send 계열의 상대 비용을
따로 보기 위한 항목이다.

## 3. 실행 조건

- client process 1개와 비교 대상별 server process 1개로 실행한다.
- 로컬 runner는 gRPC server, ZLink raw binding server, ZLink framework server를 각각 띄운다.
- loopback 주소(`127.0.0.1`)만 사용한다.
- Release build로 실행한다.
- warmup 뒤 정해진 시간의 measured active 구간을 실행한다.
- 기본 payload 크기는 `1024,4096` bytes다.
- 기본 `request_window`는 `100`이다.
- 기본 send concurrency는 `8`이다.
- gRPC와 ZLink framework는 같은 protobuf DTO를 사용한다. ZLink raw binding은 같은 bytes payload를
  protobuf envelope 없이 보낸다.
- ZLink는 location store 없이 manual endpoint 연결을 사용한다.
- ZLink raw binding의 request echo endpoint와 command 수신 endpoint는 분리한다. command 측정에서
  reply 없는 단방향 수신량을 보려면 request echo reply가 같은 socket에 섞이면 안 되기 때문이다.
- TLS, compression, service mesh, gateway, broker는 사용하지 않는다.

## 4. 출력 형식

report 표는 패턴별로 묶고, 각 행에 구현 이름을 표시한다. 예시는 아래 형식이다.

```text
  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |       10.00 KOPS |   10.24 MB/s |     1.000 ms |     2.000 ms |     3.000 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
      | zlink-dotnet            | 1024B    |       30.00 KOPS |   30.72 MB/s |     0.300 ms |     0.600 ms |     0.900 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
      | zlink-framework-dotnet  | 1024B    |       20.00 KOPS |   20.48 MB/s |     0.500 ms |     0.900 ms |     1.200 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
```

보고서와 콘솔 출력은 perf runner에서 다루기 쉽게 metric별 `RESULT,current,...` 형식도 같이
남긴다. 한 행의 필드는 아래 순서다.

```text
RESULT,current,<scenario>,local,<payload_size>,<metric>,<value>
```

예시는 아래와 같다.

```text
RESULT,current,grpc-dotnet-request-window,local,1024,throughput,10000.000
RESULT,current,zlink-dotnet-request-window,local,1024,latency,0.300
RESULT,current,zlink-framework-dotnet-request-window,local,1024,latency_p99,1.200
```

`metric` 값은 `throughput`, `bandwidth`, `latency`, `latency_p95`, `latency_p99`,
`client_cpu_percent`, `client_memory_mb`, `server_cpu_percent`, `server_memory_mb`를 사용한다.
`throughput`의 raw 값은 초당 완료 수이며, 표에서는 `KOPS` 또는 `KMSG/s`로 나누어 표시한다.

## 5. 메트릭

필수 출력은 아래 메트릭이다. 처리량 단위는 패턴 성격에 맞춰 분리한다.

| 메트릭 | 의미 |
|--------|------|
| `Throughput` | measured 구간 처리량. 표에서는 `10.000 KOPS`, `183.618 KMSG/s`처럼 값과 단위를 한 칸에 함께 표시 |
| `Bandwidth` | payload 크기와 처리량으로 계산한 전송량. `MB/s`로 표시 |
| `Lat.Mean(ms)` | 평균 latency. request/reply는 client 왕복 latency, send는 server가 header로 계산한 수신 latency |
| `Lat.P95(ms)` | p95 latency |
| `Lat.P99(ms)` | p99 latency |
| `Client CPU` | client process가 active 구간 동안 사용한 CPU 비율 |
| `Client Mem` | client process working set |
| `Server CPU` | 해당 구현의 server process가 active 구간 동안 사용한 CPU 비율 |
| `Server Mem` | 해당 구현의 server process working set |

`request-serial`과 `request-window`는 echo reply가 돌아온 완료 수를 기준으로 `KOPS`를 계산한다.
여기서 `1 KOPS`는 초당 1,000건의 request/reply 완료를 뜻한다.

`send-saturation`은 server가 active phase에서 받은 메시지 수를 기준으로 `KMSG/s`를 계산한다.
여기서 `1 KMSG/s`는 초당 1,000개 메시지를 뜻한다. ZLink send는 reply를 기다리지 않으므로
client의 `Submit()` 호출 수만으로 처리량을 계산하지 않는다.

## 6. 측정 payload header

측정 payload 앞에는 `bindings/c/perf`의 metric header와 같은 의미의 29-byte header를 넣는다.
이 header는 payload 본문 일부이며, protobuf `bytes body` 또는 raw ZLink message body의 앞부분에
들어간다.

| offset | 크기 | 값 |
|--------|------|----|
| 0 | 4 | magic `0x5A4C4E4B` (`ZLNK`) |
| 4 | 4 | run id |
| 8 | 1 | phase (`0` warmup, `1` active) |
| 9 | 4 | payload size |
| 13 | 8 | sequence |
| 21 | 8 | send timestamp ns |

`request-serial`과 `request-window`는 reply payload에 돌아온 header를 client가 검증하고,
active phase reply 수로 `KOPS`를 계산한다. `send-saturation`은 server가 header를 읽어 active
phase message 수와 server-side 수신 latency를 계산한다. 이 방식은 payload 안에 측정 값을 넣어,
client stopwatch만으로 단방향 처리량을 과장하지 않기 위한 기준이다.

## 7. 결과 해석

이 bench는 작은 로컬 비교 도구다. 결과 문서나 guide에 성능 우위 문장을 넣으려면 아래 정보를
함께 남긴다.

- CPU, OS, .NET SDK version
- commit hash
- payload size
- warmup과 active duration 설정
- gRPC와 ZLink endpoint
- request window 값
- send concurrency 값
- 결과 JSON 원본

성능 판정은 gRPC가 아니라 같은 ZLink 계층끼리 비교한다. raw .NET binding은 같은 조건의
`zlink-c` request-window 결과를 기준으로 본다. raw .NET binding이 C 결과의 80% 이상이면
binding 계층의 기본 성능은 통과로 판단한다. framework .NET은 raw .NET binding 결과를 기준으로
본다. framework가 raw .NET binding 결과의 80% 이상이면 framework 추가 비용은 통과로 판단한다.

이 기준은 request-window처럼 ZLink가 reply를 기다리지 않고 다음 request를 보낼 수 있는 패턴을
중심으로 적용한다. `request-serial`은 한 번에 하나만 처리하는 사용 패턴의 왕복 지연을 보기 위한
보조 지표로 남긴다.

로그 단위는 runner마다 다를 수 있다. `bindings/c/bench/with_grpc`의 C report는 `throughput`
값을 KOPS로 남기고, .NET with-grpc report는 `RESULT` 라인의 `throughput` 값을 초당 완료 수로
남긴다. C와 .NET을 직접 비교할 때는 .NET 값을 1000으로 나누어 KOPS로 맞춘 뒤 비율을 계산한다.

결과는 "이 조건에서 더 빠르다/느리다"로만 해석한다. 일반적인 운영 성능이나 모든 payload에서의
우위를 주장하지 않는다.
