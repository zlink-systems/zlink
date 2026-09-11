# .NET Multi DEALER_DEALER 메시지당 비용의 함수별 귀속 지도

감독자가 고정 Core 0.17.2의 .NET/C DD 비용을 판단하기 위한 측정 기록이다. 구현 변경·후보 제안·채택/기각 판정은 포함하지 않는다.

**64 B의 managed 할당은 112.010 B/msg이며, 객체 크기 80 B의 `SocketSendOperation`과 32 B의 `TwoMessageReadOnlyList`가 정상 전송당 각각 한 번 생성된다.**
GC pause는 EventPipe 실행에서 0.0469%, 최종 CPU+GC 실행에서 0.3039%였다. 65536 B의 최종 할당은 265.492 B/msg, 관측 GC는 0회였다.
**64 B에서 분리해 식별한 P/Invoke 비용은 send 전환 helper보다 message init-size·close 경계에 많다.** 구체적인 함수별 수치는 §4에 있다.

## 집계 경계와 합계의 의미

주표의 **ns는 기존 r1net throughput에 정규화한 CPU 기반 환산값**이다. 함수별 wall-clock을 직접 재서 합산한 값은 아니다.
합계 일치는 아래 식으로 만든 회계적 일치이며, 그 자체가 각 함수의 wall-clock 기여도를 검증하지 않는다.

- `N`은 .NET의 기존 `sent` counter, C의 native FINAL 성공 counter다. 서버 수신 수나 throughput×duration을 정규화 분모로 쓰지 않는다.
- `D`는 계측한 client 구간의 wall ns다. .NET은 send phase 시작부터 모든 send task 완료까지다. C는 START 전달부터 CLIENT_DONE까지로, 기존 retained-send drain·stop token도 포함한다.
- `P_i`는 그 구간에서 해당 함수 문맥에 귀속한 application thread의 `cpu-clock` sample period 합(ns)이다. 한 CPU sample은 한 행에만 들어간다. inclusive를 중복 합산하지 않는다.
- 실측 CPU 비용은 `P_i/N`. profile client wall은 `D/N`. 기준값 `W = 1e9 / 기존 r1net throughput`이다.
- **주표 환산 비용 = `W × (P_i/N)/(D/N) = W × P_i/D`.** 프로파일 실행의 RESULT로 기존 성능 판정을 바꾸지 않는다.
- GC 행은 `W × GC_pause/D`. 계측 전용 CPU는 함수 행에서 제외해 미귀속 잔여에 넣었다.
- **미귀속 잔여 = `W − Σ 함수 환산값 − GC 행`.** 대기 시간, application thread끼리의 병렬 실행 중복, 계측 CPU, 경계 차이, 표본 오차가 섞인 순차액이다. 이를 특정 함수나 순수 대기 시간으로 해석하지 않는다.
- Core I/O thread는 application thread와 병렬이므로 주표에 더하지 않는다. process 전체 CPU와 application CPU를 §6에서 따로 제시한다. .NET의 application thread는 해당 구간에 managed application stack이 관측된 thread와 main thread다. C는 main thread다. 제외한 native worker TID와 근거 stack은 raw map/TSV에 있다.
- .NET 64 B는 application thread 1개, 4096 B는 41개, 65536 B는 36개에서 CPU 표본이 있었다. 큰 크기에서는 **각 함수의 CPU가 critical path wall에 얼마나 겹치는지는 미분리**다.
- Release 인라인 명령은 관측된 상위 함수에 남긴다. `PerfSocketIo.SendMeasurementAsync` self에는 builder 생성·메시지 연결·동기 Task terminal이 함께 포함된다. `SendAsync` self의 lock·검증·context를 가상 함수로 나누지 않았다.
- 원래 EventPipe CPU 자료의 .NET 64 B 전송 thread 표본 4,538개와 stdin control thread 표본 4,538개를 분리했다. 65536 B의 `RuntimePump` 표본 78.20%는 동시 대기까지 포함한다. **이 EventPipe 비중을 CPU나 wall 비중으로 사용하지 않았다.** 주표는 추가 Linux CPU 표본으로 작성했다.

¹ CPU→기준 wall 환산값이다. 순수 P/Invoke 전환의 인라인 명령, caller가 복원되지 않은 표본, 큰 크기의 동시 실행 기여는 완전히 분리하지 못했다.
² managed allocation만 나타낸다. AllocationTick의 interval byte weight로 호출자별 **추정**을 내고, `GC.GetTotalAllocatedBytes(true)` 차이를 정확한 합계로 사용한다. native malloc bytes는 측정하지 않았다. C의 0 B는 managed heap이 없다는 뜻이며 native allocation이 0이라는 뜻이 아니다.
³ 미귀속 잔여를 다른 행에 비례 배분하지 않았다. `0.000`은 관측 표본 0개를 포함하며 실제 비용 0을 증명하지 않는다.

## 1. 64 B — .NET과 C

