# Go·Rust return-based parity inventory

> 대상 독자는 Go와 Rust bindings의 공개 시그니처와 error mapping을 구현·검토하는 개발자다. 이 문서는
> “두 언어의 문법은 달라도 어떤 성공, 실패, no-data와 ownership 의미를 같게 검증해야 하는가?”에 답한다.

## 1. 문서 역할

반환과 error 의미의 정본은 [공통 bindings spec](../spec/README.ko.md)의 `오류 처리 정책` 절이다. 이
문서는 Core 0.9.0 최신화 과정에서 대응 메서드를 비교하고 evidence를 연결하는 inventory다. 정식 계약을
추가하거나 변경하지 않는다. 이전에 작성된 submit 반환 설계 후보는 공통 승인을 받지 못해 삭제했으며,
현재 구현과 정식 spec에 없는 반환 형태를 구현 지시로 사용하지 않는다.

정식 계약을 변경해야 하면 공통 contract governance에 따라 설계 review와 승인을 먼저 진행한다. 구현과
contract test가 통과한 뒤 공통 spec, [Go spec](../spec/go/README.ko.md)과 [Rust spec](../spec/rust/README.ko.md)의
한국어·영문 문서를 함께 갱신한다.

## 2. 공통 반환 의미

| 의미 | Go 표현 | Rust 표현 |
|------|---------|-----------|
| 성공 값 없음 | `error`가 `nil` | `Ok(())` |
| 성공 값 있음 | `(T, nil)` | `Ok(T)` |
| 값이 없을 수 있는 조회 | `(zero, false, nil)` | `Ok(None)` |
| non-blocking 수신 결과 없음 | `(false, nil)` | `Ok(false)` |
| non-blocking submit backpressure | 현재 `(false, nil)` | 현재 `Ok(false)` |
| 성공 값 없는 submit | operation별 public signature를 따름 | operation별 public signature를 따름 |
| 단일 함수군 실패 | `error`의 실제 값이 함수군별 concrete error | `Err(<Category>Error)` |
| 여러 함수군을 조합한 실패 | 실제 값은 함수군별 concrete error이며 모두 `ZlinkError` interface 구현 | `Err(ZlinkError)` |

Caller-provided receive의 no-data와 submit backpressure는 별도 의미로 기록한다. 현재 submit 표면은 Go
`(bool, error)`와 Rust `Result<bool, SubmitError>`를 사용하며, 이 반환 shape를 변경할지는 아직 parity
결정이 아니다. 실제 Core 실패는 `false`, `None` 또는 zero value로 숨기지 않는다. 동일한 Core 작업에서
성공하거나 실패했을 때 message part와 handle ownership도 두 언어에서 같아야 한다.

## 3. 함수군과 입력 검증

Core API가 반환하는 result enum이 함수군을 정한다.

| 함수군 | Go 실제 error type | Rust error type | 대표 작업 |
|--------|--------------------|-----------------|-----------|
| Submit | `*SubmitError` | `SubmitError` | send, publish와 request submit |
| Request | `*RequestError` | `RequestError` | request completion |
| Recv | `*RecvError` | `RecvError` | receive, subscription event와 monitor receive |
| Handler | `*HandlerError` | `HandlerError` | callback handler 등록 |
| Close | `*CloseError` | `CloseError` | close와 destroy |
| Bind | `*BindError` | `BindError` | bind |
| Connect | `*ConnectError` | `ConnectError` | connect, disconnect와 unbind |
| Config | `*ConfigError` | `ConfigError` | option, poller와 timer 설정 |

Bind endpoint 형식처럼 Core 작업의 인자를 검사하다 실패하면 함수군의 `INVALID_ARGUMENT`를 사용한다.
Core 작업과 독립된 값 객체를 만들기 전에 형식만 검사할 때만 별도 validation error를 사용할 수 있다.

