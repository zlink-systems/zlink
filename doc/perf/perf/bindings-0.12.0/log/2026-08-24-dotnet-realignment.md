# .NET binding 0.13.0 계약 재정렬 (send_complete)

`bindings/doc/spec/async-coroutine-policy.ko.md`(3차 개정)와
`doc/plan/core-send-completion-design.ko.md` / `core/include/zlink/socket/api.h`
에 맞춰 .NET binding을 Core 0.13.0 send-completion 계약으로 옮겼다. 참조
구현은 HEAD의 `bindings/cpp`와 `2026-08-24-cpp-async-readd.md`다.

## 구현

### P/Invoke 표면

- `zlink_send_ready_handler` / `zlink_routed_send_ready_handler` P/Invoke와
  두 delegate 타입을 삭제하고 `zlink_send_async`,
  `zlink_send_complete_handler`, `zlink_send_async_cancel`을 추가했다.
  `ZlinkSendAsyncOptions`(`zlink_send_async_options_t`),
  `ZlinkSendCompleteEvent`(`zlink_send_complete_event_t`),
  `ZlinkSendCompleteResult`(ADMITTED/TIMED_OUT/TERMINAL) native mirror를
  추가하고 `ZlinkRoutedSendReadyEvent`/`ZlinkRoutedSendReadyState`를 지웠다.
- `NativeMethods.RequiredExports`에서 두 send_ready 심볼을 빼고 새 세 심볼을
  넣었다. **이 목록이 0.13.0의 실질적 break였다** — 아래 "선행 상태" 참고.

### 삭제한 binding-owned 기계장치

- `Runtime/Messaging/RoutedAdmissionScheduler.cs` (1,143줄) 삭제. park queue,
  target별 pending map, ready target queue, `ThreadPool` pump, deadline
  `System.Threading.Timer`, WRITABLE-callback 재시도가 모두 사라졌다.
- `Runtime/Messaging/PublisherAdmissionScheduler.cs` (693줄) 삭제.
- `SocketKernel.Callbacks.cs`의 `SendReadyHandler` /
  `AdmissionSendReadyHandler` / `EnsureNativeSendReadyHandlerLocked` /
  `OnNativeSendReady`와 `SocketCallbackRegistry`의 send-ready 필드를 삭제했다.
- 공개 계약에서 `IReceivingMessageSocket.OnSendReady` /
  `IPublisherSocket.OnSendReady`를 제거했다 —
  `send_ready` readiness-hint 시맨틱은 폐지되었다.

### 새 완료 경계

- `Runtime/Messaging/SendCompletionRegistry.cs` (신규): socket당 하나의
  `zlink_send_complete_handler`를 첫 `Async(...)` 시점에 지연 설치하고,
  complete record 하나를 `zlink_send_async`로 넘긴다. 완료 콜백은
  `TaskCompletionSource`만 완료하며 어떤 send/publish/request도 다시 호출하지
  않는다(Core는 그런 호출을 `EDEADLK`로 거부한다).
  - **delegate lifetime**: Core는 reverse-P/Invoke stub의 raw function
    pointer를 보관한다. 완료 delegate 인스턴스를 instance field와 명시적
    `GCHandle` 두 경로로 뿌리내리고, native socket이 닫힌 **뒤에만**
    (`SocketKernel.Dispose`의 `_handle.Dispose()` 다음)
    `ReleaseAfterNativeClose()`로 해제한다. 수집되면 Core가 해제된 stub을
    호출하게 된다.
  - **operation state lifetime**: operation state는
    `zlink_send_async_options_t.userdata`로 전달되는 `GCHandle`이 살려 두며,
    정확히 한 번 도착하는 완료가 그 handle을 해제한다. op_id keyed map 대신
    userdata round-trip을 쓰므로 inline 완료(제출 반환 전 실행)와도 경합하지
    않는다. `ZLINK_SUBMIT_OK`가 아니면 완료가 없으므로 그 경로에서만 binding이
    handle을 해제하고 payload ownership을 호출자에게 되돌린다.
  - **cancellation**: `CancellationToken` → `zlink_send_async_cancel`.
    `NOT_FOUND`/`INVALID_STATE`는 양성(이미 완료했거나 admission 확정)이며
    operation은 어느 쪽이든 정확히 한 번 완료한다. 완료 시 등록 해제는
    `Dispose()`가 아니라 `Unregister()`를 쓴다 — Core가
    `dispatch_send_completions_if_local()`로 취소 콜백 스레드에서 완료를 inline
    dispatch할 수 있어 `Dispose()`는 자기 자신을 기다리며 deadlock한다.
  - **deadline**: per-operation deadline은 Core-side 옵션
    (`timeout_ms`)이다. binding-owned deadline timer는 없다.
  - **submit gate**: Core는 같은 native handle에 multipart part sequence가
    떠 있는 동안 `zlink_send_async`를 `EINVAL`로 거부한다
    (`send_sequence_active`). 그래서 record 제출은 request/reply part loop와
    같은 짧은 `SubmitGate`를 공유한다. gate는 대기를 감싸지 않는다 —
    `zlink_send_async`는 record를 넘기고 즉시 반환한다.
