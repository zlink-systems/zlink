# .NET send 프로파일 결과

## 1. 어떻게 쟀나

이는 throughput 판정이 아닌 EventPipe 프로파일링 실행이다. 실행 직전
`bash scripts/perf/perf-ticket.sh status`에서 실행 중·대기 ticket이 없음을 확인했다.
`dotnet-trace`와 `dotnet-counters` 10.0.745401, .NET SDK 8.0.131 및 runtime 8.0.31을
사용했다.

`framework/bench/grpc/dotnet/run_local.sh`의 source A 기동 인자와 같은
`send-saturation`, payload 1024, `request-window=100`, `send-concurrency=8`,
`raw-socket=router`, warmup 1000을 사용했다. runner는 target B 뒤에 source A를
`setsid dotnet run --no-build ... Client/WithGrpcBench.Client.csproj`로 기동한다
([run_local.sh](../../../framework/bench/grpc/dotnet/run_local.sh:343),
[run_local.sh](../../../framework/bench/grpc/dotnet/run_local.sh:369)). 이 실행에서는
source executable child PID에 붙일 수 있도록 동일 명령을 직접 기동하고, runner와 같은
`/bench/start` 요청을 보냈다.

```bash
# target B와 source A를 runner와 같은 --no-build/Release 인자로 기동
# warmup 요청 후 source /bench/stats 의 phase=idle을 확인
dotnet-counters monitor -p "$source_child_pid" \
  --counters System.Runtime --refresh-interval 1
dotnet-trace collect -p "$source_child_pid" --profile gc-verbose \
  --providers Microsoft-DotNETCore-SampleProfiler \
  --duration 00:00:10 -o /tmp/send-gc.nettrace
# trace 연결 1초 뒤 동일 cell의 active(durationMs=12000) 요청
```

trace의 처음 1초는 active 시작 전 idle이고, 나머지 약 9초가 active 구간이다. 따라서
idle에는 CPU 표본이 거의 없지만 allocation tick 총계와 counter는 10초 수집 구간 기준으로
기록했다. `gc-verbose`의 `AllocationTick`을 type 및 allocation-site call stack으로
집계했다.

| source A | active 12초 완료 수 | SampleProfiler event 수 | AllocationTick event 수 | trace AllocationTick 바이트 |
|---|---:|---:|---:|---:|
| `zlink-framework-dotnet` | 1,371,868 (114.3 KMSG/s) | 202,368 | 92,008 | 9,809,112,216 B |
| `zlink-dotnet` raw | 12,617,518 (1,051.5 KMSG/s) | 156,648 | 146,944 | 15,665,579,424 B |

원본은 `/tmp/zlink-active-framework.6AXcrB/send-gc.nettrace` 및
`/tmp/zlink-active-raw.jEDC2l/send-gc.nettrace`에 보존했다. 각 source의 종료 stats는
오류 0, peak in-flight 8이었다.

## 2. 시간이 어디로 가나

다음은 `dotnet-trace report ... topN`의 exclusive 비율이다. wait 계열도 표본에 나온
그대로 남겼다. 여러 ThreadPool worker를 함께 표본화했으므로, 이 비율은 중첩된 send
경로를 더해 만든 비용 분해가 아니다.

| source | 상위 표본 frame | exclusive |
|---|---|---:|
| framework | `LowLevelLifoSemaphore.WaitNative` | 28.10% |
| framework | `Monitor.Wait` | 15.43% |
| framework | `WaitHandle.WaitOneNoCheck` | 11.57% |
| framework | `Watcher.Interop.Sys.Read` | 7.72% |
| framework | `ManualResetEventSlim.Wait` | 5.42% |
| framework | `String.Concat` | 4.03% |
| framework | `Poller.Wait` | 3.80% |
| framework | `CompletionOwner.SendAsync` | 2.18% |
| framework | `Google.Protobuf.MessageExtensions.WriteTo` | 1.24% |
| framework | `Message.InitializeManagedCopy` | 0.83% |
| raw | `WaitHandle.WaitOneNoCheck` | 20.44% |
| raw | `CompletionOwner.RuntimePump` | 18.97% |
| raw | `Monitor.Wait` | 15.35% |
| raw | `LowLevelLifoSemaphore.WaitNative` | 11.64% |
| raw | `BenchPayloads.CreateBytes` | 11.49% |
| raw | `RawBenchTransport.SendAsync` | 1.55% |
| raw | `CompletionOwner.SendAsync` | 0.37% |

framework 표본에서 경로상으로 관측된 inclusive frame은
`ZLinkSpotNodeRuntime.SendToNodeAsync` 6.63%,
`ZLinkManagedMeshNode.SendToNodeDirectAsync` 6.30%,
`ZLinkStateLane.RunAsync` 3.24%, `ZLinkFrameworkRuntime.GetMeshNodeRuntime` 3.05%였다.
이들은 서로 호출 관계여서 합산하지 않았다. raw 표본에는 해당 framework frame이 없고,
raw의 `RawBenchTransport.SendAsync` inclusive 비율은 17.09%였다.

## 3. 할당이 어디서 나나

`System.Runtime` counter의 active 구간은 framework에서 allocation rate
1.025–1.042 GB/s, Gen0 68–71회/s, GC 시간 3–12%, monitor lock contention
3,845–4,264회/s였다. raw는 1.529–1.647 GB/s, Gen0 106–113회/s, GC 시간 1–4%,
contention 110–216회/s였다.

framework AllocationTick 상위 allocation site는 다음과 같다. byte 수는 10초 trace의
표본 합계이며 정확한 object allocation count가 아니라 GC allocation tick 표본 합계다.