Go의 공개 시그니처는 단일·복합 경계 모두 `error`를 반환한다. 실제 값은 함수군별 concrete error를 유지하며
`ZlinkError`, `Code()`, `InternalErrno()`와 `errors.As`를 지원한다. 복합 경계의 GoDoc은 발생 가능한 함수군을
나열한다. 권고안에서 `context.Context`를 받는 terminal method는 이를 첫 인자로 받고 실제 cancellation과 deadline에
사용한다. Context를 builder에 저장하거나 이름 없는 인자로 버리지 않는다. Rust의
함수군별 error는 `std::error::Error`, `code()`와 `internal_errno()`를 지원하며 단일 함수군 메서드의
`Result`에 직접 나타난다. 기존 `NativeErrno()`와 `native_errno()`는 major 전환에서 제거하고 호환 alias를
남기지 않는다.

## 4. 대응 API inventory

아래 표는 PGR-COMMON-03에서 Core 0.9.0 raw 공개 API를 대조해 채운다. 한쪽 메서드가 없으면 `GAP`으로 표시하며,
private helper나 test-only adapter로 메우지 않는다.

| Core 작업 | Go 현재 surface | Rust 현재 surface | 성공·no-data 의미 | Error 함수군 | Ownership | 상태 | Evidence |
|-----------|----------------|------------------|--------------------|--------------|-----------|------|----------|
| Context 생성·종료 | `NewContext`; `Context.Close`, `Shutdown`, `Options` | `Context::new`, `shutdown`, `options`; `Drop` | 생성은 `(*Context, nil)` / `Ok(Context)`이고 종료는 값 없이 성공한다 | Config·Close | 호출자가 Context를 소유한다. 종료 시 아직 열린 socket도 정리한다 | `CURRENT`; parity gate 대기 | `bindings/go/internal/native/context.go`, `bindings/rust/src/contracts/core/context.rs` |
| Message 생성·copy·move·close | `NewMessage*`; `Message.Clone`, `Copy`, `Close` | `Message::{new, with_size, try_from, try_clone}`; RAII `Drop` | 생성은 handle 또는 `Result<Message>`를 반환하고, copy는 독립 payload를 만든다 | Config | 송신 성공 시 builder가 part를 소비하고, 수신 envelope가 받은 part를 소유한다 | `CURRENT`; ownership parity 대기 | `bindings/go/internal/native/message.go`, `bindings/rust/src/contracts/messaging/message.rs` |
| Socket bind | `Socket.Bind`, `Unbind`, `Close` | `bind`, `unbind`, `close` | 성공 값 없이 반환한다 | Bind·Connect·Close | socket handle이 endpoint 설정과 native resource의 수명을 관리한다 | `CURRENT`; parity gate 대기 | `bindings/go/internal/native/socket_core.go`, `bindings/rust/src/contracts/sockets/message_socket_contracts.rs` |
| Socket connect·disconnect | `Socket.Connect`, `Disconnect`, `DisconnectRID` | `connect`, `disconnect`, `disconnect_rid` | 성공 값 없이 반환한다. 연결 완료는 monitor/eventing에서 관찰한다 | Connect | 연결 요청은 socket이 관리하며 호출자는 endpoint 문자열을 보관할 책임이 없다 | `CURRENT`; parity gate 대기 | `bindings/go/internal/native/socket_core.go`, `bindings/rust/src/contracts/sockets/message_socket_contracts.rs`, `routed_socket_contracts.rs` |
| Multipart send·publish | `Send`/`Publish` → `Message`·`MoveMessage`·`Bytes`·`Flags`·`Submit(ctx) (bool, error)` | `send`/`publish` → `message`·`flags`·`submit() -> Result<bool, SubmitError>` | 현재 구현은 submit 단계의 boolean과 error를 함께 사용한다 | Submit | 성공 시 submit builder가 추가한 message를 소비한다. 실패 시 보존 조건은 contract test로 확인한다 | `CURRENT`; 반환 parity 결정 대기 | `operations.go`, `spot_operations.rs` |
| Non-blocking submit backpressure | `Flags(DontWait)` 뒤 `(bool, error)` | `flags(DONT_WAIT)` 뒤 `Result<bool, SubmitError>` | 현재 구현은 boolean으로 즉시 미수락을 구분한다 | Submit | submit 시점까지 builder가 part ownership을 관리한다 | `CURRENT`; error mapping parity 대기 | `operations.go`, `spot_operations.rs` |
| Multipart receive | `Recv`, `RecvPart`, `Subscribe`, `ReceiveSubscriptionEvent` | `recv`, `subscribe`, `receive_subscription_event` | caller-provided receive의 no-data는 Go `(false, nil)`, Rust `Ok(false)`다 | Recv | 호출자가 output envelope/message를 재사용하고 수신 결과를 소유한다 | `CURRENT`; no-data parity 확인 대기 | `socket_direct.go`, `socket_subscribe.go`, `bindings/rust/src/contracts/sockets/*.rs` |
| Request submit·completion | `Request` → `Message`·`Bytes`·`Timeout`·`Submit(ctx, callback) (bool, error)` 또는 `SubmitAsync` | `request` → `message`·`timeout`·`submit(callback) -> Result<(), SubmitError>` | submit 접수와 reply completion은 별도 결과다 | Submit·Request | callback/completion이 받은 reply parts를 소유하며 late completion은 폐기 규칙을 따른다 | `CURRENT`; 반환 shape parity 대기 | `operations.go`, `spot_operations.rs` |
| Handler 등록 | `OnSendReady`, Stream `OnPacket`, Monitor `OnEvent` | `on_send_ready`, Stream `on_packet`, Monitor `on_event` | 등록 성공은 값 없이 반환한다 | Handler | callback 수명은 handle이 관리하고 close 시 callback을 해제한다 | `CURRENT`; callback lifetime parity 대기 | `socket_direct.go`, `socket_types.go`, `bindings/rust/src/contracts/eventing/monitor.rs` |
| Socket monitor | `OpenSocketMonitor`; `Recv`, `Status`, `OnEvent`, `Close` | `SocketMonitor::open`; `recv_with_flags`, `status`, `on_event`, `close` | monitor no-data는 Go `RecvNoData` error와 Rust `Ok(None)`으로 표현한다 | Config·Recv·Handler·Close | monitor는 data plane과 분리되며 호출자가 수명을 관리하는 resource다 | `CURRENT`; no-data/error mapping 대기 | `monitor.go`, `bindings/rust/src/contracts/eventing/monitor.rs` |
| Poller·timer | `NewPoller`, `Poller.Wait`; `NewTimer`, `Timer.Recv` | `Poller::new`, `wait`; `Timer::new`, `recv` | timer no-data는 Go `(0, false, nil)`, Rust `Ok(None)`이고 poll timeout은 ready count `0`이다 | Config·Recv·Handler·Close | poller와 timer가 등록 source를 관리하고 close 시 native registration을 해제한다 | `CURRENT`; parity gate 대기 | `poller_timer.go`, `bindings/rust/src/contracts/eventing/poller.rs` |
| Option set·get | `Context.Options`, `CommonOptions`와 typed socket options | `Context::options`, `common_options`와 typed socket options | getter는 `(value, nil)` / `Ok(value)`, setter는 값 없이 성공한다 | Config | option facade는 resource를 빌려 쓰며 resource의 소유권을 바꾸지 않는다 | `CURRENT`; type/error parity 대기 | `context.go`, `socket_options.go`, `bindings/rust/src/contracts/sockets/socket_options.rs` |