기준 원본: .NET **922,799.8 msg/s → 1,083.659 ns/msg**, C **1,741,225.4 msg/s → 574.308 ns/msg**. 차이는 **509.350 ns/msg**다.

### .NET

| 항목 | 함수/경로 | 환산 ns/msg¹ | 비중 | managed alloc B/msg² | GC |
|---|---|---:|---:|---:|---|
| native send 본체 | `zlink_send_part`와 Core 하위 호출 | 440.163 | 40.618% | 0.000 | — |
| native send P/Invoke 경계·인자 준비 | `JIT_InitPInvokeFrame` → `SendPartSubmitter.Submit` 및 해당 managed 함수 self | 8.244 | 0.761% | 0.000 | — |
| Message wrapper 생성·소멸 | `Message.AllocateCoreValidated`, `CopyTo`, pool·Dispose의 분리된 managed 호출 | 76.145 | 7.027% | 0.000 | — |
| native message 저장소·copy·close | `zlink_msg_init_size/init/copy/close` 하위 호출; malloc 포함 | 90.896 | 8.388% | 0.000 | — |
| message helper P/Invoke 경계 | `InlinedCallFrame::Init`, `JIT_InitPInvokeFrame`, message IL stub self | 137.537 | 12.692% | 0.000 | — |
| 기타 P/Invoke·marshalling 경계 | 분리된 기타 IL stub·전환 helper self | 0.217 | 0.020% | 0.000 | — |
| 2-part staging·collection | `RequestReplySupport`, `OperationMessageBuffer`; native 호출과 unwind 제외 | 17.138 | 1.581% | 31.331 | — |
| send builder·runner helper | `SocketSendOperation`, `PerfSocketIo.SendMeasurementAsync`; 인라인된 builder·동기 terminal 포함 | 177.671 | 16.395% | 80.511 | — |
| async 제출·lock·context | `CompletionOwner.SendAsync`; 인라인된 lock·검증·NextContext 포함 | 48.377 | 4.464% | 0.000 | — |
| async terminal·재개 | 분리된 Task·awaiter·state machine 함수 | 0.651 | 0.060% | 0.000 | — |
| completion/token 조회 | 분리된 `SendCompletionEntry`·등록·완료 함수 | 0.000 | 0.000% | 0.000 | — |
| poller wait·drain의 CPU 실행 | `zlink_poller_wait`, `zlink_completion_*`, `RuntimePump/DrainRuntime/DrainCore` | 0.000 | 0.000% | 0.000 | — |
| CLR throw·stack unwind | `ExceptionTracker`, `RaiseTheException`, `_Unwind_*` 등이 있는 호출 문맥 | 0.434 | 0.040% | 0.000 | — |
| runner loop·clock·통계 | `SendLoopAsync.MoveNext`, C `run_single_size_case`, clock 등 | 25.815 | 2.382% | 0.000 | — |
| 나머지 CLR·JIT·미해석 CPU | 이름이 해석된 나머지 CLR/JIT 및 caller가 복원되지 않은 CPU 표본 | 52.065 | 4.805% | 0.000 | — |
| GC pause | `GC.GetTotalPauseDuration`의 구간 차이 | 3.293 | 0.304% | 0 | Gen0/1/2 = 1/0/0 |
| **미귀속 잔여³** | wall − application CPU − GC pause; 계측 CPU 포함 | **5.014** | **0.463%** | 0.168 | — |
| **합계** | **기존 r1net 1/throughput** | **1,083.659** | **100.000%** | **112.010** | — |

### C

| 항목 | 함수/경로 | 환산 ns/msg¹ | 비중 | managed alloc B/msg² | GC |
|---|---|---:|---:|---:|---|
| native send 본체 | `zlink_send_part`와 Core 하위 호출 | 467.541 | 81.409% | 0.000 | — |
| native message 저장소·copy·close | `zlink_msg_init_size/init/copy/close` 하위 호출; malloc 포함 | 73.114 | 12.731% | 0.000 | — |
| C payload memcpy | runner 문맥의 `memcpy/memmove`; Core I/O thread 제외 | 0.805 | 0.140% | 0.000 | — |
| runner loop·clock·통계 | C run_single_size_case, clock·통계 등 | 30.924 | 5.385% | 0.000 | — |
| 나머지 native·미해석 CPU | 나머지 native 호출 및 caller 미해석 CPU | 1.265 | 0.220% | 0.000 | — |
| GC pause | managed runtime 없음 | 0.000 | 0.000% | 0 | 0/0/0 (managed GC 없음) |
| **미귀속 잔여³** | wall − application CPU − GC pause; 계측 CPU 포함 | **0.660** | **0.115%** | 0.000 | — |
| **합계** | **기존 r1net 1/throughput** | **574.308** | **100.000%** | **0.000** | — |

