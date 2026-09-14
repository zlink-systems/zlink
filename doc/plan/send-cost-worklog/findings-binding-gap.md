# binding이 C API 대비 느린 이유 — 구조 조사

조사 대상은 `request-backpressure`, 1 KiB의 `multi socket REQREP`이다. 벤치와
테스트는 실행하지 않았고, 이 문서는 source를 따라 세는 **호출·객체·잠금의 구조적
차이**다. 따라서 아래의 비용 항목은 존재가 확인된 것이며, 각 항목이 0.80 미달의
몇 %를 설명하는지는 profiler 없이는 말할 수 없다.

`메시지마다`는 정상적으로 admission된 request 1건과 그 reply 1건을 뜻한다.
backpressure 재제출은 별도 표시한 경우만 포함한다. `잠금`은 이 문서에서 확인한
binding 코드의 lock 획득 지점만 세며, Core 내부의 lock과 transport의 system call은
모든 행에 공통이므로 별도로 섞지 않았다.

## 1. C가 request 한 건에 하는 일

### client submit과 completion

1. `submit_request`는 stack `zlink_msg_t part` 하나를 `zlink_msg_init_size`로
   초기화하고, slot에 미리 보관한 payload를 `memcpy`한다
   (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-260`).
   이 하네스 수준에서는 **frame 할당 1회, 1 KiB payload copy 1회**다. two-part
   설정이면 helper가 추가한 empty tail의 비용은 여기서 읽지 못했다.
2. 같은 함수가 `zlink_request(..., DONTWAIT, ..., slot, &completion_id)`를
   정확히 **1회** 부른다 (`:290-305`), errno를 읽고 곧바로 `zlink_msg_close`한다
   (`:307-309`). 성공이면 slot의 정수 counters만 증가시킨다 (`:309-313`).
   C 하네스는 request마다 C++ future, map entry, mutex를 만들지 않는다.
3. Core의 `zlink_request`는 request pending pair와 completion id를 만들고 첫
   message에 request/reply metadata를 붙인 뒤 admission을 한 번 시도한다
   (`core/src/api/socket/socket_request_reply_submit_api.cpp:545-598`,
   `:602-631`). fast admission의 lifecycle scope는 Core 소유이고
   (`core/src/runtime/sockets/common/socket_send_submit.cpp:605-650`), 이는 모든
   binding이 결국 같은 C entry로 들어가서 공유하는 비용이다.
4. 한 bounded submit turn 뒤 C는 `zlink_poller_wait`를 **1회** 호출하고
   (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:576-603`), event가
   난 socket만 `zlink_completion_recv(DONTWAIT)`로 `NO_DATA`까지 drain한다
   (`:352-412`). 각 returned completion은 `zlink_completion_close`를 정확히
   **1회** 한다 (`:379`, `:395`, `:409`). request completion은 slot pointer와
   integer counter를 대조한 뒤 reply payload header를 읽는다 (`:400-409`,
   `:163-205`).

### server receive와 echo

5. server는 POLLIN 뒤 `zlink_router_recv(..., DONTWAIT)`를 한 번씩 호출하여
   `EAGAIN`까지 drain한다 (`:879-899`, `:932-960`). 성공 record의 part는 caller의
   stack array `zlink_msg_t parts[2]`로 **adopt**되어 들어온다. C API도
   uninitialized caller slots에 `zlink_msg_adopt`하며 payload bytes를 복사하지
   않는다 (`core/src/api/socket/socket_request_reply_router_api.cpp:228-241`).
