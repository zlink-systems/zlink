# #4060 .NET binding submit·close 검증

지정 branch `fix/4060-dotnet-binding-submit-close`에서 .NET binding과 그 test만 변경했다. 시작 시 `git status --short --branch`는 깨끗했고 지정 branch를 표시했다. 기준 revision은 사용자가 지정한 main `85835f03ad`다. 별도 checkout이나 fetch는 하지 않았다. C++ 참조는 이 기준 사본에 있는 `bindings/cpp/src/Runtime/Native/socket_handle.hpp`를 직접 읽었다. 원문과 줄 번호는 `raw/main-socket_handle.hpp.numbered`에 보존했다. commit, push, GitHub 작업, spec 수정, Core 빌드는 하지 않았다.

정확성 검증 결과와 비용 목표 충족 여부는 별개다. 동기 send·publish·reply는 Interlocked 증가가 +2여서 요청한 +1 목표를 충족하지 못한다. request와 completion entry가 필요한 async send는 기존 context 번호 Interlocked를 없애 +1이다. ABI나 public API를 바꾸지 않고 동일 상태값의 활성 예약 수를 정확히 유지하려면 입장과 퇴장에 각각 원자적 갱신이 필요하다. 이 차이를 숨겨 +1이라고 판정하지 않는다.

## 1. 호출 경로와 경합 구간

이 절에서 ‘변경 전’ 줄 번호는 기준 main 사본을 가리킨다. `raw/main-handle-sites.txt`에는 변경 전 handle 사용 위치 전체를 보존했다. 아래 native 호출 줄은 해당 호출의 첫 줄이다.

