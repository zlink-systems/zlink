# #4350 .NET L0 결과

## 판정 요약

- 실험 branch `exp/4350-dotnet-ladder`, 최종 commit `40dc14b84f17b0aa44822eb2705c3e357c1940f0`; worktree clean. merge-base는 `494c5f4827db8d86a5dce716b7a8da6e7e9cf970`이다.
- Release solution 빌드는 이 worktree에서 성공했다(오류 0, 경고 25). DLL SHA-256과 각 측정 전 HEAD/base 기록은 `progress.md`에 있다. `/root/zlink` 바이너리는 사용하지 않았다.
- L0 및 binding 본 측정은 6개 셀 × 3회 모두 `errors=0`, `server_errors=0`, `abandoned=0`, 최종 `submitted=completed=received`였고 G5도 통과했다. 숫자 원본은 `raw/l0/bench/dotnet/run{1,2,3}/`, `raw/binding/bench/dotnet/run{1,2,3}/`, 집계는 `raw/aggregates/l0-v-binding.json`이다.
- 0.95 기준은 5/6 셀 통과했다. `request-backpressure × 1 KiB`는 0.946으로 기준 미달이다. per-message 서버 할당량은 L0와 binding이 각각 약 1456.1 B/receive로 같았다. 아래 비용 항목을 참고한다.
- main control은 이번 L0 단계에서 재측정하지 않았다(PM 결정). main 값은 commit `d3fd955131`의 `r4190w` 11-12 기준 자료를 사용했다. 기존 `/root/zlink`의 commit `10f63acaf8`에서 측정한 `raw/control-driver.log`는 잘못된 control로 보존만 하고 사용하지 않았다. 이번 단계에서 main 바이너리 SHA는 없다.

## L0 / binding / main 처리량 비교

처리량은 msg/s이며 request 패턴은 완료 request/s다. L0/binding은 같은 세션의 3회 중앙값, main/binding은 기준 보고서의 중앙값이다. L0 판정은 task의 `L0 ≥ 0.95 × same-session binding` 기준이다.

| 패턴 | payload | L0 | binding | L0/binding | L0 ≥ 0.95 | main | main/binding | 원자료 |
|---|---:|---:|---:|---:|---|---:|---:|---|
| request-serial | 1 KiB | 11,139.4 | 10,476.8 | 1.063 | 통과 | 6,352.4 | 0.560 | `raw/aggregates/l0-v-binding.json`; `r4190w/raw/aggregates/dotnet.json`; L0/binding 각 `run{1,2,3}/zlink{,-framework}-dotnet-request-serial-1024/results.json` |
| request-serial | 4 KiB | 9,666.8 | 9,597.2 | 1.007 | 통과 | 5,980.8 | 0.585 | 위 집계 파일; run별 `request-serial-4096/results.json` |
| request-backpressure | 1 KiB | 104,785.2 | 110,758.0 | 0.946 | **미달** | 46,722.0 | 0.452 | 위 집계 파일; run별 `request-backpressure-1024/results.json` |
| request-backpressure | 4 KiB | 105,690.8 | 102,162.0 | 1.035 | 통과 | 39,504.6 | 0.380 | 위 집계 파일; run별 `request-backpressure-4096/results.json` |
| send-saturation | 1 KiB | 454,832.8 | 460,568.8 | 0.988 | 통과 | 194,056.8 | 0.434 | 위 집계 파일; run별 `send-saturation-1024/results.json` |
| send-saturation | 4 KiB | 173,325.2 | 176,487.0 | 0.982 | 통과 | 140,020.8 | 0.751 | 위 집계 파일; run별 `send-saturation-4096/results.json` |

main/binding 각 숫자의 원본은 `/root/jobs/fx/cx/v21/s033/r4190w/raw/aggregates/dotnet.json`; run 원본은 `/root/jobs/fx/cx/v21/s033/r4190w/raw/bench/dotnet/run{1,2,3}/`의 `zlink-framework-dotnet-*` 및 `zlink-dotnet-*` 셀이다. 상세 기준은 `/root/jobs/fx/cx/v21/s033/r4190w/report.md`다.

## L0 cross test: send-saturation, 1 KiB

| server | client | 중앙값 (msg/s) | count/error gate | 결과 원자료 |
|---|---|---:|---|---|
| raw binding | raw binding | 458,019 | 3/3 유효, 오류 0, submitted=completed=received | `raw/cross-l0/bench/dotnet/raw-raw/run{1,2,3}/zlink-dotnet-send-saturation-1024/results.json` |
| Framework bench L0 server | raw binding client | 453,849 | 3/3 유효, 오류 0, submitted=completed=received | `raw/cross-l0/bench/dotnet/framework-raw/run{1,2,3}/zlink-dotnet-send-saturation-1024/results.json` |
| Framework bench L0 server | Framework client | 측정값 없음 | **실패**: source stats `http://127.0.0.1:5213`가 준비되지 않음. L0 target 로그에 `Protocol message contained an invalid tag (zero)`가 기록됨. 유효 throughput 결과 없음 | `raw/dotnet-cross-framework-framework-driver.log`; `raw/cross-l0/bench/dotnet/framework-framework/run1/zlink-framework-dotnet-send-saturation-1024/{target.log,source.log}` |