C에 managed Message wrapper·Task/TCS·ArrayPool·P/Invoke는 없다. C의 message 준비와 payload copy는 위 native message 및 memcpy 행에 들어간다.
함수별 원시 CPU-ns/msg와 sample 수: NET 64 application 함수 (`/tmp/zlink-dotnet-dd-cost-map/perf-dotnet-64.application-functions.tsv`), C 64 application 함수 (`/tmp/zlink-dotnet-dd-cost-map/perf-c-64.application-functions.tsv`).

## 2. 65536 B — .NET과 C

기준 원본: .NET **94,364.2 msg/s → 10,597.239 ns/msg**, C **162,350.8 msg/s → 6,159.502 ns/msg**. 차이는 **4,437.738 ns/msg**다.

### .NET

| 항목 | 함수/경로 | 환산 ns/msg¹ | 비중 | managed alloc B/msg² | GC |
|---|---|---:|---:|---:|---|
| native send 본체 | `zlink_send_part`와 Core 하위 호출 | 824.788 | 7.783% | 0.000 | — |
| native send P/Invoke 경계·인자 준비 | `JIT_InitPInvokeFrame` → `SendPartSubmitter.Submit` 및 해당 managed 함수 self | 19.082 | 0.180% | 0.000 | — |
| Message wrapper 생성·소멸 | `Message.AllocateCoreValidated`, `CopyTo`, pool·Dispose의 분리된 managed 호출 | 190.825 | 1.801% | 22.622 | — |
| native message 저장소·copy·close | `zlink_msg_init_size/init/copy/close` 하위 호출; malloc 포함 | 324.402 | 3.061% | 0.000 | — |
| message helper P/Invoke 경계 | `InlinedCallFrame::Init`, `JIT_InitPInvokeFrame`, message IL stub self | 118.735 | 1.120% | 0.000 | — |
| 기타 P/Invoke·marshalling 경계 | 분리된 기타 IL stub·전환 helper self | 74.210 | 0.700% | 0.000 | — |
| 2-part staging·collection | `RequestReplySupport`, `OperationMessageBuffer`; native 호출과 unwind 제외 | 154.780 | 1.461% | 28.691 | — |
| send builder·runner helper | `SocketSendOperation`, `PerfSocketIo.SendMeasurementAsync`; 인라인된 builder·동기 terminal 포함 | 277.756 | 2.621% | 85.016 | — |
| async 제출·lock·context | `CompletionOwner.SendAsync`; 인라인된 lock·검증·NextContext 포함 | 178.103 | 1.681% | 0.000 | — |
| async terminal·재개 | 분리된 Task·awaiter·state machine 함수 | 171.742 | 1.621% | 31.077 | — |
| completion/token 조회 | 분리된 `SendCompletionEntry`·등록·완료 함수 | 103.894 | 0.980% | 11.731 | — |
| poller wait·drain의 CPU 실행 | `zlink_poller_wait`, `zlink_completion_*`, `RuntimePump/DrainRuntime/DrainCore` | 1,795.874 | 16.947% | 28.687 | — |
| CLR throw·stack unwind | `ExceptionTracker`, `RaiseTheException`, `_Unwind_*` 등이 있는 호출 문맥 | 2,673.669 | 25.230% | 56.742 | — |
| runner loop·clock·통계 | `SendLoopAsync.MoveNext`, C `run_single_size_case`, clock 등 | 65.729 | 0.620% | 0.000 | — |
| 나머지 CLR·JIT·미해석 CPU | 이름이 해석된 나머지 CLR/JIT 및 caller가 복원되지 않은 CPU 표본 | 1,346.376 | 12.705% | 0.435 | — |
| GC pause | `GC.GetTotalPauseDuration`의 구간 차이 | 0.000 | 0.000% | 0 | Gen0/1/2 = 0/0/0 |
| **미귀속 잔여³** | wall − application CPU − GC pause; 계측 CPU 포함 | **2,277.274** | **21.489%** | 0.491 | — |
| **합계** | **기존 r1net 1/throughput** | **10,597.239** | **100.000%** | **265.492** | — |

### C

| 항목 | 함수/경로 | 환산 ns/msg¹ | 비중 | managed alloc B/msg² | GC |
|---|---|---:|---:|---:|---|
| native send 본체 | `zlink_send_part`와 Core 하위 호출 | 936.658 | 15.207% | 0.000 | — |
| native message 저장소·copy·close | `zlink_msg_init_size/init/copy/close` 하위 호출; malloc 포함 | 373.431 | 6.063% | 0.000 | — |
| poller wait·drain의 CPU 실행 | `zlink_poller_wait`, `zlink_completion_*`, `RuntimePump/DrainRuntime/DrainCore` | 638.406 | 10.365% | 0.000 | — |
| C payload memcpy | runner 문맥의 `memcpy/memmove`; Core I/O thread 제외 | 3,267.211 | 53.043% | 0.000 | — |
| runner loop·clock·통계 | C run_single_size_case, clock·통계 등 | 693.866 | 11.265% | 0.000 | — |
| 나머지 native·미해석 CPU | 나머지 native 호출 및 caller 미해석 CPU | 1.232 | 0.020% | 0.000 | — |
| GC pause | managed runtime 없음 | 0.000 | 0.000% | 0 | 0/0/0 (managed GC 없음) |
| **미귀속 잔여³** | wall − application CPU − GC pause; 계측 CPU 포함 | **248.697** | **4.038%** | 0.000 | — |
| **합계** | **기존 r1net 1/throughput** | **6,159.502** | **100.000%** | **0.000** | — |