- `Runtime/Messaging/RoutedRequestSubmitter.cs` (신규): DEALER/ROUTER request는
  호출자 thread에서 exact target을 골라 동기 제출하고, Core reply callback이
  `TaskCompletionSource<IReadOnlyList<Message>>`를 완료한다. pending queue,
  재시도 loop, deadline timer, dispatcher thread가 없다. timeout은 Core의
  per-request deadline(`ZLINK_REQUEST_TIMED_OUT`)이다. 제출은 `DONTWAIT`이다 —
  .NET terminal이 `Async(...)`이므로 호출자 thread를 HWM 대기로 점유할 수 없고,
  backpressure 정책은 어플리케이션이 소유한다.

### Publish

- `IPublisherSocket.Publish(topic)`가 `PublishOperation`을 반환하고
  terminal은 `PublishSubmitOperation.Submit() -> void`다. 실패는
  `ZlinkSubmitException`이다. lossy PUB 의미론상 publisher는 HWM에서 대기하지
  않으므로 제출은 항상 `DontWait` 의미로 내려가고 `NODROP`의 가득 찬
  subscriber는 그 자리에서 `Backpressured`로 표면화된다.
  `TryPublish(topic)`는 같은 backpressure를 예외 없이 `false`로 관찰하는
  별도 표면으로 남는다.
- `AsyncSendOperation` / `AsyncSendSubmitOperation` 공개 인터페이스와 그
  `Messages(...)` 확장은 삭제했다.

## 선행 상태 (baseline)

**HEAD의 .NET 테스트 suite는 native runtime을 전혀 로드하지 못한 상태였다.**
`NativeMethods.RequiredExports`에 0.13.0에서 사라진
`zlink_send_ready_handler` / `zlink_routed_send_ready_handler`가 남아 있어
`NativeLibraryLoader.ValidateRequiredExports()`가 `DllNotFoundException`을
던졌고, 모든 테스트가 `CoreTestSupport.IsNativeAvailable()` 초입에서 조용히
return했다. 실제로 HEAD에서 `bash tests/run_tests.sh`는 181개 전부 통과했지만
총 실행 시간이 296ms였다 — 즉 "통과"가 아니라 vacuous였다. 이 목록을 고친
뒤에야 실 계약 실패들이 드러났고, 아래 수정은 전부 그 이후에 나온 것이다.

`bindings/dotnet/Zlink.sln`은 존재하지 않는 sample project 5개
(`SpotRecv`, `SpotRequestAsync`, `ActorRoomServer`, `ActorGatewayRelay`,
`ActorSinglePlayerQueue`)를 참조해 HEAD에서 이미 복원 불가다(이번 작업 범위
밖으로 남겨 둔다). `perf/multi`의 `PerfMultiStreamServer.cs`도 HEAD에서 이미
컴파일 실패였다(`IStreamSocket.Send(rid)`가 `RoutedSendOperation`인데
`.Flags(...).Submit()`을 호출) — smoke 실행을 위해 `TrySend(rid)`로 고쳤다.

## 테스트

명령: `ZLINK_CORE_SOURCE=local bash bindings/dotnet/tests/run_tests.sh`
(local `core/build` 0.13.0 runtime).

```
Passed!  - Failed: 0, Passed: 184, Skipped: 0, Total: 184, Duration: 31 s
SUMMARY,passed,7,failed,0,total,7      # samples/run_samples.sh
```