6. server는 받은 parts 자체로 `zlink_reply`를 **1회** 호출하고 close한다
   (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:803-832`). 정상
   reply에는 새 1 KiB copy가 없다. reply가 backpressured일 때만 retry template와
   retry vector를 만들고 각 part를 `zlink_msg_copy`한다 (`:812-868`); 이는 정상
   경로의 건당 비용으로 세지 않았다.

### C에서 셀 수 없는 것

* 위 하네스의 정상 client submit/complete/server echo 코드에는 직접 OS syscall이
  없다. `zlink_poller_wait`와 `perf_socket_poll`은 Core/transport로 내려가는 API
  호출이며, TCP/IPC/inproc 및 ready 상태에 따라 실제 `poll`/read/write syscall 수가
  달라진다. 이 source 범위만으로 request 한 건의 syscall 수를 숫자로 확정할 수
  없다.
* Core의 pending-request state, pipe admission, timeout, transport write와 그 내부
  잠금·할당은 C와 네 binding이 공통으로 호출한다. 그것은 C 대비 binding의 추가
  비용이 아니므로 아래 비교에서는 제외했다.

## 2. 언어별로 C보다 더 하는 일

### C++

* **무엇** — async request마다 request completion bundle 하나를 heap에 만들고,
  그 안에 admission/reply async state와 completion entry를 둔 뒤 socket-local
  registry에 등록한다.
  **어디** — bundle 구조는
  `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:16-27`, 생성·lifetime bind·등록은
  `:101-142`다. registry는 첫 entry도 `_inline_entry`에 넣고 mutex를 잡으며,
  둘째부터 unordered map에 넣는다
  (`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:521-550`).
  **C는 어떻게 하는가** — C는 caller-owned `client_slot_t *`를 Core user context로
  넘기고 counter만 바꾼다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:290-313`).
  **메시지마다 몇 번인가** — 정상 request 1회마다 `make_shared` 1회, registry
  register 1회와 그 mutex lock 1회가 확인된다. public poller가 drain할 때 completion
  하나당 owner mutex를 여러 차례 잡아 entry를 찾고
  (`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:600-676`), terminal이면
  unregister도 1회다 (`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:676-679`).
  **없앨 수 있는가** — `RequestSubmission`의 admitted/reply 두 async 결과와
  Core completion의 user context를 연결해야 하므로 request state 자체는 공개
  동작에 필요하다. heap bundle, inline/map registry, mutex의 정확한 대체 가능성은
  이 조사만으로 판정하지 못했다.

* **무엇** — C API에 넘길 때 public `message_t`와 별개인 shallow native
  `zlink_msg_t` view를 만들고 닫으며, reply completion에서는 Core-owned parts를
  `std::vector<message_t>`로 adopt한다.
  **어디** — submit은 `submit_borrowed_message_part` 또는
  `submit_message_parts`를 거쳐 `zlink_request`를 호출한다
  (`bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:177-193`); completion
  capture는 vector를 resize한 뒤 part별 adopt를 한다
  (`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:402-412`).
  **C는 어떻게 하는가** — C는 원래 `zlink_msg_t part` 주소를 그대로 넘기고
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-305`), completion의
  reply pointer를 metric에 바로 쓴다 (`:400-409`).
  **메시지마다 몇 번인가** — payload part 1개이면 `zlink_msg_init` 1회,
  `zlink_msg_copy` 1회, `zlink_msg_close` 1회로 native view를 만든다
  (`bindings/cpp/src/Runtime/Native/native_message_parts.hpp:211-235`). 이 copy는
  refcounted storage를 공유하는 shallow copy라서 추가 1 KiB byte copy는 아니다.
  completion은 vector resize 1회와 reply part당 adopt 1회다.
  **없앨 수 있는가** — C++ public reply type이 `std::vector<message_t>`이므로
  vector materialization은 API surface에 맞춘 표현이다. internal staging을 제거할
  수 있는지는 ABI/native call contract 확인이 필요하다.

* **무엇** — perf harness도 request마다 detached coroutine, application-ready queue
  scheduling, `message_t::from`과 reply await를 만든다.
  **어디** — request coroutine과 message 생성은
  `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:463-524`; 한 turn의 poller
  wait는 `:338-360`이다. 서버는 `received_t`를 request마다 만들고 public
  `received.reply()` builder를 통해 echo한다 (`:553-562`, `:624-640`).
  **C는 어떻게 하는가** — C turn은 동일한 위치에서 직접 submit/poll/drain한다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:576-603`).
  **메시지마다 몇 번인가** — detached coroutine 1회, `message_t::from` 1회,
  reply `co_await` 1회가 하네스에서 확인된다. 이는 제품 binding의 completion
  path를 사용하기 위한 하네스 비용이지, C++ binding source만의 비용이라고
  분리할 근거는 없다.
  **없앨 수 있는가** — 이 async public API를 계속 재는 한 reply await 자체는
  필요하다. harness coroutine을 없애도 결과가 같은지는 실행 없이 알 수 없다.