.NET의 throw·unwind 분류는 `ExceptionTracker`, `RaiseTheException`, `_Unwind_*` 등이 있는 문맥의 배타적 CPU 표본이다.
`RequestReplySupport.SubmitPreservingOnFailure`의 실패 반환 처리와 `CompletionOwner.SubmitSend`의 catch 경로가 해당 stack에 나타난다.
backpressure exception의 **정확한 발생 횟수는 수집하지 않았다**. AllocationTick의 exception 표본을 예외 횟수로 바꾸지 않는다.

C의 65536 B FINAL은 **807,116회 = 성공 734,338 + backpressure 72,778 + 기타 실패 0**이다. 시도/성공은 1.099107회다.
이 재시도 비용은 위 C CPU 분자에 포함하며, 분모에는 성공 제출 734,338건을 썼다. .NET도 `sent=seq=488,733`을 분모로 사용한다.

## 3. GC와 할당의 호출자별 귀속

### 정확한 구간 합계

| 수집 | N | managed 총 할당(B) | B/msg | Gen0/1/2 | GC pause(ms) | pause/wall |
|---|---:|---:|---:|---|---:|---:|
| eventpipe 64 B | 4,314,754 | 483,302,264 | 112.011546 | 1/0/0 | 2.346000 | 0.046918% |
| eventpipe 65536 B | 452,515 | 120,041,832 | 265.277023 | 0/0/0 | 0.000000 | 0.000000% |
| perf 64 B | 4,373,725 | 489,900,824 | 112.009974 | 1/0/0 | 15.194000 | 0.303862% |
| perf 4096 B | 2,335,639 | 279,017,480 | 119.460876 | 0/0/0 | 0.000000 | 0.000000% |
| perf 65536 B | 488,733 | 129,754,856 | 265.492316 | 0/0/0 | 0.000000 | 0.000000% |

64 B GC pause의 0.0469~0.3039%는 **프로파일 실행 구간에서의 관측 범위**다. 원본 r1net 실행의 GC 비중은 당시 이벤트가 없어 직접 재구성하지 않았다.
65536 B의 GC 0회는 고정 5초 구간의 결과이며, 더 긴 실행에서도 0회라는 뜻이 아니다.

### 타입·호출자별 AllocationTick 추정

| 할당 타입 / 주 호출자 | 관측 object size(B) | 64 B 할당 추정 B/msg | 4096 B | 65536 B |
|---|---:|---:|---:|---:|
| `SocketSendOperation` / `DealerSocket.Send` | 80 | 80.511 | 85.042 | 85.016 |
| `SByte[]` / CLR stack trace 저장 | 72,120 | 0.000 | 1.319 | 29.349 |
| `Message` / pool·CloneParts | 88 | 0.000 | 1.228 | 22.622 |
| `TwoMessageReadOnlyList` / `OperationMessageBuffer.Parts` | 32 | 31.331 | 26.918 | 21.951 |
| `ZlinkSubmitException` / `CreateSubmitException` | 144 | 0.000 | 0.909 | 19.786 |
| `Task` / TCS·runtime pump | 64 | 0.000 | 0.728 | 17.381 |
| `AsyncStateMachineBox1[System.Threading.Tasks.VoidTaskResult,PerfSocketIo+<AwaitMeasurementSendAsync>d__13]` | 128 | 0.000 | 0.591 | 13.044 |
| `ZlinkPollerEvent[]` / `RuntimePump` | 72 | 0.000 | 0.228 | 12.382 |
| `SendCompletionEntry` / `CompletionOwner.SendAsync` | 112 | 0.000 | 0.682 | 11.731 |
| closure / `StartRuntimePump` | 32 | 0.000 | 0.136 | 8.472 |
| `Action` / runtime pump 등록 | 64 | 0.000 | 0.409 | 7.832 |
| `String` / submit exception 생성 | 80 | 0.000 | 0.454 | 7.606 |
| `Message[]` / CloneParts·pool | 40,2072 | 0.000 | 0.091 | 4.785 |
| array enumerator / retained parts 처리 | 32 | 0.000 | 0.227 | 1.956 |
| `TaskCompletionSource` / entry 생성 | 24 | 0.000 | 0.182 | 0.652 |
| `System.Int32` | 24 | 0.000 | 0.000 | 0.218 |
| `System.Object[]` | 152 | 0.000 | 0.091 | 0.217 |
| `System.GCMemoryInfoData` | 288 | 0.000 | 0.045 | 0.000 |
| AllocationTick에 귀속되지 않은 차액 | — | 0.168 | 0.181 | 0.491 |
| **정확한 total allocated 합계** | — | **112.010** | **119.461** | **265.492** |