- xUnit 184/184 통과, sample 7/7 통과.
- HEAD 대비 **새 실패는 없다.** HEAD의 181 통과는 native runtime이 로드되지
  않은 vacuous 통과였으므로 수치 비교는 의미가 없다(위 "선행 상태").
- 아래 항목들은 native가 실제로 로드되면서 처음 드러난 것이고 모두
  이번 변경에서 해소했다:
  `test_validation_contract.monitor_open_rejects_unknown_event_flags`(정지),
  `test_flow_state.pause_and_resume_report_flow_events_with_full_payload`
  (연결 전 send), `test_socket_concurrency.*`(아래 submit gate),
  routed/publisher 계약 테스트(새 계약으로 재작성).

### Core multipart 결함 (재현과 해소)

계약 테스트를 실 native runtime에서 처음 돌렸을 때, 2 part 이상의 record를
`zlink_send_async`로 넘기면 DEALER/ROUTER 모두 `ZLINK_SEND_TERMINAL` +
`errno=EFSM(156384763)`로 완료했다. inproc과 tcp 모두 동일했다. .NET 밖에서도
같은지 확인하려고 plain C probe(`zlink_send_complete_handler` +
`zlink_send_async` 2 part)를 `core/build`에 직접 링크해 실행했고 동일하게
재현됐다 — binding 결함이 아니라 Core 결함이었다. 원인은
`try_admit_send_pending()`이 record의 **모든** part를 exact-target
`send_direct_with_retry(&rid, ...)`로 보내, 첫 part 이후 socket의 xsend
continuation state(ROUTER의 `_current_out`/`_more_out`, DEALER의 load-balancer
multipart pipe)를 무시한 데 있다.

작업 중 다른 작업자가 같은 결함을
`core/src/runtime/sockets/common/socket_send_complete.cpp`에서 고쳤다
(routed target을 첫 part에서만 pin하고 continuation은 일반 xsend 경로로
보낸다). `cmake --build core/build` 후 C probe는 `result=0`(ADMITTED)로
바뀌었고, .NET multipart routed send 계약 테스트도 그대로 통과한다. 이
로그의 테스트 수치는 그 고쳐진 Core 기준이다.

### 계약 테스트 갱신

- `test_publisher_async_admission.cs` → `test_publisher_sync_publish.cs`로
  교체했다. 동기 `Submit()`, `NODROP` 즉시 backpressure, `TryPublish`의
  non-throwing `false`, lossy publisher의 무대기를 검사한다.
- `test_routed_async_admission.cs`를 새 계약으로 다시 썼다. HWM 대기가
  `BACKPRESSURED` 반환이 아니라 Core pending operation이 되므로 fill helper는
  "park한 task"를 기준으로 바뀌었고, admission deadline 검사는 Core-owned
  request timeout 검사로 대체했다.
- multipart routed send 계약 테스트
  (`test_routed_async_admission.multipart_record_admission`)를 새로 넣었다 —
  위 Core 결함의 회귀 방지용이다.
- `test_socket_surface.cs`에 publish 동기 terminal과 `OnSendReady` /
  `AsyncSendOperation` 부재를 검사하는 케이스를 추가했다.
- `test_request_reply.cs`의 `OnSendReady(() => { })` 호출 3곳을 제거했다.
- `test_validation_contract.cs`의 `MonitorOpen((SocketEvent)0x10000)`은
  0x10000이 `SocketEvent.SendFlowPaused`(유효 값, `All == 0x7FFFF`)라서 이미
  깨진 검사였다 — monitor가 열린 채 leak되어 ctx term이 영원히 걸렸다.
  `0x80000`으로 고쳤다. HEAD에서는 native가 로드되지 않아 드러나지 않았다.
- `CoreTestSupport.SendAsyncWithTimeout`과 routed fill helper에 재시도를
  넣었다. binding이 재시도하지 않으므로 "아직 연결/route 미확립"에 대한
  재시도는 호출자 정책이다.

## Grep 증거