### Java

* **무엇** — 정상 request도 `Pending`과 두 `CompletionStage`를 만들고
  `ConcurrentHashMap` registry에 넣으며, submit과 drain에 lock을 건다.
  **어디** — `submitRequest`의 normal path는
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:161-213`,
  `Pending` 등록은 `:550-560`이다. owner에는 `drainLock`, `ownerLock`,
  `nativeCallGate`, `ConcurrentHashMap`이 있다 (`:81-95`); native call은 read
  lock으로 감싼다 (`:259-271`).
  **C는 어떻게 하는가** — C 하네스는 slot pointer/counter만 사용한다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:163-205`, `:290-313`).
  **메시지마다 몇 번인가** — 정상 request마다 `drainLock.lock` 1회,
  `nextContextToken`의 `ownerLock` 1회, native-call read lock 1회, Pending 1개와
  map insert 1회가 확인된다. Pending은 `CompletableFuture<T>`와 admission
  `CompletableFuture<Void>`를 각각 1개 만든다
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1006-1035`).
  **없앨 수 있는가** — admission/reply `CompletionStage`와 Core token correlation은
  public request contract가 요구한다. lock/map의 구현 교체 가능성은 있으나,
  state를 없애면 두 stage와 close/race 처리가 사라지므로 공개 동작을 보존한다고
  말할 수 없다.

* **무엇** — native request 전에 Java `Message`마다 `zlink_msg_copy`로 native
  header를 scratch array에 staging한다.
  **어디** — `NativeScratch`는 thread-local target/id/completion/parts storage를
  보유한다
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:53-79`);
  part별 copy와 `Native.request` downcall은
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:330-390`)이다.
  **C는 어떻게 하는가** — C는 만들어 둔 `zlink_msg_t`를 바로 `zlink_request`에
  넘긴다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-305`).
  **메시지마다 몇 번인가** — 1-part request이면 `messageCopyTo` 1회와 foreign
  downcall 1회다. 주석은 copy가 refcount 공유로 large payload copy를 피한다고
  명시한다
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:345-348`),
  따라서 **추가 1 KiB byte copy 1회라고 세지 않는다**.
  thread-local scratch의 최초 allocation은 warm-up 뒤 매 request 비용인지 확인하지
  못했다.
  **없앨 수 있는가** — C ABI가 `zlink_msg_t *` array를 받으므로 Java object를
  그대로 전달할 수는 없다. header staging을 더 싸게 표현할 수 있는지는 FFM
  boundary와 `Message` native-layout 계약을 확인해야 한다.

* **무엇** — reply completion을 `Message[]`, `Arrays.asList`, `List.copyOf`로 새
  Java list에 감싸고, perf harness가 `whenComplete` callback을 붙인다.
  **어디** — native reply conversion은
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:700-722`; harness의
  `submission.reply().whenComplete`는
  `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiSocketReqRep.java:383-415`다.
  **C는 어떻게 하는가** — C는 completion `reply_parts`를 직접 검사하고 바로
  close한다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:400-409`).
  **메시지마다 몇 번인가** — normal reply마다 part wrapper는 part 수만큼, array/list
  conversion은 1회씩, harness callback 등록은 1회다. 정확한 `Message` wrapper
  allocation 수는 factory 구현을 이 조사에서 세지 못했다.
  **없앨 수 있는가** — `List<Message>` reply surface를 유지하면 list와 part
  wrappers는 필요하다. immutable-copy 형태와 callback scheduling은 internal
  구현이므로 공개 API 변경 없이 다룰 여지는 있으나 안전한 대안은 확인하지 못했다.

* **무엇** — Java perf server는 reusable `Received`를 쓰지만 request마다
  `received.reply()` builder와 Java message list를 거쳐 echo한다.
  **어디** — receive/echo loop는 `PerfMultiSocketReqRep.java:67-96`이다.
  **C는 어떻게 하는가** — C server는 stack parts를 `zlink_reply`로 바로 전달한다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:879-917`).
  **메시지마다 몇 번인가** — reusable `Received` 자체는 0회 새로 만들지만 public
  reply builder는 1회다. builder의 allocation 여부는 source를 확인하지 못했다.
  **없앨 수 있는가** — reply builder surface를 우회하면 public binding을 재지 않게
  되므로 이 benchmark 범위에서는 제거 대상이 아니다.