AllocationTick은 모든 객체의 allocation event가 아니다. 약 100 KB 단위 byte weight를 그 tick의 타입·stack에 귀속한다.
예를 들어 65536 B의 `SocketSendOperation` 추정 85.016 B/msg와 list 추정 21.951 B/msg를 각각의 실제 객체 크기 80/32 B와 동일시하지 않는다.
정상 제출마다 두 객체가 각각 하나씩 생성되는 소스 경로의 고정 합은 **112 B/msg**다. 64 B 실측과의 차액은 **43,624 B / 4,373,725 = 0.009974 B/msg**이며, cold pool·구간 준비·계측용 EventSource 초기화 등이 합쳐진 값이다.
모든 allocation caller와 byte weight는 `perf-dotnet-{size}.alloc.json`의 `callers`에 보존했다. 표의 주 호출자는 대표 경로이며 전수 호출자를 대체하지 않는다.

소스 근거: [DealerSocket.Send](../../../../../bindings/dotnet/src/Zlink/Runtime/Sockets/DealerSocket.cs), [SocketSendOperation](../../../../../bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs), [OperationMessageBuffer.Parts](../../../../../bindings/dotnet/src/Zlink/Runtime/Messaging/OperationMessageBuffer.cs), [Message pool](../../../../../bindings/dotnet/src/Zlink/Runtime/Messaging/Message.Pool.cs).

### 이전 pass와의 경계 대조

- 이전 pass의 **수신 72 B/msg**는 DD/PUBSUB 수신 collection 경로다. 이번 측정은 DD **client 송신**이므로 112.010 B/msg와 같은 항목이 아니다. server 수신 allocation은 이번 job에서 계측하지 않았다.
- 이전 **REQREP 1,024→640 B/op**는 request/reply 양쪽 경로의 최소 API 계측이다. 이번 DD client 64 B에서 Task/TCS allocation tick은 **0개**, 65536 B에서는 Task **80개**, TCS **3개**가 관측됐다. tick 수를 총 Task 생성 수로 해석하지 않는다.
- DD는 성공 admission에 `Task.CompletedTask`를 반환하며, writable token이 있는 backpressure에서 `SendCompletionEntry`를 만든다. 따라서 REQREP의 640 B를 DD 비용으로 전용하지 않았다.
- 원본: [dotnet-perf-pass2-summary](../../../../plan/c016-worklog/dotnet-perf-pass2-summary.md). 72/640 B는 historical 수치이며 현재 binary의 재측정값이 아니다.

## 4. P/Invoke 경계

Ubuntu 런타임 **8.0.30-0ubuntu1~24.04.1**의 native debug symbol을 사용했다. `libcoreclr.so`와 debug file의 Build ID는 둘 다 **397d78d5ce9ead084dc6d11237fad4499e6914b9**다.
아래는 native Core 함수 내부를 제외한 **식별된 helper/stub self**다. send adapter의 인라인 전환·인자 준비는 별도 행이다.

| 경계 / 함수 문맥 | 64 B CPU-ns/msg | 64 B 환산 ns/msg | 65536 B CPU-ns/msg | 65536 B 환산 ns/msg |
|---|---:|---:|---:|---:|
| send — InitPInvokeFrame → SendPartSubmitter | 2.518 | 2.386 | 6.144 | 6.361 |
| init_size — ZlinkMsg*, native uint IL stub | 86.969 | 82.436 | 45.059 | 46.646 |
| close — ZlinkMsg* int32 IL stub·ConsumeParts | 52.639 | 49.895 | 51.204 | 53.007 |
| copy — ZlinkMsg&, ZlinkMsg& IL stub | 5.493 | 5.206 | 18.433 | 19.082 |
| 기타 식별된 native 경계 | 0.229 | 0.217 | 71.685 | 74.210 |
| send adapter self — 인라인 전환·인자 준비 포함 | 6.179 | 5.857 | 12.289 | 12.722 |

`InlinedCallFrame::Init()` 자체의 64 B self는 **126.792 CPU-ns/msg**다. 이 함수의 다수 표본은 init-size·close IL stub 아래에 있었으며 native send 경계로 재분류하지 않았다.
식별된 P/Invoke helper/stub 외에도 managed 함수에 인라인된 GC mode 변경·frame push/pop이 남는다. **순수 전환 전체의 정확한 ns 및 그 인라인 명령별 분리는 미완료**다. 위 send adapter self 전체를 순수 전환이라고 부르지 않는다.