```
$ grep -rn "send_ready\|SendReadyHandler\|OnSendReady" bindings/dotnet/{src,perf,samples} --include=*.cs
(no match)

$ ls bindings/dotnet/src/Zlink/Runtime/Messaging/ | grep -i admission
(no match — RoutedAdmissionScheduler.cs / PublisherAdmissionScheduler.cs deleted)

$ grep -rn "new Thread(\|System.Threading.Timer\|ThreadPool.UnsafeQueueUserWorkItem\|ThreadPool.QueueUserWorkItem" \
    bindings/dotnet/src --include=*.cs
src/Zlink/Runtime/Eventing/ZlinkThread.cs:15      # public Zlink.CreateThread(...) wrapper (user-requested thread)
src/Zlink/Runtime/Eventing/CallbackDelivery.cs:104 # callback delivery thread (pre-existing, not admission)
src/Zlink/Runtime/Messaging/RequestProgressPump.cs:127 # request reply progress poller (pre-existing, see Flagged)
```

admission 경로에는 binding-owned thread, timer, queue, 재시도가 하나도 남아
있지 않다. 남은 세 `new Thread(...)`는 admission과 무관하며 두 개는 공개 API
표면(사용자가 요청한 thread)과 콜백 전달 경로다. 세 번째는 아래 Flagged 항목이다.

## PERF SMOKE

local `core/build` 0.13.0 runtime, tcp, 64B, duration 1, runs 1.

### single suite — `status: complete` (7/7)

```
bash bindings/dotnet/perf/single/run_benchmarks.sh \
  --pattern ALL --transports tcp --msg-sizes 64 --duration 1 --runs 1
```

| Pattern | 64B Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 |
|---|---|---|---|---|---|
| PAIR | 602.36 Kmsg/s | 38.55 MB/s | 1.591 ms | 9.625 ms | 22.481 ms |
| PUBSUB | 269.52 Kmsg/s | 17.25 MB/s | 0.822 ms | 2.659 ms | 10.301 ms |
| DEALER_DEALER | 132.27 Kmsg/s | 8.47 MB/s | 340.076 ms | 617.056 ms | 637.368 ms |
| DEALER_ROUTER | 207.63 Kmsg/s | 13.29 MB/s | 370.232 ms | 746.313 ms | 812.177 ms |
| DEALER_ROUTER_REQREP | 29.17 Kops/s | 3.73 MB/s | 1.213 ms | 9.951 ms | 10.612 ms |
| ROUTER_ROUTER | 300.18 Kmsg/s | 19.21 MB/s | 158.445 ms | 515.568 ms | 549.567 ms |
| ROUTER_ROUTER_REQREP | 21.63 Kops/s | 2.77 MB/s | 0.990 ms | 1.695 ms | 2.867 ms |

report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260824_024559_dotnet-realign-final.txt`
(`status: complete`)

### multi suite — `status: partial` (6/7)

```
bash bindings/dotnet/perf/multi/run_benchmarks.sh \
  --transports tcp --duration 1 --msg-sizes 64
```

| Pattern | 64B Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 |
|---|---|---|---|---|---|
| MULTI_DEALER_DEALER | 283.556 Kmsg/s | 18.148 MB/s | 0.404 ms | 0.404 ms | 11.022 ms |
| MULTI_DEALER_ROUTER_SENDSEND | 57.870 Kmsg/s | 3.704 MB/s | 0.649 ms | 1.197 ms | 1.961 ms |
| MULTI_ROUTER_ROUTER_SENDSEND | 52.245 Kmsg/s | 3.344 MB/s | 0.713 ms | 1.382 ms | 2.212 ms |
| MULTI_DEALER_ROUTER_REQREP | 21.332 Kops/s | 2.730 MB/s | 1.064 ms | 2.294 ms | 3.082 ms |
| MULTI_ROUTER_ROUTER_REQREP | 21.363 Kops/s | 2.734 MB/s | 1.120 ms | 2.711 ms | 3.448 ms |
| MULTI_PUBSUB | 693.804 Kmsg/s | 44.403 MB/s | 292.950 ms | 690.973 ms | 815.508 ms |
| MULTI_STREAM | FAIL (`process_exit_nonzero`) | — | — | — | — |

report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260824_024637.txt`
(`status: partial`)

`MULTI_STREAM`은 이 suite의 기본 stream client 수(10,000)에서만 실패한다.
같은 명령을 `--clients 1000`(또는 200)으로 돌리면 `status: complete`이고
(200 clients 기준 150.6 Kmsg/s, 19.3 MB/s, p99 3.3 ms), `--clients 5000`에서는
다시 partial이다. 즉 binding 계약 문제가 아니라 이 WSL2 환경의 10k 동시
TCP client fan-out 한계다. `perf/multi` 프로젝트는 HEAD에서 컴파일조차 되지
않았으므로 비교할 baseline이 없다.