| type | allocation site frame | 바이트 | 전체 tick 바이트 비율 |
|---|---|---:|---:|
| `System.Byte[]` | `Google.Protobuf.MessageExtensions.WriteTo` | 1.135 GB | 11.58% |
| `System.Byte[]` | bench source worker `Program...MoveNext` | 1.107 GB | 11.28% |
| `System.Threading.ExecutionContext` | `ExecutionContext.SetLocalValue` | 0.600 GB | 6.12% |
| `ThreeElementAsyncLocalValueMap` | `TwoElementAsyncLocalValueMap.Set` | 0.487 GB | 4.96% |
| `Systems.Zlink.Message` | `Message.From` | 0.473 GB | 4.82% |
| `Systems.Zlink.Message` | `Message.AllocateCoreValidated` | 0.406 GB | 4.14% |
| `SendCompletionEntry` | `CompletionOwner.SendAsync` | 0.327 GB | 3.34% |
| `ZLinkEnvelopeHeader` | generated `ZLinkEnvelopeHeader.Clone` | 0.305 GB | 3.11% |
| async state machine | `ZLinkSpotNodeRuntime.SendToNodeAsync` | 0.304 GB | 3.10% |
| async state machine | `ZLinkManagedMeshNode.SendToNodeDirectAsync` | 0.301 GB | 3.07% |
| async state machine | `ZLinkRouteSendCall.Async` | 0.288 GB | 2.94% |

raw AllocationTick의 큰 항목은 `BenchPayloads.CreateBytes`의 `System.Byte[]`
13.738 GB(87.65%), bench source worker의 `System.Byte[]` 1.252 GB(7.99%),
`CompletionOwner.SendAsync`의 `SendCompletionEntry` 0.437 GB(2.79%)였다.

## 4. raw와 무엇이 다른가

동일한 방식의 active 12초 비교에서 raw는 framework보다 9.20배 많은 메시지를
완료했다. 총 allocation rate만으로 이 차이를 설명할 수 없다. raw의 rate는 framework보다
높았지만, active 약 9초와 각 active 처리량으로 나눈 trace AllocationTick은 framework
약 9.5 KiB/message, raw 약 1.6 KiB/message이다.

raw의 큰 할당은 bench가 raw body를 만드는 `BenchPayloads.CreateBytes`에 집중되어 있다.
framework에는 그 대신 protobuf `WriteTo`, `Message.From`/`AllocateCoreValidated`,
`ExecutionContext`/`AsyncLocalValueMap`, envelope clone 및 세 framework async state-machine
site가 표본에 나타났다. CPU 표본에서도 framework에만 route lookup, spot send, managed
mesh send의 중첩 경로가 관측됐다. 다만 topN은 중첩 frame을 포함하므로 이 관측치로 각
경로의 독립적인 9.20배 기여도를 계산하지 않았다.

## 5. 고칠 후보

아래는 구현 제안이 아니라, 표본에 나온 위치만 다음 조사 대상으로 적은 것이다.

1. `ZLinkRouteClient.Async`의 route lookup 및 spot send 경로:
   [ZLinkRouteClient.cs:305](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:305),
   [ZLinkSpotNodeRuntime.cs:483](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeRuntime.cs:483),
   [ZLinkFrameworkRuntimeSpots.cs:254](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeSpots.cs:254).
   inclusive 표본은 각각 spot send 6.63%, route lookup 3.05%였고, 이 경로의 async
   state machine/lookup delegate allocation이 0.304 GB, 0.204 GB 이상으로 나타났다.

2. envelope header clone 및 encode:
   [ZLinkEnvelopeCodec.cs:70](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:70),
   [ZLinkEnvelopeCodec.cs:886](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:886).
   header clone은 0.305 GB(3.11%)를 차지했고 encode frame도 exclusive 0.21%로
   관측됐다.

3. framework protobuf body serialization:
   [ZLinkProtobufCodec.cs:82](../../../framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:82).
   `WriteTo` frame은 exclusive 1.24%, 그 site의 `System.Byte[]` tick은 1.135 GB
   (11.58%)였다. raw에는 이 serializer frame이 없다.

4. writable-retry completion 경로:
   [CompletionOwner.cs:75](../../../bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:75).
   `CompletionOwner.SendAsync`는 framework exclusive 2.18% 및 `SendCompletionEntry`
   0.327 GB(3.34%)로 관측됐다. 이 위치는 `Ok`에서 entry를 만들지 않고
   `Task.CompletedTask`를 반환하는 경로([CompletionOwner.cs:66](../../../bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:66))가 아니라,
   profiler가 실제로 본 writable-retry 분기다.

## 6. 확인 못 한 것

- `Missing Symbol`은 framework 3.86%, raw 5.12%였다. native/C++ 쪽의 해당 표본은
  source line으로 귀속하지 못했다.
- `String.Concat`은 framework exclusive 4.03%로 보였지만 EventPipe topN 결과만으로는
  호출 source line을 특정하지 못했다. 후보 목록에 넣지 않았다.
- SampleProfiler의 nested frame 비율은 호출 횟수나 메시지 하나의 독립 비용이 아니다.
  이 결과만으로 특정 후보의 개선량이나 9.20배 차이의 단일 원인을 확정하지 못했다.
- 이 실행은 profiler 부하가 있는 12초 수집이며 perf ticket을 통한 throughput 판정이 아니다.
  따라서 결과의 KMSG/s는 표본 구간의 경로 비교용으로만 기록했다.