### .NET

* **무엇** — 정상 async request도 `RequestCompletionEntry`와 두
  `TaskCompletionSource`를 만들고 dictionary에 등록한 뒤 `_submitSync`으로 native
  submit과 drain을 직렬화한다.
  **어디** — request path는
  `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:119-173`; registry
  insert은 `:401-430`; entry의 reply/admitted TCS는 `:1098-1125`다.
  **C는 어떻게 하는가** — C client은 `completion_id`와 slot pointer를 넘긴다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:290-313`).
  **메시지마다 몇 번인가** — normal request 1회에 `RequestCompletionEntry` 1개,
  reply TCS 1개(상속 base), admitted TCS 1개, dictionary registration 1회,
  `_submitSync` lock 1회가 코드에서 확인된다. public poller drain도 completion
  record마다 `_submitSync` 1회와 `_sync` lock으로 dictionary lookup 1회다
  (`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:268-346`).
  **없앨 수 있는가** — `RequestSubmission`이 `Result`, `Admitted`, `Reply`를
  반환하므로 two-stage completion state는 필요하다. TCS 두 개와 dictionary/lock
  조합을 바꿀 수 있는지는 lifecycle/race contract 검토 없이는 결론 낼 수 없다.

* **무엇** — reply completion을 managed `IReadOnlyList<Message>`로 이동하고,
  completion record를 구조체 초기화·P/Invoke·close까지 관리한다.
  **어디** — drain loop는
  `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:294-346`, reply
  `MoveReply`와 Task settlement는
  `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1267-1321`이다.
  public poller가 event마다 `Drain()`을 호출한다
  (`bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs:284-308`).
  **C는 어떻게 하는가** — C completion record의 reply parts를 header 측정 후
  `zlink_completion_close` 한다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:400-409`).
  **메시지마다 몇 번인가** — reply completion 1개마다 `ZlinkCompletion` stack
  struct 초기화 1회, P/Invoke `zlink_completion_recv` 1회 이상(`NO_DATA`까지),
  dictionary lookup 1회, `zlink_completion_close` 1회다. `MoveReply`가 만드는
  managed array/list와 wrapper 수는 해당 helper를 이 조사에서 세지 못했다.
  **없앨 수 있는가** — managed reply parts의 ownership transfer는 public
  `IReadOnlyList<Message>` contract에 필요하다. native buffer를 그대로 노출하는
  것은 lifetime 계약을 바꿀 수 있다.

* **무엇** — perf harness가 request마다 managed payload allocation, request builder,
  fire-and-forget `ObserveRequestAsync` Task를 만든다.
  **어디** — payload/builder/Async call은
  `bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiSocketReqRep.cs:253-284`,
  reply observer task와 poll turn은 `:286-405`다. server는 reusable `Received`를
  사용하고 received payload를 public reply builder에 넘긴다 (`:440-468`).
  **C는 어떻게 하는가** — C has one frame allocation/copy then direct
  `zlink_request`, and server forwards received part directly (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-309`, `:879-917`).
  **메시지마다 몇 번인가** — `Message.Allocate(payloadSize)` 1회, `RequestSubmission`
  1개, observer async Task invocation 1회가 확인된다. body bytes는 `StampMetricHeader`
  만 쓰므로 C baseline의 `memcpy`와 달리 full 1 KiB copy라고 기록하지 않았다.
  **없앨 수 있는가** — observer task는 bench metric 처리이고 payload allocation은
  public `Message` 입력을 만드는 과정이다. 이들을 제품 binding 결함이라고
  분류할 근거는 없다.

### Node

* **무엇** — 정상 request마다 JavaScript `CompletionEntry` 하나와 reply/admitted
  Promise 둘, token map과 completion-id map을 만든다.
  **어디** — entry constructor와 Promise 생성은
  `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:105-220`; normal
  submit path의 entry creation/publish/map insert은 `:350-400`, map fields는
  `:224-234`다.
  **C는 어떻게 하는가** — C는 Core user context로 slot pointer 하나를 사용하고
  reply state를 별도 heap map에 보관하지 않는다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:290-313`, `:400-409`).
  **메시지마다 몇 번인가** — normal request 1회에 entry 1개, Promise 2개, token map
  insert 1회, completion-id map insert 1회가 확인된다. source는 JS object allocation
  수를 더 세는 근거를 제공하지 않는다.
  **없앨 수 있는가** — Node public `RequestSubmission`은 two-stage Promise를
  명시한다 (`bindings/node/src/zlink/contracts/messaging/operations.ts:53-68`).
  따라서 entry/promise semantics는 없앨 수 없고, map representation만의 대체
  가능성은 close/early-completion rule을 포함해 별도 검토가 필요하다.