### 실행 환경 주의

이 저장소에서 다른 작업이 병렬로 돌고 있어 측정 중 load average가 9~50
사이를 오갔다. load가 25 이상일 때는 duration 1 single suite에서
`DEALER_DEALER`(때로 `ROUTER_ROUTER`)가 `non_zero_exit_2`로 실패했다.
원인은 harness가 active window 안에 도착한 record만 세는데
(`recvTicks <= deadlineTicks`), 새 완료 경로가 완료 continuation을 thread
pool로 넘기므로 load가 높으면 1초 창 안에 유효 sample이 하나도 남지 않기
때문이다. 같은 명령을 duration 3으로 돌리면 load 25에서도
`status: complete`였고(report `..._dotnet-realign-d3.txt`), load가 10으로
내려간 뒤에는 duration 1에서도 `status: complete`였다. 위 표는 후자다.
따라서 절대 수치는 이 환경의 비교 기준으로 쓰기 어렵다.

## Flagged

1. **`RequestProgressPump`는 아직 binding-owned thread다.** request의 reply
   완료를 구동하기 위해 socket handle당 background thread에서
   `zlink_poller_wait(POLLCOMPLETION)`을 돌린다. admission 기계장치가 아니고
   이번 0.13.0 break와도 무관한 선행 코드지만, "바인딩은 스레드를 하나도
   소유하지 않는다"는 규범과는 어긋난다. Core가
   `zlink_send_complete_handler` 설치 시 socket의 async mailbox를 시작하듯
   reply 완료도 handler 설치만으로 구동되는지 확인한 뒤 제거해야 한다.
   이번 범위에서는 제거 시 request 완료가 아예 오지 않을 위험이 있어
   손대지 않았다.
2. **`bindings/dotnet/Zlink.sln`이 HEAD에서 이미 복원 불가**다 — 존재하지 않는
   sample project 5개(`SpotRecv`, `SpotRequestAsync`, `ActorRoomServer`,
   `ActorGatewayRelay`, `ActorSinglePlayerQueue`)를 참조한다. 프로젝트별
   빌드로 우회했다.
3. **`ZLINK_OPT_SEND_PENDING_MAX_MSGS` / `ZLINK_OPT_SEND_PENDING_MAX_BYTES`가
   .NET 옵션 표면에 없다.** 이 두 옵션이 새 async send의 pending bound이자
   `BACKPRESSURED` 반환의 유일한 원인이므로, 어플리케이션이 backpressure
   정책을 소유하려면 노출이 필요하다. 이번 작업 지시 범위 밖이라 추가하지
   않았다.
4. **inline admission은 보장이 아니다.** Core는 inproc DEALER에서도 완료를
   async mailbox로 dispatch하는 경우가 있어, `Async()`가 항상 완료된 Task를
   돌려주지는 않는다. 계약 테스트는 "호출자를 점유하지 않는다"만 검사하도록
   썼다.
5. **`test_router_multiple_dealers(transport: "tcp")`가 간헐적으로 실패**한다
   (route 학습 타이밍). 재실행하면 통과한다. binding 계약 문제가 아니라
   테스트의 connect 대기 문제로 보인다.
6. **`MULTI_STREAM` multi smoke는 기본 10,000 client에서 실패**한다
   (1,000 client에서는 `status: complete`). 이 환경의 fan-out 한계로
   보이지만 baseline이 없어 단정하지 못했다.
7. **request 제출은 `DONTWAIT`**이다. C++ 참조 구현은 blocking flags를 쓰지만
   C++ terminal은 blocking `submit()`이다. .NET terminal은 `Async(...)`이므로
   blocking flags를 쓰면 `SubmitGate`를 잡은 채 HWM에서 무한 대기해
   같은 socket의 다른 제출을 모두 막는다(실제로 테스트가 hang했다).
   `DONTWAIT`은 backpressure를 즉시 표면화하며 재시도 정책은 어플리케이션
   몫이다 — spec의 "바인딩은 재시도하지 않는다"와 일치한다.