| 경로 | 변경 전 handle 취득과 Core 진입 | close와의 경합 구간 | 변경 후 예약과 Core 진입 |
|---|---|---|---|
| send, routed send, sync와 async | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:30` → `CompletionOwner.Send`/`SendAsync` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:226` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:475`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:478` | `_closing` 확인 뒤 parts 복사와 native 호출까지 예약이 없다. 저장된 IntPtr는 close 후에도 남는다. | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:455`의 본문 예약 → `call.Handle`로 native send. 반환·예외 직후 Dispose로 해제한다. |
| request, routed request, sync와 async | `bindings/dotnet/src/Zlink/Runtime/Sockets/DealerSocket.cs:35`, `bindings/dotnet/src/Zlink/Runtime/Sockets/RouterSocket.cs:77` → `CompletionOwner.Request`/`RequestAsync` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:382` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:523` | Register의 `_closing` 확인도 그 뒤의 native 입장을 보호하지 못한다. close가 entry를 실패 처리한 뒤 native 호출이 시작될 수 있다. | `RequestMessageSubmitter.Submit`에서 SocketHandle 예약 후 `zlink_request`. native 반환 직후 해제하며 reply 대기까지 유지하지 않는다. `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:542`에서 completion 수신은 별도로 예약한다. |
| reply, Received.reply | `bindings/dotnet/src/Zlink/Runtime/Messaging/Received.Operations.cs:33` / `bindings/dotnet/src/Zlink/Runtime/Sockets/RouterSocket.cs:95` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:220` → `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:243` | `_closing` 읽기와 payload 복사 뒤에도 stale IntPtr로 Core에 들어갈 수 있다. | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:239`의 `ReplyMessageSubmitter.Submit`에서 예약하고 native reply 반환 직후 해제한다. |
| publish, try-publish, 단일 part | `bindings/dotnet/src/Zlink/Runtime/Sockets/PublisherSocketBase.cs:45` → `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.SendCore.cs:45` → `SinglePartSubmit.MoveTo` → `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.SendCore.cs:183` | Handle을 struct에 복사한 뒤 message 이동과 native 입장 사이에 close가 끝날 수 있다. | `PublishSingleSubmitter`는 SocketHandle을 보유한다. `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.SendCore.cs:182`에서 같은 방식으로 마지막 native 입장 직전에 예약한다. |
| multipart send·publish | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.MultipartSubmit.cs:90`, `zlink_send(Handle, ...)`, `zlink_send_rid(Handle, ...)` | 인자 평가의 Handle 읽기와 native 입장 사이에도 close가 가능하다. | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.MultipartSubmit.cs:87`에서 예약한다. 거부 예외는 기존 `RestoreManaged` 경로로 이동한다. |

close는 `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketBase.cs:86` → `SocketKernel.Close` → `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Lifecycle.cs:36` → `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs:39`다. 변경 전에는 native close가 반환한 뒤 `_handle = IntPtr.Zero`를 썼고, 그 뒤 `CompletionOwner.CompleteClose`가 `_closing`을 썼다. native 해제와 두 managed 갱신 사이에 별도 구간이 있었다. `DangerousGetHandle`은 `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs:51`에서 plain IntPtr를 두 번 읽어, null 검사 이후 close가 값을 바꾸면 반환 값도 달라질 수 있었다. 같은 객체의 close 두 번도 native 입장을 원자적으로 배제하지 않았다.

변경 후 close의 선형화 위치는 `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs:38`다. native close는 예약 수가 0인 경우에만 호출한다. 결과 공개와 context 참조 해제는 같은 finally에서 수행한다. `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Lifecycle.cs:39`는 Core SHUTDOWN도 이미 닫힌 결과로 처리하고 pending request를 TERMINATED로 정리한다.

| 다른 경합 handle 사용 | 변경 후 보호 범위 |
|---|---|
| receive, subscribe receive, ROUTER generation | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs:35`에서 예약한다. native 수신뿐 아니라 빌린 RID 복사와 route generation 읽기까지 유지한다. |
| STREAM packet, XPUB subscription event | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Stream.cs:20`, `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveSubscription.cs:65`에서 native 호출과 metadata 복사를 보호한다. |
| option get/set, routing ID get/set | `SocketOptionAccessor`의 예전 DangerousGetHandle 4곳을 제거했다. 일반 option은 SetCore/GetCore, routing ID는 전용 분기에서 예약한다. byte/string 재호출이 중첩 예약하지 않는다. |
| bind/connect/unbind/disconnect, route·connection snapshot | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Connection.cs:14` 및 같은 파일의 각 API에서 예약한다. |
| subscribe/unsubscribe, subscription introspection | `SocketKernel.Send.cs`의 SetSubscription/UnsetSubscription과 `bindings/dotnet/src/Zlink/Runtime/Sockets/SubscriberSocketBase.cs:33`에서 예약한다. introspection의 크기 조회와 값 조회를 함께 보호한다. |
| TLS, receive flow state | `SocketBase`의 세 native control 호출에서 예약한다. |
| monitor open | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Options.cs:91`에서 socket을 예약한다. 이미 생성한 monitor의 recv/status/close는 별도 monitor handle의 lifecycle이며 이번 socket 예약의 대상이 아니다. |
| completion recv, inline과 poller drain | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:287` 한 곳에서 예약한다. 닫힌 socket이면 native에 들어가지 않고 RecvResult.Terminated를 전달해 pending entry를 실패 처리한다. |
| poller add/modify, temporary ZlinkPoll | Poller.Add/Modify는 socket 예약을 유지한 채 등록을 변경한다. `ZlinkPoll.PollSocketsCore`는 socket마다 예약하고 일부 획득 실패도 finally에서 해제한다. |
| poller remove, poller wait | remove는 등록 당시 식별자를 계속 쓴다. Core가 registration의 lifetime pin을 소유하므로 closed socket도 제거할 수 있다. `core/src/api/core/zlink.cpp:298`의 주석과 구현을 확인했다. 여기서 새 socket 예약을 요구하면 기존 closed_registered_socket_can_be_removed 계약을 깨뜨린다. wait는 socket 포인터를 새로 인자로 넘기지 않는다. drain은 위의 ReceiveCompletion을 거친다. |
| proxy | frontend/backend/capture 각각을 예약하고 native proxy 실행 동안 유지한다. |

별도 monitor·timer·context handle 자체의 동시 dispose 설계는 이 socket 작업의 범위 밖이다. 동일 SocketHandle의 native 입장을 수행하는 경로는 모두 위 예약으로 전환했다.

## 2. 예약 설계와 memory ordering

`bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs:16`의 long 하나에 low 62-bit 활성 호출 수, bit 62 Closed, bit 63 Closing을 둔다. `_handle`은 생성 후 바뀌지 않는 readonly IntPtr다. 새 지속 제어 상태는 `_state` 하나이며 CompletionOwner의 `_closing`은 삭제했다. 기존 context 참조는 accepted close까지 유지하고 native close 중에는 지역 참조와 `GC.KeepAlive(context)`로 보존한다.

새 `_lifetimeOwner`는 SocketKernel 참조다. active Call이 SocketHandle만 유지하면 Kernel finalizer가 먼저 실행돼 BUSY로 끝나고 해제가 누락될 수 있다. 이 역참조는 finalizer를 가진 Kernel도 마지막 native 사용까지 유지한다. 제어 상태나 별도 입장 판정은 아니다. `reservation_roots_the_finalizing_kernel`에서 외부 socket 참조 없이 강제 GC를 수행해 이 수명을 검사한다. 새 `SocketHandle.Call._owner`는 stack 예약이 해제될 때까지 이 SocketHandle과 Kernel, context를 유지하는 필드다. using의 struct Dispose는 boxing이나 새 lease 객체 할당 없이 Interlocked.Decrement를 한 번 수행한다. submitter의 기존 Handle 필드는 IntPtr 대신 SocketHandle 참조로 바뀌었다. 새 lock, queue, generation, timer, persistent flag는 없다.

`TryReserve`는 Volatile.Read로 상태를 읽고, 열린 상태에서 Interlocked.CompareExchange로 수를 하나 늘린다. CAS 실패 때는 반환된 관측값으로 반복하므로 별도 volatile 읽기가 매번 추가되지 않는다. Closing이면 SpinWait와 Volatile.Read로 close 결과를 기다린다. Core BUSY는 열린 상태로 돌아올 수 있으므로 Closing만 보고 영구 종료라고 판단하지 않는다. Closed이면 실패한다. native 호출 직후 using의 finally가 Interlocked.Decrement로 예약을 해제한다. 스레드마다 독립된 native 호출을 실행할 수 있으며 새 socket lock으로 submit을 직렬화하지 않는다.

Interlocked 연산은 full fence다. Volatile.Read는 acquire, Volatile.Write는 release다. Linux x64 측정 기준으로 native 호출의 입장과 종료를 다음 순서로 설명할 수 있다.

1. reserve CAS가 먼저 성공하면 상태가 0이 아니므로 close의 0→Closing CAS는 실패한다. close는 BUSY를 반환하고 native close를 호출하지 않는다. 마지막 release의 full fence보다 앞에 native 호출 종료가 있으므로, close가 다시 0을 관측해 성공할 때는 앞선 native 사용이 끝난 뒤다.
2. close CAS가 먼저 성공하면 이후 reserve CAS는 이전 열린 상태를 기대하더라도 실패한다. acquire 읽기가 Closing을 보거나 CAS가 Closing을 반환하며, 포인터를 native에 넘기지 않는다. close는 결과를 release로 공개한다. OK/SHUTDOWN은 Closed, 그 밖의 반환·호출 예외는 0을 공개한다.
3. 대기자가 공개된 Closed를 acquire로 읽으면 그 native handle을 사용할 수 없다. BUSY 후 공개된 0을 읽으면 새 CAS로 예약할 수 있다. 성공한 예약이 존재하는 동안 accepted close와 pointer release가 동시에 일어날 수 없다. 다른 close가 Closing을 보면 BUSY이므로 native close 두 번도 겹치지 않는다.

실제 TERMINATED 판정은 `TryReserve`의 Closed 검사 하나다. `Reserve(submit: true)`는 이 실패를 SubmitError(TERMINATED, ESHUTDOWN)로 투영한다. 일반 control 경로는 종전처럼 ObjectDisposedException으로 투영한다. completion recv는 동일 실패를 RecvResult.Terminated로 투영한다. close의 Closed 검사는 idempotent close 반환을 위한 것으로 submit 입장 판정이 아니다.