정상 2-part 송신의 소스상 native 호출은 **13회/msg**다: init_size 2, data 1, init 2, copy 2, send 2, close 4.
일반 GC transition 경로는 init_size/copy/send/close의 **10회/msg**, SuppressGCTransition 표시가 있는 init/data는 **3회/msg**다.
이는 성공·재시도 없는 경로의 호출 수이며, 65536 B의 retry·poller·completion 호출에는 적용하지 않는다.
근거: `NativeMethods.Core.cs:170-201`, `NativeMethods.Socket.cs:31-42`, `Message.Native.cs:97,192,293,350`, `RequestReplySupport.cs:219-254`.

Microsoft의 [CLR ABI 설명](https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/botr/clr-abi.md)은 per-frame InitPInvokeFrame과 per-call-site 전환을 구분한다. 이 구분에 따라 helper self와 managed 함수에 남은 인라인 작업을 합쳐 순수 전환으로 단정하지 않았다.

## 5. 크기에 따른 비중과 .NET에만 있는 경로

### 64 / 4096 / 65536 B 비교

| .NET 항목 | 64 B 환산 ns | 비중 | 4096 B 환산 ns | 비중 | 65536 B 환산 ns | 비중 |
|---|---:|---:|---:|---:|---:|---:|
| native send 본체 | 440.163 | 40.618% | 294.020 | 16.930% | 824.788 | 7.783% |
| native send P/Invoke 경계·인자 준비 | 8.244 | 0.761% | 5.432 | 0.313% | 19.082 | 0.180% |
| Message wrapper 생성·소멸 | 76.145 | 7.027% | 62.471 | 3.597% | 190.825 | 1.801% |
| native message 저장소·copy·close | 90.896 | 8.388% | 174.171 | 10.029% | 324.402 | 3.061% |
| message helper P/Invoke 경계 | 137.537 | 12.692% | 95.743 | 5.513% | 118.735 | 1.120% |
| 기타 P/Invoke·marshalling 경계 | 0.217 | 0.020% | 2.716 | 0.156% | 74.210 | 0.700% |
| 2-part staging·collection | 17.138 | 1.581% | 16.976 | 0.977% | 154.780 | 1.461% |
| send builder·runner helper | 177.671 | 16.395% | 219.327 | 12.629% | 277.756 | 2.621% |
| async 제출·lock·context | 48.377 | 4.464% | 41.760 | 2.405% | 178.103 | 1.681% |
| async terminal·재개 | 0.651 | 0.060% | 7.809 | 0.450% | 171.742 | 1.621% |
| completion/token 조회 | 0.000 | 0.000% | 4.414 | 0.254% | 103.894 | 0.980% |
| poller wait·drain의 CPU 실행 | 0.000 | 0.000% | 85.558 | 4.926% | 1,795.874 | 16.947% |
| CLR throw·stack unwind | 0.434 | 0.040% | 93.027 | 5.357% | 2,673.669 | 25.230% |
| runner loop·clock·통계 | 25.815 | 2.382% | 46.514 | 2.678% | 65.729 | 0.620% |
| 나머지 CLR·JIT·미해석 CPU | 52.065 | 4.805% | 112.719 | 6.490% | 1,346.376 | 12.705% |

4096 B의 기준 차이는 **212.165 ns/msg**로, 64 B의 **509.350**, 65536 B의 **4,437.738**보다 작다.
4096 B에서는 .NET의 init-size 경계 **52.625 ns**, close 경계 **36.328 ns**, throw·unwind **93.028 ns**, native poll/completion 및 managed drain **85.557 ns**가 관측됐다.
같은 크기의 C는 native message **382.089 ns**, native send **537.756 ns**, poll/completion **126.774 ns**였다. .NET의 대응 native message/send는 **174.171/294.020 ns**였다.
이는 **프로파일 비용 구성의 차이**이며 87.78%라는 원본 ratio의 인과 분해를 완료한 값은 아니다. 4096 B의 .NET 미귀속 잔여가 남고, 해당 원본 r1net의 실행 stack은 보존되어 있지 않다.
기준 latency는 .NET/C 각각 64 B **0.543048/0.081408 ms**, 4096 B **244.645636/717.976147 ms**, 65536 B **3.701712/7.060639 ms**다. 이 latency만으로 queue 원인을 결정하지 않는다.

### C에 없는 managed/P/Invoke 경로

다음은 .NET에만 있는 symbol·객체 경로의 환산값이다. 일부 준비·검증은 C runner에도 의미상 대응하므로 **각 행 전체를 C 대비 순증분으로 보지 않는다**.

| .NET 경로 | 64 B 환산 ns/msg | 65536 B 환산 ns/msg |
|---|---:|---:|
| 식별된 P/Invoke helper/stub | 140.141 | 199.306 |
| send adapter self | 5.857 | 12.722 |
| Message managed wrapper | 76.145 | 190.825 |
| managed 2-part staging/collection | 17.138 | 154.780 |
| SocketSendOperation·PerfSocketIo helper | 177.671 | 277.756 |
| CompletionOwner async 제출 | 48.377 | 178.103 |
| Task·awaiter terminal | 0.651 | 171.742 |
| completion entry 처리 | 0.000 | 103.894 |
| managed runtime pump/drain | 0.000 | 224.749 |
| CLR throw·unwind | 0.434 | 2,673.669 |