* **무엇** — native addon으로 넘길 request payload를 `normalizeOperationPayload`로
  바꾸고, completion의 `Buffer[]`를 `Message[]`로 바꾼다.
  **어디** — native request call은
  `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:350-399`; native
  reply parts conversion은
  `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:172-193`의
  `messagesFromNativeBuffers`다. C API 호출과 `DONTWAIT`/token 전달은 addon
  (`socketSubmitRequest`) 경계 뒤라 이 TypeScript file에는 보이지 않는다.
  **C는 어떻게 하는가** — C는 `zlink_msg_t`를 직접 전달하고 completion reply
  pointer를 직접 읽는다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-305`, `:400-409`).
  **메시지마다 몇 번인가** — normal path에 normalize 1회와 native-addon call 1회,
  reply마다 conversion 1회가 확인된다. normal path에서 Buffer body를 다시 1 KiB
  복사하는지는 `normalizeOperationPayload`와 addon implementation을 이 조사에서
  끝까지 확인하지 못해 세지 않았다. backpressure에서만 `Buffer.from`/`toBytes`로
  retry snapshot을 만든다
  (`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:417-435`,
  `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:72-82`).
  **없앨 수 있는가** — JS Buffer와 C ABI frame 표현의 경계는 필요하다. zero-copy
  가능성은 Node addon의 Buffer lifetime/finalizer contract을 확인하지 않아 판단하지
  못했다.

* **무엇** — perf harness가 request마다 `Buffer.from(template)`, operation builder,
  async `collectReply` Promise와 `Set`/`Map` bookkeeping을 만든다. 또한 매 bounded
  turn에 `await sleepImmediate()`로 event loop를 양보한다.
  **어디** — `bindings/node/perf/multi/perf_multi_socket_reqrep.ts:97-167`.
  **C는 어떻게 하는가** — C는 slot payload를 `memcpy`로 message에 넣고 같은
  thread에서 poll/drain한다 (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:256-305`, `:576-603`).
  **메시지마다 몇 번인가** — Buffer copy 1회, request builder 1회, `collectReply`
  async task 1회, pending Set insert/delete 1회씩이 확인된다. `sleepImmediate`는
  request마다가 아니라 socket sweep마다 1회다 (`:119-167`).
  **없앨 수 있는가** — Buffer copy와 task bookkeeping은 Node perf harness에
  명시돼 있다. `sleepImmediate`는 completion Promise continuation을 진행시키는
  turn 경계이므로 이를 제거하면 C와의 동작 동등성을 이 문서만으로 보장할 수 없다.