## 5. Contract test 규칙

각 행은 Go와 Rust에서 같은 public-observable 조건을 만든다. Result enum을 private hook으로 직접 주입하지
않는다.

- 성공, no-data와 함수군별 대표 실패를 public API로 재현한다.
- Go는 반환된 `error`를 `errors.As`로 함수군별 type과 `ZlinkError`에 대조한다.
- Go는 `context.Context` cancellation과 deadline을 `errors.Is`로 대조하고 callback·goroutine이 종료되는지
  확인한다. Context error와 Core 함수군별 error는 서로 다른 실패 원인으로 검증한다.
- Rust는 compile-time assertion으로 단일 함수군 메서드의 구체 error type을 확인한다.
- Rust는 background callback의 `FnOnce + Send + 'static` bound와 각 operation의 현재 반환 type을
  compile-time assertion으로 확인한다.
- 두 언어에서 `code`와 `internal_errno`가 같은 Core 의미를 나타내는지 비교한다.
- 성공과 실패 뒤 message와 handle을 다시 사용하거나 close하여 ownership 변화를 확인한다.
- Submit 실패와 request completion 실패를 별도 scenario로 검증한다.
- Non-blocking send, publish와 request submit의 backpressure가 현재 Go에서는 `false, nil`, Rust에서는
  `Ok(false)`로 관찰되며, 실제 Core failure만 함수군별 error로 전달되는지 검증한다. 반환 shape를 바꾸는
  결정은 이 검증과 별도 contract approval 뒤에 반영한다.

