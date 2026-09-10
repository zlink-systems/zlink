# 바인딩 비동기 submit 종결자가 Core 제출 결과(enum)를 돌려준다 — draft

> 작성일: 2026-09-10. 상태: 사용자 결정(호환성 없이 변경, 바인딩 7언어 동시, perf는 `BACKPRESSURED`일 때만 비동기 대기).
> 적용 계획: [`../plan/bindings-submit-result-terminal-plan.ko.md`](../plan/bindings-submit-result-terminal-plan.ko.md).
> 선행: Issue #63 / PR #86(whole-message API)이 먼저 main에 들어간 뒤 그 위에서 진행한다.
> 배경 기록: `doc/plan/fw-bench-worklog/decisions.ko.md` FB-065, Issue #12·#13.

## 1. 배경·문제

Core C API의 send·request 제출 함수는 결과 enum `zlink_submit_result_t`를 돌려준다
(`core/include/zlink/socket/api.h`, PR #86 뒤 `zlink_send`·`zlink_send_rid`·`zlink_request`).
`DONTWAIT` 제출이 HWM에 닿으면 `ZLINK_SUBMIT_BACKPRESSURED` + 대기 토큰이 돌아오고, credit이 회복되면
`ZLINK_COMPLETION_WRITABLE` record가 completion queue에 들어온다(0.17 계약 B, D-B79/D-B85).
C perf reference는 이 enum을 그대로 써서 "`BACKPRESSURED`까지 연속 제출하고 WRITABLE에서 재개"한다
(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:320-335, :576-597`).

C 외 바인딩 7종(cpp·dotnet·go·java·node·python·rust)은 이 결과를 호출자에게 주지 않는다. 스펙
"Submit 결과 투영"(`bindings/doc/spec/README.ko.md` `#submit-result-projection`)이 `BACKPRESSURED`를
"바인딩이 입력을 보관하고 WRITABLE을 기다린다"로 정의하고, 구현은 즉시 admission과 대기 중을 **같은
terminal 하나**로 합친다. 2026-09-10 main(8d4c6680f7) 기준:

| 언어 | request 종결자 | HWM에서 | 호출자가 보는 것 |
|---|---|---|---|
| Java | `CompletionStage<List<Message>> submit()` | payload 보관·토큰 arm·같은 future 반환 (`CompletionOwner.java:157-206`) | reply까지의 stage 하나 |
| .NET | `Task<IReadOnlyList<Message>> Async()` | 같음 (`CompletionOwner.cs:185-196`) | Task 하나 |
| Node | `submit(): Promise<Message[]>` | 같음 (`completion_owner.ts:352-372`) | Promise 하나 |
| C++ | `async_result_t<std::vector<message_t>> async() &&` | 같음 (`completion_owner.cpp:242-252`) | awaitable 하나 |
| Go | `Submit(ctx) ([]*Message, error)` | 같음 (perf 주석 `perf_multi_socket_reqrep.go:266`) | 반환값 하나 |
| Rust | `submit() -> impl Future<Output = Result<Vec<Message>, ZlinkError>>` | `SendAttempt::Waiting(token)` 내부 분기 (`send_ops.rs:398`) | future 하나 |
| Python | `submit() -> Awaitable[list[Message]]` | send/publish 경로는 `SubmitError(BACKPRESSURED)`를 던짐 (`socket_base_impl.py:222, :278`) | 예외 또는 awaitable |

**send**는 stage가 admission에서 완료되므로 "반환 시점에 이미 완료됐는가"로 HWM 도달을 간접 판정할 수
있고, Java perf `AdmissionRoundRobin`이 그렇게 socket당 미완료 admission 1건을 유지한다
(`PerfMultiRoutedSendCoordinator.java:250-279`). **request**는 stage가 reply에서 완료되므로 즉시
admission과 HWM 대기가 반환 시점에 구분되지 않는다. 그 결과:

- perf `PerfMultiSocketReqRep`는 turn당 socket당 1건 제출 뒤 완료 poll(≤50 ms)로 pacing한다. socket이
  100개면 깊이 100이지만 socket이 1개면 깊이 1(request-serial과 같음).
- gRPC 비교 벤치의 raw request-backpressure 행(socket 1개)은 8k/s에 머문다. 깊이 실험은 같은 socket
  하나로 HWM 아래 깊이 699에서 361k/s를 냈고, HWM을 넘겨 토큰을 수만 건 쌓으면 timeout으로 무너졌다
  (`.artifacts/codex/java-raw-depth-ladder/`). 즉 producer가 멈출 지점을 알 수 없는 것이 원인이다.
- 정책(`doc/perf/PERF_MULTI_TEST_POLICY.md` request/reply 클라이언트: "admission backpressure를 만날
  때까지 연속 제출")을 C 외 언어가 그대로 구현할 수 없다.

Core에는 문제가 없다. 결과 정보는 native 경계에서 이미 있고 바인딩이 버린다.

## 2. 목표

1. 비동기 submit 종결자가 **제출 시점 Core 결과를 enum으로** 돌려준다. `OK`(즉시 admission) 또는
   `BACKPRESSURED`(대기 토큰, 바인딩이 재제출 소유).
2. `BACKPRESSURED`일 때만 호출자가 **admission stage**를 기다린다. `OK`면 기다릴 것이 없다(perf 규칙).
3. request는 admission stage와 reply stage를 **따로** 준다.
4. payload 보관·재제출·토큰 수명은 지금처럼 **바인딩이 소유**한다. 호출자에게 payload가 되돌아가는
   "try" 계열 API는 만들지 않는다(사용자 결정).
5. 호환성은 유지하지 않는다(1.0 전). 7언어를 함께 바꾼다.
6. `submit_sync()`(blocking)·publish(`void`)·reply(동기 one-shot)는 바꾸지 않는다.

## 3. 공통 계약

### 3.1 결과 enum

기존 `SubmitResult`(언어별 이름은 §4)를 재사용한다. 종결자가 돌려주는 값은 두 개뿐이다.

| 값 | 뜻 | admission stage | 이후 |
|---|---|---|---|
| `OK` | Core가 즉시 admission. SEND는 ID `0`·completion 없음, REQUEST는 nonzero ID | 이미 완료 | REQUEST는 reply stage가 completion에서 완료 |
| `BACKPRESSURED` | Core가 대기 토큰을 발급. 바인딩이 입력을 보관하고 WRITABLE에서 재제출 | 재제출이 admission되면 완료. 재제출도 거절되면 새 토큰으로 계속(호출자에게는 보이지 않음) | REQUEST는 admission 뒤 reply stage |

그 밖의 submit 실패(`NOT_CONNECTED`, `NOT_FOUND`, `NOT_ADMITTED`, `INVALID_ARGUMENT`, `TERMINATED`,
`OUT_OF_MEMORY`, `INTERNAL_ERROR` …)는 지금처럼 **예외/에러 반환**으로 둔다. 결과 객체와 예외 두 곳을
보게 하지 않기 위해서다(Go·Rust는 `error`/`Result`로 같은 자리).

### 3.2 결과 객체

```text
SendSubmission
  result   : SubmitResult            // OK | BACKPRESSURED, 제출 시점 스냅샷
  admitted : Stage<void>             // OK면 완료 상태로 반환

RequestSubmission
  result   : SubmitResult
  admitted : Stage<void>
  reply    : Stage<List<Message>>    // 지금 submit()이 돌려주던 것
```

- `result`는 종결자 반환 시점 값이며 바뀌지 않는다. 그 뒤의 재제출 결과는 `admitted`로만 본다.
- `admitted`가 실패로 끝나는 경우: 토큰이 `SEND_TERMINAL`(target 제거·close·termination)로 끝나거나
  재제출이 대기 토큰 없는 실패를 받은 경우. 지금 terminal이 실패하는 조건과 같다.
- REQUEST의 `reply`는 `admitted` 성공 뒤에만 완료될 수 있다. `admitted`가 실패하면 `reply`도 같은
  원인으로 실패한다(정확히 한 번).
- 완료 순서: 합류 규칙은 `async-execution-model.ko.md` §5 그대로. `result` 스냅샷과 completion이
  경합해도 terminal은 정확히 한 번 끝난다.

### 3.3 왜 stage 둘인가

producer가 필요한 신호는 "다음을 내도 되는가"(admission)이고, consumer가 필요한 것은 "응답"(reply)이다.
둘을 하나로 합치면 producer는 응답을 기다리게 되어 socket당 깊이가 1이 된다(§1). C API가 두 신호를
`zlink_submit_result_t`(admission)와 `zlink_completion_t`(reply)로 나누듯, 바인딩도 나눈다.

## 4. 언어별 시그니처 (초안)

`Stage`는 각 언어의 관용 비동기 타입이다. **종결자 이름은 `bindings/doc/spec/async-coroutine-policy.ko.md` §6 표를 그대로
쓴다**(비동기: Java·Node·Python·Rust `submit()`, .NET `Async()`, C++ `async()`; 동기 종결자 불변). 바뀌는 것은 비동기
종결자의 **반환형**뿐이며, 결과 객체·필드 이름(`Submission`·`result`·`admitted`·`reply`)은 7언어가 공유한다.

### Java

```java
public interface SendSubmission {
    SubmitResult result();
    CompletionStage<Void> admitted();
}
public interface RequestSubmission {
    SubmitResult result();
    CompletionStage<Void> admitted();
    CompletionStage<List<Message>> reply();
}
// SendSubmitOperation
SendSubmission submit();            // 이전: CompletionStage<Void>
void submit_sync();                 // 불변
// RequestSubmitOperation
RequestSubmission submit();         // 이전: CompletionStage<List<Message>>
List<Message> submit_sync();        // 불변
```

Kotlin suspend 표면은 `admitted()`·`reply()`를 `await()`하는 확장으로 대응한다.

### .NET

```csharp
public readonly struct SendSubmission { SubmitResult Result; Task Admitted; }
public readonly struct RequestSubmission { SubmitResult Result; Task Admitted; Task<IReadOnlyList<Message>> Reply; }
SendSubmission    SendSubmitOperation.Async(CancellationToken ct = default);     // 이전: Task
RequestSubmission RequestSubmitOperation.Async(CancellationToken ct = default);  // 이전: Task<IReadOnlyList<Message>>
void / IReadOnlyList<Message> Submit();                                           // 불변
```

`ct`는 `Admitted`·`Reply` 둘 다에 적용한다. 기존 `TrySubmit()`(send·reply, `OperationContracts.cs:54`)은 **제거**한다(사용자 결정). `Result == BACKPRESSURED`가 그 역할을 대신한다.

### Node

```ts
interface SendSubmission    { result: SubmitResult; admitted: Promise<void>; }
interface RequestSubmission { result: SubmitResult; admitted: Promise<void>; reply: Promise<Message[]>; }
submit(): SendSubmission;      // 이전: Promise<void>
submit(): RequestSubmission;   // 이전: Promise<Message[]>
submit_sync(): void / Message[];  // 불변
```

`result`는 동기 필드다. Promise를 한 번 더 감싸지 않는다.

### C++

```cpp
struct send_submission_t {
    zlink_submit_result_t result;
    async_result_t<void> admitted;
};
struct request_submission_t {
    zlink_submit_result_t result;
    async_result_t<void> admitted;
    async_result_t<std::vector<message_t>> reply;
};
send_submission_t    send_operation_t::async () &&;      // 이전: async_result_t<void>
request_submission_t request_operation_t::async () &&;   // 이전: async_result_t<std::vector<message_t>>
void / std::vector<message_t> submit () &&;              // 불변
```

### Go

```go
type SendSubmission struct {
    Result   SubmitResult
    Admitted <-chan error          // 닫히거나 error 하나
}
type RequestSubmission struct {
    Result   SubmitResult
    Admitted <-chan error
    Reply    <-chan RequestReply   // {Parts []*Message; Err error}
}
func (b *sendBuilder)    Submit(ctx context.Context) (SendSubmission, error)     // 이전: error
func (b *requestBuilder) Submit(ctx context.Context) (RequestSubmission, error)  // 이전: ([]*Message, error)
```

Go의 종결자 이름은 정책(`async-coroutine-policy.ko.md` §6)대로 `Submit(context.Context)` 하나만 둔다. 새 이름을 만들지
않는다. 제안은 `Submit(ctx)`가 결과 객체를 돌려주고 reply는 채널로 받는 것이며(`<-sub.Reply`가 지금의 블로킹 결과),
정책 §6 Go 행을 그렇게 고친다. 사용자 확인 대기(plan §2 #5).

### Rust

```rust
pub struct SendSubmission {
    pub result: SubmitResult,
    pub admitted: impl Future<Output = Result<(), SubmitError>> + Send,
}
pub struct RequestSubmission {
    pub result: SubmitResult,
    pub admitted: impl Future<Output = Result<(), SubmitError>> + Send,
    pub reply:    impl Future<Output = Result<Vec<Message>, ZlinkError>> + Send,
}
pub fn submit(self) -> Result<SendSubmission, SubmitError>;      // 이전: impl Future<Output = Result<(), SubmitError>>
pub fn submit(self) -> Result<RequestSubmission, ZlinkError>;    // 이전: impl Future<Output = Result<Vec<Message>, ZlinkError>>
pub fn submit_sync(self) -> …;                                    // 불변
```

`impl Trait` 필드는 안정 Rust에서 불가하므로 `Pin<Box<dyn Future<Output = …> + Send>>`로 시작한다(plan §2 #7).
`SubmitRetryMode`(`socket_options.rs:64`)는 local failure 재시도 옵션이며 backpressure와 무관하다. 그대로 둔다.

### Python

```python
class SendSubmission:    result: SubmitResult; admitted: Awaitable[None]
class RequestSubmission: result: SubmitResult; admitted: Awaitable[None]; reply: Awaitable[list[Message]]
def submit(self) -> SendSubmission        # 이전: Awaitable[None]
def submit(self) -> RequestSubmission     # 이전: Awaitable[list[Message]]
def submit_sync(self) -> …                # 불변
```

send/publish 경로가 `SubmitError(BACKPRESSURED)`를 던지던 동작(`socket_base_impl.py:222, :278`)은
send에서는 `result == BACKPRESSURED`로 바뀐다. publish는 `void`·완료 없음이 스펙이므로 그대로 둔다(예외 유지).

## 5. perf 규칙 변경

`doc/perf/PERF_MULTI_TEST_POLICY.md` §1.1 "C 이외의 binding" 문단과 §1.2 request/reply 클라이언트 항목을
아래로 바꾼다. 의미는 C reference와 같아진다.

- **send**: `result == OK`면 즉시 다음 제출. `BACKPRESSURED`면 그 socket만 `admitted`를 기다렸다가 재개.
  다른 socket의 제출은 계속한다.
- **request**: 같은 규칙. reply는 `reply` stage로 별도 진행하며 제출을 막지 않는다. socket당 깊이는
  HWM(byte)이 정한다. **turn당 1건 pacing·완료 poll 대기는 없앤다.**
- 비동기 대기는 `BACKPRESSURED`일 때만 한다. `OK`에서 `admitted`를 기다리지 않는다.

같은 규칙이 gRPC 비교 벤치(`framework/bench/grpc`)의 raw 행 request-backpressure·send-saturation에 적용된다
(규격 §2의 "admission backpressure를 만날 때까지 연속 제출, 재개 신호에서 이어서 제출"이 그대로 구현된다).

## 6. 수정 대상

### 6.1 스펙·정책 (감독자)

- `bindings/doc/spec/README.ko.md` / `.en.md`: "Submit 결과 투영" 표 — `BACKPRESSURED` 행을 "바인딩이
  입력을 보관하고 WRITABLE을 기다리며, **종결자는 `BACKPRESSURED`와 admission stage를 돌려준다**"로.
  send·request terminal 정의 절(언어별 §)에 결과 객체 추가. 도메인 모델 목록(`SubmitResult` 항목)의
  "exception 언어에서는 예외 `.code`로 노출" 문장 수정.
- `bindings/doc/spec/async-execution-model.ko.md` §5: 결과 객체의 `result` 스냅샷과 completion 합류 규칙 한 문단.
- 언어별 `bindings/doc/spec/<lang>/README.*`: terminal 절.
- `doc/perf/PERF_MULTI_TEST_POLICY.md` §1.1·§1.2 (§5).
- `framework/bench/grpc/README.{ko,en}.md`(= `framework/doc/framework/common/bench/with-grpc-local.*`) §2·§10.3:
  request-backpressure 구현 규칙 한 문장.

### 6.2 바인딩 코드 (언어별 job)

| 언어 | 종결자 | 내부 |
|---|---|---|
| Java | `SendSubmitOperation`, `RequestSubmitOperation`, Kotlin 확장 | `CompletionOwner.submitSend/submitRequest`가 `Pending`에서 `admitted` future를 따로 완성. 재제출 성공(`retrySend`/`retryRequest` OK) 시 `admitted` 완료 |
| .NET | `OperationContracts.cs` `Async()` | `CompletionOwner.SendAsync/RequestAsync`, entry의 `Arm*`에 admitted TCS |
| Node | `operations.ts` `submit()` | `completion_owner.ts` entry에 admitted resolver |
| C++ | `operation_contracts.hpp` `async()` | `completion_owner.cpp` `_request_waiting_writable` 전이에서 admitted 완료 |
| Go | `operations.go` `Submit` | writable retry에서 Admitted 채널 완료 |
| Rust | `operations.rs` `submit` | `send_ops.rs`/`routed_async.rs` `SendAttempt::Waiting` 경로에 admitted future |
| Python | `operations.py` `submit` | `socket_base_impl.py` bridge가 `(result, awaitable)` 반환 |

### 6.3 framework (사용자 결정 2026-09-10, plan §6.1)

- **F1**: framework 메시징 공개 terminal은 backpressure를 노출하지 않는다(01 §5 유지). framework 내부가 binding 결과 객체를 소비해
  3단계 backpressure(04 §8)를 "`BACKPRESSURED`일 때만 `admitted` 대기"로 정확히 구현한다.
- **F2**: framework 메시징 call에 동기 **blocking** 종결자를 추가한다. 이름은 binding 정책 §6(Java·Node `submit_sync()`, .NET `Submit()`,
  C++ `submit()`). nonblocking try·callback 종결자는 만들지 않는다.
- **F2-a**: blocking 종결자를 handler turn·Spot turn·state lane 등 runtime 실행 문맥에서 부르면 `InvalidOperation`.

### 6.4 호출부

- framework 4언어의 binding 호출부(예 Java `ZLinkJavaRawServicePort.java:211-219`): `submit()` → `.reply()`/`.admitted()`.
  공개 의미 변화 없음(F1). .NET `TrySubmit()` 호출부 2곳 포함. 위치는 plan §6.
- bindings perf multi·single의 send·request 클라이언트 7언어(§5).
- gRPC 벤치 raw 드라이버 4언어(Java `RawStack`·`BenchDrivers.runRaw` 외).
- samples·tests·guide 코드 블록.

## 7. 리스크·주의

- **정확히 한 번**: `admitted`와 `reply`가 같은 실패를 두 번 보고하거나 한쪽만 끝나면 안 된다. 기존 contract
  test(`async-execution-model` §7)에 결과 객체 두 stage의 완료 횟수 검사를 추가한다.
- **스냅샷 경합**: `result == BACKPRESSURED`를 돌려준 직후 WRITABLE이 이미 도착해 `admitted`가 완료돼 있을 수 있다.
  허용한다(호출자는 `admitted`를 기다리면 된다).
- **Go 채널 소유권·GC**: 채널을 읽지 않는 호출자가 있어도 goroutine이 새지 않게 buffered(1)로 둔다.
- **Python sync bridge**: 현재 `_publish_payload_via_native_bridge`가 bool을 돌려주는 구조를 `(result, awaitable)`로 넓힌다.
- **Rust 타입**: `impl Trait` 필드 불가 → boxed future 또는 명명 타입. 성능 영향은 perf로 확인.
- **framework 성능**: framework 호출부는 `.reply()`만 쓰므로 hot path 영향 없음. 단 결과 객체 할당 1회가 추가된다.
  Java는 `record`로 두고 escape analysis에 맡긴다. 08장 E1~E5 재측정으로 확인.
- **호환성 없음**: 1.0 전 변경. 릴리스 노트에 breaking으로 적는다.

## 8. 성공 기준

1. 7언어 contract test에 "즉시 admission → `result == OK`, `admitted` 완료 상태", "HWM 도달 → `BACKPRESSURED`,
   WRITABLE 뒤 `admitted` 완료, request는 그 뒤 `reply`" 두 시나리오가 있고 통과한다.
2. bindings perf multi ROUTER_ROUTER_REQREP(tcp, 1024) clients=1이 깊이 1을 벗어나 clients=100과 같은 자릿수
   (기준 2026-09-10: clients=100 248k/s, clients=1 8.5k/s)를 낸다. 오류·timeout 0.
3. gRPC 벤치 raw request-backpressure(Java, socket 1개)가 warmup 포기 0·오류 0으로 3-run 완료되고 처리량이
   깊이 실험 K=100 수준(수십만/s)에 있다. framework/raw 비율은 같은 루프 모양에서 잰다.
4. 기존 perf 전 패턴 회귀 없음(사이즈별 −5% 이내, D-B 판정 기준).