## 6. 조건·원본·정규화 분모

- branch `main`. r1net의 report commit는 `63ec6187d9`, 첫 EventPipe report는 `09f9ad72ec`, 최종 CPU+GC report는 `6cd4e71a0d`다. 다른 작업이 HEAD를 이동했으며 이 job은 commit하지 않았다. 비교 경계는 고정 Core·보존한 runner 및 binding binary hash다. 기존 tracked 변경 9개와 기존 untracked 항목을 보호했다. source 수정·Core 재빌드·commit/push는 0회다.
- Core: `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`; 실제 `libzlink.so.0.17.2`를 사용했다. C는 `bindings/c/build-release-0.17.2/perf/comp_src_dealer_dealer_{client,server}`다.
- `.NET SDK 8.0.130`, runtime `8.0.30`, Release, server/concurrent GC. Intel Core Ultra 7 265K, 20 CPU, WSL2 Linux 6.6.87.2.
- 모든 측정 전 `bash scripts/perf/wait-for-idle-perf.sh`를 실행했다. flock 대기 뒤 load > 5이면 다시 10초 간격으로 대기했다. 실행은 한 번에 한 쌍이었다. 각 `*.run.log`에 실제 시작 load를 남겼다.
- 고정 조건: tcp, DD, clients 100, duration 5 s, part-count 2, I/O threads 4/4, auto-HWM balanced, send/recv timeout 200 ms, connect/server ready 10000 ms, shutdown 5000 ms. drain으로 client 구간이 5초를 넘는 경우도 기존 코드의 결과이며 duration·timeout을 늘리지 않았다.
- .NET: `bash bindings/dotnet/perf/multi/run_benchmarks.sh --reuse-build --pattern DEALER_DEALER --transports tcp --msg-sizes SIZE --clients 100 --duration 5 --runs 1`. 64/65536 EventPipe와 64/4096/65536 CPU+GC 수집. 첫 64 B Linux CPU 실행은 default clock으로 수집돼 경계 정렬용 주표에서 제외하고 `--clockid mono`로 다시 수집했다.
- EventPipe: dotnet-trace **8.0.547301**, runtime provider `0x40000001:5`, custom `Zlink-DD-Cost`. 초기 2개 실행만 SampleProfiler를 추가했다. 최종 CPU 실행에는 EventPipe CPU sampler를 넣지 않았다.
- Linux CPU: perf **6.8.12**, `record --clockid mono -e cpu-clock -F 999 --call-graph dwarf,8192 -p PID`. 권한은 대상 PID sampling에만 사용했으며 kernel 설정을 바꾸지 않았다. managed symbol은 `DOTNET_PerfMapEnabled=3`의 perf map, CLR symbol은 일치하는 Ubuntu native dbgsym이다.
- C: 기존 binary에 `/tmp/count-send.so` 계수기를 LD_PRELOAD했다. 원래 `zlink_send_part`에 그대로 전달한 뒤 DONTWAIT FINAL의 성공/backpressure/기타 실패를 세고 종료 때 한 줄 출력한다. 성공 N과 retry 수를 분리하기 위한 임시 계측이며 Core 파일을 변경하지 않았다.

| 자료 | N: client 성공 제출 수 | 구간(s) | client CPU 전체 / N(ns) | application CPU / N(ns) | 전체 CPU/wall | profile server active 수 |
|---|---:|---:|---:|---:|---:|---:|
| dotnet 64 B | 4,373,725 | 5.000292 | 2,080.171 | 1,137.926 | 1.820x | 4,371,236 |
| dotnet 4096 B | 2,335,639 | 5.120374 | 3,213.898 | 1,605.878 | 1.466x | 2,335,639 |
| dotnet 65536 B | 488,733 | 5.003049 | 30,144.747 | 8,104.550 | 2.945x | 488,365 |
| c 64 B | 8,528,950 | 5.000741 | 1,156.046 | 585.652 | 1.972x | 8,528,950 |
| c 4096 B | 3,190,298 | 5.176228 | 3,793.722 | 1,230.583 | 2.338x | 3,190,285 |
| c 65536 B | 734,338 | 5.002794 | 25,357.016 | 6,537.590 | 3.722x | 733,226 |

`profile server active 수`는 RESULT×5를 **대조 목적으로만** 계산했다. client N과는 .NET 64 B 2,489건(0.057%), .NET 65536 B 368건(0.075%), C 65536 B 1,112건(0.151%) 차이가 있다.
주표 분모를 server 수신 수로 바꾸지 않았다. C 4096 B는 기존 drain 때문에 client 구간 **5.176228 s**이고, .NET 4096 B도 task drain을 포함한다.

### 원본 report와 재현 자료