submit 거부 판정 위치는 변경 전 `EnsureOpenForSubmit`, `Register`의 `_closing`, `DangerousGetHandle`의 zero 검사 3곳에서 변경 후 `TryReserve` 1곳으로 줄었다. 검사 호출 수가 아니라 상태로 submit 입장을 결정하는 구현 위치를 셌다. poller transfer/drain의 별도 `_closing` 검사 2곳도 제거했다. 새 public API, public member, native ABI 변경은 없다. 제거·추가한 Handle/Reserve 같은 member는 모두 internal type의 구현이다. C#의 public 키워드가 붙은 내부 member까지 포함하면 변경 목록은 `SocketHandle(Context, SocketType)` → `SocketHandle(SocketKernel, Context, SocketType)`, `SocketHandle.DangerousGetHandle()` 삭제, `SocketKernel.Handle` 삭제, `SocketOptionAccessor.ReadSocketType(IntPtr)` 삭제, 내부 `SocketHandle.Call.Dispose()` 추가다. 이 type들은 외부에 공개되지 않는다.

entry 등록과 close snapshot은 기존 `_sync` 하나로 순서화한다. close snapshot 전에 등록된 entry는 FailLifecycle이 정리한다. snapshot 뒤에 등록되더라도 native 예약이 거부되어 기존 AbortBeforeNativeWait/Remove 경로가 정리한다. 별도의 registry closed 상태는 필요 없다. async request에 public completion owner가 없으면 기존 InvalidState 검사를 수행하되, 오류를 내기 전에 같은 SocketHandle 예약으로 TERMINATED 우선순위를 보장한다. 유효한 async request에는 이 실패 경로의 추가 예약이 없다.

예약은 submitter의 native 호출 안에서 끝낸다. 초기 구현에서 SubmitPreservingOnFailure 전체를 감싸면 native 반환 뒤 managed ConsumeParts/publication 대기까지 예약이 남았다. 기존 close_does_not_wait_for_an_in_flight_async_submit 검사가 이 문제를 드러냈다. 해당 중간 실행은 SIGTERM으로 중지했고 `raw/after-races-01.log`, `raw/after-races-01-stacks.log`, stress log를 보존했다. 최종 구현은 이 구간을 수정했으며 기존 test 조건이나 timeout은 완화하지 않았다.

실패한 예약의 message 소유권은 기존 helper가 처리한다. RequestReplySupport는 native 복사본만 정리하고 managed 원본을 유지한다. SinglePartSubmit은 이동한 message를 RestoreFrom으로 되돌린다. multipart 이동은 기존 RestoreManaged를 사용한다. Core가 실제 반환한 submit result에 대한 종전 consume 규칙은 바꾸지 않았다.

SafeHandle 대안은 배제한 판단이 맞다. `bindings/dotnet/tests/Zlink.Tests/test_socket_handle_reservation.cs:74`에서 DangerousAddRef 뒤 Dispose를 호출해도 ReleaseHandle이 실행되지 않고 마지막 DangerousRelease 때 한 번 실행됨을 확인한다. ReleaseHandle이 true/false를 반환하는 양쪽을 검사하며 false도 Dispose/DangerousRelease 호출자에게 BUSY 결과를 반환하지 않는다. 따라서 동기 close가 즉시 BUSY 여부를 알려야 하는 계약을 SafeHandle의 해제 지연으로 구현할 수 없다. target runtime은 .NET 8.0.31이다.

소유 계층은 binding의 SocketHandle이다. binding spec `bindings/doc/spec/README.ko.md:1851`, `:2829`, `:2845`와 Core socket spec `core/doc/spec/core/socket/README.ko.md:58`, `:647`, `:657`을 적용했다. C++의 acquire/acq_rel CAS, release fetch_sub, close 결과 release store와 같은 순서다. C++의 wait/notify 대신 .NET SpinWait와 Volatile을 사용한다. Java 구현 변경이나 Framework runtime 변경은 하지 않았다. 변경 분류는 B, 기존 binding lifecycle 결함이다.

## 3. 검증 결과