* **무엇** — completion drain은 JS loop에서 native completion을 하나씩 받아 map
  lookup/capture하고, public poller가 그 drain을 호출한다.
  **어디** — drain loop는
  `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:524-540`, poller hook은
  `bindings/node/src/zlink/runtime/eventing/poller.ts:193-205`다.
  **C는 어떻게 하는가** — C는 matching slot을 직접 검사해 reply를 record한다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:361-412`).
  **메시지마다 몇 번인가** — completion 1개마다 native `socketCompletionRecv` 1회,
  token/id map lookup 1회, Promise settle 1회다; drain 끝에는 extra `NO_DATA` native
  call 1회가 있다. JS source에 explicit mutex는 없다.
  **없앨 수 있는가** — Promise correlation/settlement는 public async surface에
  필요하다. C-like slot array로 대체해도 JS concurrency/lifecycle semantics가
  유지되는지는 확인하지 못했다.

## 3. 네 언어에 공통인 것

있다. 네 binding 모두 C Core의 request completion id를 언어 runtime의 two-stage
async API로 바꾸기 위해, **정상 admission에서도** request state를 만든다. C++는
bundle/entry, Java는 `Pending`, .NET은 `RequestCompletionEntry`, Node는
`CompletionEntry`다. 모두 Core completion을 language object로 바꾸고 user-context
또는 completion-id로 대응시킨다. C baseline도 Core pending request는 만들지만,
하네스는 slot pointer와 counters로 completion을 처리하며 language future/promise와
object registry를 만들지 않는다.

네 binding 모두 C ABI 호출 전에 언어 객체를 native frame/header 표현으로 바꾸고,
reply 때 native reply parts를 언어 message collection으로 바꾼다. C++/Java/.NET은
명시적인 lock을 갖고 Node는 single event loop와 Map/Promise로 같은 correlation을
관리한다. 이는 "언어라서 느리다"라는 일반론이 아니라 위 각 파일의 completion
ownership·conversion 코드에 있는 구체적인 경로다.

## 4. binding이 아니라 다른 곳일 가능성

* **Core/transport** — 네 binding과 C 모두 `zlink_request`, completion poller,
  `zlink_completion_recv`, `zlink_router_recv`, `zlink_reply`를 사용한다. 따라서
  동일한 Core shared library, socket options, transport와 client 수로 실제 실행됐다면
  Core의 공통 admission/transport 비용만으로 언어별 비율 차이를 설명하기는 어렵다.
  하지만 각 runner가 같은 local Core binary/package를 실제 로드했는지는 이
  read-only 조사에서 확인하지 않았다.
* **측정 하네스** — C++/Java/.NET/Node 하네스도 각자의 public async API, metric
  callback/task, payload representation을 매 request에 사용한다. Node는 Buffer copy와
  `sleepImmediate`, .NET은 observer Task, Java는 `whenComplete`, C++는 detached
  coroutine을 명시적으로 만든다. 이는 제품 binding 내부와 독립된 하네스 코드에도
  비용이 있음을 뜻한다. 다만 네 하네스가 C의 submit/poll/drain 순서를 따르도록
  작성되어 있으므로, 이 사실만으로 하네스 결함이라고 결론 내릴 수는 없다.
* **language runtime** — Java의 FFM downcall/GC, .NET P/Invoke/Task scheduling/GC,
  Node addon boundary·V8 GC·event-loop turn, C++ allocator/coroutine scheduling은
  source에서 실제 호출 경로가 확인된다. 그러나 GC pause, JIT warm-up, worker
  migration, event-loop utilization, allocator contention의 크기는 코드만으로
  측정할 수 없다.
* **server도 포함한 end-to-end 차이** — C server는 received native part를 직접
  reply한다. 각 binding server는 public `Received`/reply builder를 통과한다. 따라서
  표의 KOPS가 client submit만의 비용이 아니라 server wrapper와 runtime까지 포함한
  왕복 비용이라는 점은 확실하다.

## 5. 확인 못 한 것

* 제공된 603.4 KOPS와 각 ratio를 재실행하거나 profile하지 않았다. 따라서 §2의
  항목별 시간, allocation bytes, GC/JIT 비중, mutex contention, 실제 syscall 수는
  확인하지 못했다.
* `request-backpressure` 실행 시의 `PERF_PART_COUNT`, 정확한 transport, client 수,
  Core library path/version, applied HWM과 backpressure 비율을 이 조사에서 확인하지
  못했다. backpressured retry copy/lock은 normal path와 달라 이 값이 중요하다.
* C++ submit staging의 payload-copy 여부, Java `Pending`의 정확한 object 수,
  .NET `MoveReply`의 exact allocation 수, Node `normalizeOperationPayload`와 native
  addon의 Buffer copy/lifetime은 관련 helper/addon 구현을 완주하지 않아 숫자로
  세지 않았다.
* Core 내부와 transport backend의 allocation·lock·syscall 수는 C API source의
  public boundary까지만 확인했다. transport별 trace 또는 profiler 없이는 request
  한 건의 실제 syscall 수를 정할 수 없다.