- 기존 기준: NET r1net (`/home/hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_081958_r1net.txt`), C r1net (`/home/hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_081717_r1net.txt`). `/tmp`에도 baseline 사본을 보존했다.
- 측정·분석 자료 루트: `/tmp/zlink-dotnet-dd-cost-map`. `perf-{dotnet,c}-{64,4096,65536}.{data,script,map.json,application-functions.tsv,functions.tsv,stacks.tsv}`에 raw CPU와 배타적 귀속을 보존했다.
- GC·할당: `perf-dotnet-{size}.{nettrace,events.jsonl,alloc.json,client.log}`. 전수 caller와 allocation weight는 `alloc.json`에 있다. 초기 EventPipe는 `dotnet-{64,65536}.*`다.
- 실행: `measure-dotnet.py`, `measure-dotnet-perf.py`, `measure-c-perf.py`, `c-pair-perf.py`; 분석: `reader/Program.cs`, `analyze-perf.py`, `analyze-dotnet.py`, `make-report.py`. 정규화 식·범주 배정·sample별 stack을 함께 보존했다.
- `binaries.sha256`, `c-ldd.txt`, native debug Build ID가 binary 경계를 소유한다. `/tmp` 자료는 이 host의 임시 보존 자료다.

### 임시 계측·실패·원복

1. repository의 .NET/C source는 수정하지 않았다. `/tmp/runner`에 runner source를 복사하고 DD client 구간 시작·종료에 sent/seq/clock/GC counter 출력을 추가했다. binding/Common DLL은 기존 Release binary를 그대로 참조했다. `/tmp`의 runner와 도구만 build했다.
2. official `--reuse-build` runner를 사용하기 위해 ignored apphost를 일시적으로 wrapper로 바꿨다. server는 원본 복사본, client는 계측 runner를 실행했다. 매 실행의 finally에서 원래 apphost를 복원했다. 원본 source와 runtime DLL은 변경하지 않았다.
3. custom EventSource의 초기 WindowEnd payload overload는 long field가 잘못 해석된다. **모든 N은 COST_WINDOW의 sent/seq에서 읽었고**, EventSource는 구간 timestamp만 사용했다. raw event의 sent를 분모로 쓰지 않았다.
4. 최초 C callgrind 실행은 외부 harness가 server START를 전달하지 않아 client exit 0/server exit 1이었다. `brk segment overflow` 경고도 있었다. 실패 자료 (`/tmp/zlink-dotnet-dd-cost-map/failed-c-64-missing-server-start`)에 보존하고 전부 주표에서 제외했다. START 전달을 바로잡은 최종 C CPU 수집은 3개 크기 모두 client/server exit 0이다.
5. 첫 .NET 64 B Linux CPU 자료는 default perf clock과 Stopwatch clock 경계가 달라 별도 폴더 (`/tmp/zlink-dotnet-dd-cost-map/perf-64-default-clock`)에 보존했다. 최종 3개 크기는 `--clockid mono`다. 해당 보완 실행의 RESULT로 기준 throughput을 교체하지 않았다.
6. 최종 nettrace 3개와 초기 nettrace 2개 모두 reader의 `EventsLost=0`이다. perf 수집·해석 로그에서 lost/error 경고는 0건이었다. native debug symbol의 debuginfod 다운로드는 timeout, 같은 버전의 Ubuntu ddeb 다운로드는 성공했다. 처음 받은 `dotnet-runtime-dbg`는 managed PDB이고 native 해석에는 사용하지 않았다.

## 7. 남은 미분리 범위

- **순수 P/Invoke 전환 전체**: 식별된 helper/stub self는 분리했지만 JIT 인라인 GC mode 변경·frame 조작의 전체 instruction 경계는 분리하지 못했다.
- **큰 크기의 함수별 wall critical path**: application CPU에 병렬 실행이 있으므로 CPU 합을 wall 합으로 동일시할 수 없다. 주표의 미귀속 잔여는 이를 숨기지 않은 순차액이다. 합계가 W와 맞는 것은 정규화 식의 결과다.
- **native allocation bytes 및 정확한 동적 managed caller별 allocation 횟수**: 전자는 미측정, 후자는 AllocationTick 추정이다. GC total allocated와 Gen/pause만 정확한 구간 counter다.
- **4096 B 87.78%의 인과 설명**: 동일 크기의 CPU 구성·allocation·retry 관측은 확보했지만 기존 5-run ratio의 원인을 함수별 wall 기여로 확정하지 않았다.
- C++ 참조 문서는 REQREP의 Ir 지도이며 clients 4/duration 2/Core 0.17.1 문맥을 포함한다. 이번 DD clients 100/duration 5/Core 0.17.2 ns와 직접 환산·차감하지 않았다.

검증: counter/분모, 배타적 CPU 합, 주표의 W 합계, allocation 총합, Core·binding·apphost 원복 hash 확인을 통과했다. 검산 결과 (`/tmp/zlink-dotnet-dd-cost-map/verification.json`)에 수치와 hash 확인 결과를 보존했다. production 변경이 없으므로 기능 test·전체 gate는 실행하지 않는다.