최초 main 재현은 production source를 수정하기 전에 수행했다. Main과 변경 후 모두 CPU stress-ng 4개 worker, cpu-load 70, matrixprod를 `/root/jobs/fx/cx/measure-shared.sh` 안에서 실행했다. 각 실행 시작에 MemAvailable 12 GiB 이상을 확인했고 stress-ng는 SIGTERM으로 종료했다. helper는 `run_loaded.py`다. Core는 `bindings/tools/local_core_runtime.sh`가 선택한 기존 `/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.1.23.0`를 사용했다. 실행 전·후 SHA-256은 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`로 같다. `raw/environment.json`에 보존했다.

각 행은 같은 socket의 submit 한 번과 close 한 번을 동시에 시작한 1,000회다. close의 BUSY는 submit 실패 수에 포함하지 않으며, 반환 후 제출 thread를 join한 다음 close를 완료했다. 이어 같은 socket에서 close 후 submit도 1,000회 검사했다. request는 async 첫 native admission을 경합시키고, close 후 pending Reply의 RequestError(TERMINATED)를 확인한다. 동기 request와 multipart 거부는 별도 ownership 검사로 확인한다.

| 구현 | 경로 | 성공 | TERMINATED | EFAULT | ENOENT | INVALID_HANDLE | NOT_FOUND | 기타 | close BUSY |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| main | send | 1 | 999 | 0 | 0 | 0 | 0 | 0 | 0 |
| main | request | 0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0 |
| main | reply | 0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0 |
| main | publish | 3 | 338 | 0 | 0 | 1 | 0 | 658 | 0 |
| 변경 후 | send | 0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0 |
| 변경 후 | request | 0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0 |
| 변경 후 | reply | 0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0 |
| 변경 후 | publish | 2 | 998 | 0 | 0 | 0 | 0 | 0 | 2 |

main의 기타 658회는 publish의 ObjectDisposedException이다. close 후 검사에서 main send/request/reply는 각각 TERMINATED 1,000회였지만 publish는 ObjectDisposedException 1,000회였다. 변경 후 close 후 검사는 네 경로 모두 TERMINATED 1,000회였다. 분류는 native errno EFAULT/ENOENT를 먼저 적용하고, 이후 typed InvalidHandle/NotFound와 기타 exception을 센다. 존재하지 않은 EFAULT/ENOENT를 관측했다고 적지 않았다. Native Core가 stale 입력에도 TERMINATED를 반환할 수 있으므로 결과 분류만으로 pointer 안전성을 증명하지 않으며 2절의 예약 순서 증명을 함께 적용한다.

main raw는 `raw/main-races.log`, `raw/main-races-stress.log`, `raw/main-test-results/main-races.trx`다. 최종 변경 후 raw는 `raw/final-load.log`, `raw/final-load-stress.log`, `raw/final-load-results/final-load.trx`다. 집계는 `raw/race-summary-final.json`이다. 최초 검사는 main 4개 중 publish 실패, 최종 load 검사는 38/38 통과다.

추가 검사는 validation과 native 입장 사이 close, 중첩 예약 두 개와 마지막 해제 전 BUSY, 별도 thread의 동시 예약, 예약 중 강제 GC, SafeHandle 지연 해제의 true/false 결과, close 후 1/2/9 part 원본 보존이다. 9 part는 stack 한도 8을 넘어 ArrayPool 복원 경로도 검사한다. 기존 mixed 단일·multipart 50,000회 경합은 INVALID_HANDLE/NOT_CONNECTED/ObjectDisposedException/INVALID_ARGUMENT를 허용하던 조건을 제거해 성공과 TERMINATED만 허용하도록 강화했다. 이를 포함한 관련 47개 load 검사는 `raw/after-root-focused.log`에, 마지막 38개는 위 final-load에 있다.

전체 script `bindings/dotnet/tests/run_tests.sh`의 무필터 실행은 두 번 시도했고 완료되지 못했다. `raw/full-suite.log`는 294 통과와 source 검사·task 예외 2개 실패 뒤 진행 정지, `raw/full-suite-02.log`는 229 통과 뒤 진행 정지다. 중지는 SIGTERM/SIGINT로 수행했다. 두 번째 실행의 diag는 정지한 case를 `test_message_gc_lifetime.payload_round_trips_during_concurrent_compacting_gc(size: 30)`으로 특정한다. 해당 검사는 socket submit을 사용하지 않는다. 보존한 main Release DLL(`perf-main/Systems.Zlink.dll`)을 새 프로세스에 적재한 분리 실행에서도 같은 case가 진행을 멈췄다. `raw/main-message-gc.log`, `raw/main-message-gc-diag.host.*.log`, `raw/main-message-gc-native-stacks.log`와 `raw/full-suite-02-native-stacks.log`에 근거를 보존했다. Core나 message 구현은 수정하지 않았다. 이 기존 검사 3개 data case는 미해결이며 무필터 전체 suite 통과라고 판정하지 않는다.

진행 정지와 별개로 드러난 두 검사는 다음처럼 처리했다. source guard는 native 인자명 `_handle` 자체를 요구하던 항목을 native completion pull 사용과 지속 raw IntPtr 복사 금지로 바꿨다. 기존 background thread/timer/Task.Run 금지 조건은 전부 유지했다. task 예외는 기존 tokenless fixture가 Admitted를 관측하지 않고 Reply만 관측한 원인이다. 기준 main에서 `tokenless → unreturned`를 강제로 순서 실행한 `raw/main-unobserved-ordered.log`가 실패하는 음성 증거다. 두 public task를 모두 검사하도록 tokenless와 synthetic writable fixture를 강화했다. 같은 main DLL에 강화한 fixture를 적용한 `raw/main-fixed-fixture-ordered.log`와 변경 후 `raw/after-fixed-fixture-ordered.log`는 모두 통과했다. 전역 unobserved assertion이나 fixture timeout을 완화하지 않았다.

알려진 concurrent message GC 검사 3건만 명령 필터로 제외한 추가 실행은 361/361 통과와 sample 7/7 통과다. `raw/suite-except-baseline-gc.log`, `raw/suite-except-baseline-gc-results/suite-except-baseline-gc.trx`에 있다. 명령은 `bindings/dotnet/tests/run_tests.sh --filter FullyQualifiedName!~payload_round_trips_during_concurrent_compacting_gc`다. source에 Skip이나 완화 조건을 추가하지 않았다. 별도 sample 실행도 `raw/samples-separate-02.log`에서 7/7 통과했다.

`scripts/format/format.sh`는 Framework source만 선택해 binding에는 적용되지 않는다. 따라서 touched 파일만 대상으로 `dotnet format whitespace`를 적용하고 `--verify-no-changes`를 통과했다. `raw/format-apply-final.log`, `raw/format-verify-final.log`, `raw/format-verify-final.status.json`에 보존했다. `git diff --check`도 통과했다. 전체 변경은 new test 3개를 포함한 `change.diff`다.


## 4. 비용과 throughput

다음 표는 contention이 없는 Linux x64에서 유효한 submit 하나의 첫 native 입장과 해제까지 센 명시적 Interlocked/Volatile 호출이다. message API의 native 내부 atomic, 기존 Monitor의 내부 atomic, reply completion drain은 이 표에 섞지 않았다. CAS contention은 실패한 CAS만큼 비용이 추가된다.

| 경로 | main Interlocked | main Volatile.Read | 변경 후 Interlocked | 변경 후 Volatile.Read | Interlocked 증가 |
|---|---:|---:|---:|---:|---:|
| 동기 send, routed send | 0 | 1 | 2 | 1 | +2 |
| publish, try-publish, 단일·multipart | 0 | 0 | 2 | 1 | +2 |
| request 최초 submit, sync/async | 1 | 1 | 2 | 1 | +1 |
| reply | 0 | 1 | 2 | 1 | +2 |
| async send 첫 시도, public owner 또는 취소 가능 token | 1 | 1 | 2 | 1 | +1 |
| async send 첫 시도, public owner·취소 token 없음 | 0 | 1 | 2 | 1 | +2 |
| 이미 Closed인 submit의 예약 실패 | 0 | send/request/reply는 1, publish는 0 | 0 | 1 | 0 |

기존 `_nextContext` Interlocked.Increment를 registry insertion의 기존 `_sync` 안으로 통합해 제거했다. 동기 send·publish·reply에는 합칠 기존 Interlocked가 없다. 활성 수의 증가와 감소를 둘 다 같은 atomic word에 반영해야 한다. 증가만 atomic이면 감소의 lost update가 생기며, 감소를 생략하면 close가 영구 BUSY가 된다. SafeHandle처럼 마지막 release까지 close 자체를 지연하면 동기 close result 계약을 잃는다. 따라서 이 세 경로의 +1 목표는 현재 승인된 한 word 예약 설계와 기존 .NET baseline에서는 미충족이다.

completion recv마다 추가로 Interlocked 2회와 Volatile.Read 1회가 필요하다. 예를 들어 public poller가 없는 동기 request에서 reply 1개와 NO_DATA까지 수신하는 최소 경로는 submit 2회 + completion 수신 두 번 4회, 총 Interlocked 6회다. 이 경우 main의 context 번호 Interlocked는 1회였다. 대신 이전 Drain의 `_sync`/`_closing` 확인은 제거했다. 최초 submit 비용과 전체 reply 대기 비용을 구분했다.

A는 main, B는 변경 후다. 같은 SDK 10.0.112, target net8.0, Release, Optimize 기본값, ServerGC 설정, SourceLink 조회 비활성화로 빌드했다. 빌드 로그는 `raw/build-main-perf.log`와 `raw/build-perf-round2.log`다. 실행 DLL은 `perf-main/`과 `perf-after-final-02/`에 있다. Native Core는 앞 절의 동일 SHA-256 runtime이다.

기존 `bindings/dotnet/perf/single/Zlink.BindingBench` runner를 직접 실행했다. 두 패턴 모두 inproc, 1024 B, application part 1개, active duration 10초, latency sample cap 1,000,000이다. 각 실행을 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 수행했다. `run_pairs.py`, `pin_benchmark.py`가 명령과 순서를 보존한다. 홀수 pair는 AB, 짝수 pair는 BA다.

single runner는 한 프로세스의 sender thread와 receiver main thread를 사용한다. 외부 helper가 sender를 CPU 0–7, receiver를 CPU 8–15에 배정했다. sender 생성 직후 1 ms 간격 관측 세 번 뒤 pin을 적용하므로 시작 직후 짧은 구간은 초기 CPU 집합 0–7이다. 나머지 runtime thread도 초기 집합을 상속한다. 24회 모두 실제 source/target affinity와 종료 코드 0을 각 `.json`에 확인했다. 최초 두 probe는 최종 pair 통계에서 제외했다. 첫 probe의 pin 실패도 `raw/perf-probe-main-pair.*`에 그대로 보존했다.

| 패턴 | pair | 순서 | main msg/s | 변경 후 msg/s | 변화 | raw stdout/stderr |
|---|---:|---|---:|---:|---:|---|
| PAIR | 1 | AB | 1,141,113.4 | 1,151,350.2 | +0.90% | `raw/perf-pair-pair01-main.log` / `raw/perf-pair-pair01-after.log` |
| PAIR | 2 | BA | 1,114,278.1 | 1,089,954.0 | -2.18% | `raw/perf-pair-pair02-after.log` / `raw/perf-pair-pair02-main.log` |
| PAIR | 3 | AB | 1,121,990.7 | 1,079,277.8 | -3.81% | `raw/perf-pair-pair03-main.log` / `raw/perf-pair-pair03-after.log` |
| PAIR | 4 | BA | 1,063,824.6 | 1,119,202.5 | +5.21% | `raw/perf-pair-pair04-after.log` / `raw/perf-pair-pair04-main.log` |
| PAIR | 5 | AB | 1,132,130.3 | 1,103,003.2 | -2.57% | `raw/perf-pair-pair05-main.log` / `raw/perf-pair-pair05-after.log` |
| PAIR | 6 | BA | 1,179,404.2 | 1,118,852.8 | -5.13% | `raw/perf-pair-pair06-after.log` / `raw/perf-pair-pair06-main.log` |
| DEALER_DEALER | 1 | AB | 1,148,615.1 | 1,386,552.7 | +20.72% | `raw/perf-dealer_dealer-pair01-main.log` / `raw/perf-dealer_dealer-pair01-after.log` |
| DEALER_DEALER | 2 | BA | 1,299,073.2 | 1,383,218.6 | +6.48% | `raw/perf-dealer_dealer-pair02-after.log` / `raw/perf-dealer_dealer-pair02-main.log` |
| DEALER_DEALER | 3 | AB | 1,070,372.4 | 1,177,926.0 | +10.05% | `raw/perf-dealer_dealer-pair03-main.log` / `raw/perf-dealer_dealer-pair03-after.log` |
| DEALER_DEALER | 4 | BA | 1,266,595.0 | 1,114,077.1 | -12.04% | `raw/perf-dealer_dealer-pair04-after.log` / `raw/perf-dealer_dealer-pair04-main.log` |
| DEALER_DEALER | 5 | AB | 1,261,431.3 | 1,191,586.8 | -5.54% | `raw/perf-dealer_dealer-pair05-main.log` / `raw/perf-dealer_dealer-pair05-after.log` |
| DEALER_DEALER | 6 | BA | 1,167,924.1 | 1,206,952.8 | +3.34% | `raw/perf-dealer_dealer-pair06-after.log` / `raw/perf-dealer_dealer-pair06-main.log` |

PAIR의 paired ratio 기하평균은 -1.32%, 중앙값은 -2.38%다. pair 범위는 -5.13%에서 +5.21%다.

DEALER_DEALER의 paired ratio 기하평균은 +3.29%, 중앙값은 +4.91%다. pair 범위는 -12.04%에서 +20.72%다.

DEALER_DEALER의 편차가 커서 평균 증가만으로 성능 개선을 확정하지 않는다. 측정은 송신과 수신의 lifecycle 보호 비용을 함께 포함한다. Interlocked +1 목표가 충족됐다는 의미도 아니다. manifest와 runner 출력은 `raw/pairs-manifest.json`, `raw/pairs-runner.log`, 집계는 `raw/performance-summary.json`이다. 각 run의 `.json`에는 명령, runtime 경로, thread ID와 affinity를 남겼고 `.log`에는 throughput·bandwidth·latency 원문을 전부 남겼다.


## 5. 중복과 인계 점검

| 발견한 중복·인계 | 처리 |
|---|---|
| 변경 전 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:15`, `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.cs:47`의 지속 IntPtr 인계 | SocketHandle 참조로 교체했다. raw pointer는 예약된 native 호출의 인자로만 취득한다. |
| 변경 전 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:406`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:382`, `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs:51`의 중복 lifecycle 거부 | 세 판정을 TryReserve 한 곳으로 모았다. TransferToPublic/Drain의 `_closing` 판정과 두 곳의 `_closing` 갱신도 삭제했다. |
| 변경 전 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:380`와 Register의 `_sync` | context 번호 생성과 insertion을 기존 lock 안에서 함께 수행한다. 별도 atomic을 제거했다. |
| 변경 전 SocketKernel.Handle/SocketBase.Handle 및 `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketCallbacks.cs:18` | raw property 두 개와 RequireSocket 검증을 반복하던 helper를 제거했다. Poller는 기존 RequireSocket 후 예약한다. |
| 변경 전 `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOptionAccessor.cs:184` | 호출처가 없는 helper를 제거했다. socket type은 기존 SocketTypePolicy가 소유한다. |
| 변경 전 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:303`와 DrainInline의 같은 native 인계 | ReceiveCompletion 한 곳에서 예약·호출·해제를 수행한다. |
| `_closing` 삭제 뒤 의미가 같아진 CompleteClose와 FailPublicWaitTerminated | 기존 registry 실패 처리를 FailLifecycle 하나로 합쳤고 close와 poller가 이 owner를 호출한다. |

점검 결과는 7개 묶음 발견, 7개 수정, 이 작업에서 넘긴 중복 0개다. single-part 이동 helper와 RequestReplySupport의 복사 helper는 소유권 계약이 달라 유지했다. control API의 option-family get/set dispatch도 서로 다른 native API 선택이므로 lifecycle 판정으로 합치지 않았다. Framework의 execution gate는 수정 대상이 아니었다. 상태로 submit 입장을 결정하는 위치는 수정 전 3곳, 수정 후 1곳이다.

남은 항목은 동기 send·publish·reply의 Interlocked +1 목표 미충족과 기준 main에서도 재현되는 concurrent message GC 검사다. 이 두 항목을 해결했다고 보고하지 않는다. branch 변경, commit, push는 수행하지 않았다.