raw/raw 대비 Framework L0 server/raw client는 0.991배였다. Framework-client 조합은 현재 L0 direct binding receiver가 Framework route-mesh/service-wire startup 프레임을 처리하지 못해 시작되지 않았다. 이 실패를 throughput으로 계산하거나 0으로 대체하지 않았다. raw 결과는 보존했다.

## 꺼 둔 7개 기능과 L0 경로

각 기능은 L0 조건부 코드(`framework/bench/grpc/dotnet/Directory.Build.props:4-8`)에서 Framework route-mesh 경로를 실행하지 않아 꺼진다. 정상 Framework 경로 코드는 보존했다.

| ladder 기능 | L0에서 꺼 둔 항목 / 위치 | Framework 소유 경로 | 실제 사용 / 모방 |
|---|---|---|---|
| permit | 단일 binding owner가 바로 수신·처리: `ZLinkServer/Program.cs:43-58,83-134` | admission이 포함된 route dispatch: `ZLinkMeshNodeRouteDispatcher.cs:390-459` | `L0BenchHandler`는 실제 handler contract를 구현한다. Framework admission은 모방하지 않고 생략했다. |
| envelope header | raw wire의 최소 JSON header만 사용하고 Framework header decode를 생략: `ZLinkServer/Program.cs:101-107,132-133`; response header는 wire 호환성 때문에 유지 | Framework envelope decode: `ZLinkMeshNodeRouteDispatcher.cs:229-236` | response header를 새로 할당해 제출한다. Framework header validation/routing은 실행하지 않는다. |
| service wire framing | Framework mesh 미등록, binding Router를 endpoint에 직접 bind: `ZLinkServer/Program.cs:16-29,43-47` | Framework mesh dispatch entry: `ZLinkMeshNodeRouteDispatcher.cs:212-263` | 2-part binding wire를 사용한다. Framework service framing은 모방하지 않는다. |
| codec | `BenchPayload.Parser.ParseFrom` 및 `WriteTo` 직접 사용: `ZLinkServer/Program.cs:184-196`; Framework codec 등록 분기 생략: `:16-29` | protobuf codec serializer: `ZLinkProtobufCodec.cs:92-121`; 정상 등록은 `ZLinkServer/Program.cs:17-20` | 동일 protobuf 형식과 encode/decode를 직접 호출한다. codec registry는 모방하지 않는다. |
| owner queue·worker | 하나의 blocking receive owner가 `DontWait`로 drain: `ZLinkServer/Program.cs:49,83-134` | Framework mailbox queue: `ZLinkMeshOwnedMailbox.cs:61-102`, dispatch batch `ZLinkMeshNodeRouteDispatcher.cs:212-227` | binding owner를 실제 사용한다. Framework queue/worker는 생략했다. |
| activation·DI scope | handler를 한 번 직접 생성: `ZLinkServer/Program.cs:48`; `AddZLinkFramework` 및 route handler 등록은 `:16-29`에서 제외 | Framework registry/activation path는 정상 branch에서 실행 | `L0BenchHandler`는 실제 `IZLinkRouteRequestHandler`/`IZLinkRouteSendHandler` 계약을 구현한다. DI 활성화와 scope는 모방하지 않는다. |
| operation registry·completion | L0 server가 binding `ReplyToken`으로 직접 reply submit: `ZLinkServer/Program.cs:101-107`; L0 bench client는 binding transport 분기: `Client/Program.cs:167-170,375-400` | Framework route completion decoder: `ZLinkEnvelopeReplyCompletion.cs:3-24`; 정상 client route 호출: `Client/Program.cs:286-366` | binding submit/completion 경로를 사용한다. Framework operation registry/completion은 모방하지 않는다. |

### L0 reply parity와 남은 비용

- binding server: `ZLinkRawServer/Program.cs:74-80,103-108`. L0: `ZLinkServer/Program.cs:184-191,101-107`. 둘 다 request protobuf를 한 번 parse하고, request Body를 참조하는 새 payload wrapper를 만들고, 새 body Message를 할당해 protobuf encode 1회, response header Message를 할당하고 두 part를 submit한다. request body/message를 reply로 forwarding/reuse하지 않는다. L0의 `WriteTo`는 `RawWire.Encode`의 `payload.WriteTo(destination)`와 같은 구현이다.
- 남은 단일 기준 미달은 request-backpressure 1 KiB다(0.946). server 할당량은 L0 세 run에서 1456.102 / 1456.103 / 1456.112 B/receive, binding에서 1456.153 / 1456.166 / 1456.150 B/receive였다. 즉 추가 managed allocation은 관측되지 않았다. raw binding은 `ZLinkRawServer/Program.cs:74-76`에서 decode/record를 바로 수행하고 L0은 `ZLinkServer/Program.cs:109-112`에서 handler 호출을 거쳐 `:194-196`에서 decode/record한다. 이는 남아 있는 호출 경로 차이이며, 1 KiB send-saturation cross에서는 L0 server/raw client가 raw/raw의 0.991배였다. allocation 측정으로는 미달분의 CPU 원인을 확정할 수 없어, 정확한 hot frame은 미확정으로 남긴다.
- 변경 전/후 판정자 수: 이 branch는 공유 Framework runtime 결정 코드를 수정하지 않고 bench L0 분기만 추가해 1→1이다. 새 queue/lock/flag는 추가하지 않았다.