실행 결과는 `bindings/doc/plan/log/common/`의 parity log에 기록하고 이 inventory의 Evidence 열에서 연결한다.

## 6. 구현 후 Codex parity review

Go와 Rust의 대응 구현을 모두 채운 뒤 두 언어의 개별 계획에서 POSD·DDD 리팩터링과 성능 비용·dead code
review가 각각 `CLEAN`인지 먼저 확인한다. 한 언어라도 `NOT CLEAN`이거나 review를 실행하지 않았으면 parity
review를 시작하지 않는다.

구현자가 아닌 frontier Codex coding/review agent가 두 binding source manifest, 전체 diff, public API snapshot,
production call path와 contract test를 read-only로 비교한다. Contract와 architecture를 판단할 수 있는 model을
`high` 이상의 reasoning level로 사용한다. 이 review는 이름이나 반환 type만 비교하지 않고 다음 내용을
확인한다.

- 같은 command, state transition, lifecycle과 ownership invariant를 두 언어가 각 언어 관례에 맞게 같은
  의미로 표현하는가.
- Error mapping과 no-data가 adapter나 helper 여러 곳에 흩어지지 않고 각각 한 책임 경계에 있는가.
- Signature 통일을 위해 얕은 wrapper, pass-through helper와 중복 conversion을 추가하지 않았는가.
- 반환값과 error를 변환하는 hot path에 불필요한 allocation, copy, lock·channel·atomic contention이 없는가.
- 이전 boolean submit, service API와 호환 alias를 위한 dead branch, export, helper와 test fixture가 남지 않았는가.

Finding은 `Critical`, `High`, `Medium`, `Low`와 `contract`, `POSD`, `DDD`, `performance-cost`, `dead-code`,
`test/evidence` category로 기록한다. 미해결 `Critical`, `High`, `Medium` finding이 0건이고 `Low` finding이 모두
처리됐으며 같은 두 manifest에서 contract test가 다시 통과해야 parity decision을 `CLEAN`으로 기록한다.
`NOT CLEAN`이면 해당 언어 계획으로 돌아가 수정하고 새 manifest와 fresh test 결과로 개별 review부터 다시
수행한다. Parity가 `CLEAN`이 되기 전에는 공통 계획의 PGR-COMMON-03을 완료하지 않는다.

## 7. 동기화와 완료 조건

다음 대상이 같은 의미를 유지해야 한다.

- `bindings/doc/spec/README.ko.md`와 영문 대응 문서
- `bindings/doc/spec/go/README.ko.md`와 영문 대응 문서
- `bindings/doc/spec/rust/README.ko.md`와 영문 대응 문서
- Go와 Rust public API snapshot
- Go와 Rust contract test scenario

모든 필수 행이 `PASS`이고 두 언어의 개별 품질 gate와 Codex parity review가 모두 `CLEAN`이며 미해결
`Critical`, `High`, `Medium` finding이 없을 때 parity gate가 완료된다. Go 또는 Rust 한쪽만 통과하면 완료가
아니다.
