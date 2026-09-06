# 비동기 실행 모델과 완료 표면 용어

> 이 문서는 binding 스펙 전반에서 사용하는 동기·비동기, blocking·non-blocking,
> 실행 환경, awaitable과 완료 진행의 뜻을 정의한다. 언어별 public signature는 각 binding
> 스펙과 [비동기 완료 표면 정책](async-coroutine-policy.ko.md)이 소유한다.

## 1. 완료와 실행 환경의 축

API가 결과를 돌려주는 시점과 그 API를 실행하는 환경은 서로 다른 판단이다.

| 축 | 질문 | 값 |
|---|---|---|
| 완료 시점 | 호출이 최종 결과를 언제 돌려주는가 | 동기 또는 비동기 |
| 대기 방식 | 진행할 수 없을 때 호출 thread가 멈추는가 | blocking 또는 non-blocking |
| 실행 환경 | 대기와 continuation을 무엇이 처리하는가 | OS thread, 가상 thread·goroutine, coroutine, event loop |

동기 terminal은 호출이 반환될 때 최종 결과가 정해진다. 비동기 terminal은 awaitable을 먼저
반환하고 결과를 나중에 정한다. Blocking 호출은 진행할 수 있을 때까지 호출 thread를
기다리게 하고, non-blocking 호출은 기다리지 않는다.

고수준 send·request의 awaitable terminal은 native `DONTWAIT` 제출과 completion drain을
사용한다. Blocking send·request terminal은 native `NONE`을 사용한다. Go의
`Submit(context.Context)`는 public terminal 하나만 제공하지만, 내부에서는 native
`DONTWAIT` 제출 뒤 completion을 기다린다. Publish의 `DONTWAIT` 선택은 lossy/NODROP 계약의
일부이므로 send·request의 완료 모델과 구분한다.

## 2. 실행 환경

실행 환경은 public terminal의 완료 의미를 바꾸지 않는다. 같은 awaitable도 언어에 따라
다른 실행 환경에서 소비한다.

| 실행 환경 | 대기를 처리하는 방법 | 대표 binding |
|---|---|---|
| OS thread | kernel이 thread를 기다리게 한다 | C, C++ blocking terminal, Java platform thread |
| 가상 thread·goroutine | 언어 runtime이 가벼운 실행 단위를 기다리게 한다 | Java virtual thread, Go |
| coroutine | 함수가 suspend되고 continuation으로 재개된다 | C++, Kotlin, Python, Rust |
| event loop | Promise나 coroutine continuation을 queue에서 실행한다 | Node, Python asyncio |

가상 thread나 goroutine에서 blocking API를 호출해도 그 API 자체는 동기 호출이다. 반대로
awaitable을 OS thread에서 기다려도 public terminal은 비동기다. `coroutine`은 실제 coroutine
기능을 가리킬 때만 쓰고, 모든 비동기 실행 방식을 대표하는 말로 쓰지 않는다.

## 3. Awaitable과 동기 결과

Operation을 끝내고 결과 또는 완료 handle을 반환하는 method를 terminal이라고 부른다.
Awaitable은 비동기 완료를 담는 언어별 값을 함께 가리키는 문서 용어다.

| Binding | Awaitable 또는 완료 대기 형태 |
|---|---|
| C++ | `async_result_t<T>` |
| .NET | `Task` |
| Java/Kotlin | `CompletionStage<T>`; Kotlin은 Java 계약을 사용한다 |
| Node | `Promise<T>` |
| Python | `Awaitable[T]` |
| Rust | `Future<Output = T>` |
| Go | `Submit(context.Context)` 호출을 실행하는 goroutine이 internal completion을 기다린다 |

동기 terminal의 반환값은 awaitable이 아니다. `void`, collection, `Result`, `error` 또는
언어별 exception으로 호출 안에서 결정한 결과를 전달한다.

## 4. Poller와 completion drain

C의 raw readiness, `zlink_completion_recv()`와 record 수명은
[Core completion pull과 ownership](../../../core/doc/spec/core/socket/README.ko.md#completion-pull과-ownership)이
소유한다.

- **고수준 바인딩은 socket마다 completion을 읽어 언어 terminal로 전달하는 주체(completion owner)를 하나만 둔다.**
  같은 queue의 record를 두 주체가 소비하지 않도록 하기 위해서다. Public poller에
  `PollCompletion`으로 등록하지 않았으면 runtime이 owner이고, 등록하면 `wait()` 호출
  thread로 원자적으로 이전한다. 제거하거나 completion bit를 빼면 runtime owner로 되돌린다.

고수준 `PollCompletion`은 native queue에서 한 건 이상을 꺼내 live waiter를 끝내거나 detached
state를 정리한 뒤 반환하는 completion progress event다. 반환할 때 queue가 이미 비어 있을 수
있고 public awaitable의 새 상태 변화를 보장하지 않는다. `POLLIN`이 함께 준비돼도 application
DATA는 소비하지 않는다.

Public poller가 owner인 동안 completion-backed terminal은 그 poller의 `wait()` drain에
의존한다. Blocking request와 Go `Submit(context.Context)`를 함께 사용하려면 다른
thread·goroutine이 `wait()` loop를 계속 실행해야 한다. 같은 실행 thread에서 `wait()`와
blocking terminal을 직렬로 호출하지 않는다. Binding은 blocking terminal을 위해 owner를
가져오거나 별도 drain thread를 만들지 않는다.

## 5. Submit 결과와 completion의 합류

- **바인딩은 Core에 전달한 context의 유효 수명을 보장하고 submit 결과와 completion이 경합해도 언어 terminal을 정확히 한 번 끝내며 남은 native payload를 정확히 한 번 정리한다.**
  Completion은 submit 반환 전에 읽힐 수 있으므로, 반환 순서가 결과 유실·중복 완료·중복
  해제로 이어져서는 안 된다. Context의 native 수명은
  [Core part send](../../../core/doc/spec/core/socket/README.ko.md#part-send와-pending-admission)와
  [request 계약](../../../core/doc/spec/core/socket/README.ko.md#request와-reply)이 소유한다.
  내부 확인 조건은 각 native payload에 해제 또는 언어 소유권 이전이 한 번만 대응하는 것이다.

Submit 반환값과 대기 토큰을 언어 결과로 연결하는 기준은
[공통 결과 투영](README.ko.md#submit-result-projection)을 따른다. Completion이 먼저 도착해도
해당 submit 결과와 합류하기 전에는 terminal을 끝내지 않는다. 조기 completion을 처리한
`wait()`도 합류 후 settle 또는 cleanup이 끝나기 전에는 `PollCompletion` progress를 반환하지
않는다. Blocking send와 reply에는 completion을 기다리는 합류가 없다.

**언어별 재량** — 등록 시점과 자료구조는 구현이 정한다. Submit 반환 전 등록, drain과 등록의
직렬화, 조기 record 보관은 모두 위 완료·수명 결과를 보존할 때만 동등한 방법이다.
동등성은 [검증 요구](#7-구현-및-contract-test-검증-요구)의 terminal 결과·오류·완료 횟수와
cancellation 이후 관측으로 확인한다.

## 6. Caller wait cancellation

고수준 cancellation은 Core operation 취소가 아니라 caller가 language terminal을 기다리는
행위의 취소다.

- Native 호출 전에 cancellation이 확정되면 Core를 호출하거나 operation state를 남기지 않는다.
- Native 호출 중 cancellation과 submit failure가 경합하면 native 반환까지 cancellation claim만
  기록한다. Submit이 실패하면 정확한 submit error가 우선한다.
- Successful submit 뒤 cancellation이나 Future drop이 먼저 확정되면 live waiter만 한 번
  canceled 또는 detached 상태로 끝낸다. Operation state는 late completion이나 socket/context
  lifecycle cleanup까지 유지한다.
- Late completion은 native payload와 state를 정리하지만 public waiter를 다시 끝내지 않는다.
  Cancellation이 먼저 확정된 successful ID `0` send도 success로 다시 끝내지 않는다.
- Socket close나 context termination으로 completion을 받을 수 없으면 live waiter만
  shutdown 또는 terminated error로 끝내고 모든 operation state를 제거한다.

- **성공한 REQUEST reply만 언어 message collection으로 소유권을 옮기고, 변환 실패 시 이미 만든 wrapper와 남은 native part를 함께 정리한다.**
  부분 변환으로 payload의 소유자가 사라지거나 둘이 되지 않도록 하기 위해서다.
  Native array의 정리 방법은 [Core completion ownership](../../../core/doc/spec/core/socket/README.ko.md#completion-pull과-ownership)을 따른다.

Request completion이 non-OK이면 고수준 binding은 typed request error만 공개한다. Error-reply
payload는 language message collection이나 error property로 옮기지 않고 user-visible error를
끝내기 전에 native completion을 정확히 한 번 닫는다. Go에서는 typed request error가
`(nil, error)`이고, Context cancellation이 먼저 확정되면 `(nil, ctx.Err())`다.

## 7. 구현 및 contract test 검증 요구

Public terminal, public poller event와 언어별 cancellation 결과만으로 다음을 확인한다. 각
항목은 contract test 하나로 이어진다.

**완료와 drain**

C의 raw completion 관측은
[Core 검증 요구](../../../core/doc/spec/core/socket/README.ko.md#8-구현-및-contract-test-검증-요구)를 따른다.

- 고수준 `PollCompletion`을 관측해도 public awaitable의 새 상태 변화가 필요하지 않으며,
  함께 준비된 DATA는 뒤의 application receive에서 읽을 수 있다.
- Public poller에 completion을 등록한 socket에서 다른 thread·goroutine이 `wait()`를
  실행하면 blocking request와 Go `Submit(context.Context)`가 완료를 받을 수 있다.

**Submit과 completion 경합**

- 대기 토큰 없이 실패하는 native 제출과 같은 입력을 고수준 terminal로 제출하면 해당
  submit error를 한 번 받는다.
- 즉시 admission된 send의 terminal은 한 번 성공하며 완료 record를 기다리느라 멈추지 않는다.
- 응답자가 즉시 reply하는 여러 request를 제출하면 각 terminal은 해당 reply의 part 순서와
  byte를 한 번만 반환한다.

**Cancellation과 lifecycle**

- 호출 전에 취소한 send·request는 caller terminal에 취소를 반환하고 peer에 해당 message를 보내지 않는다.
- Successful submit 뒤 caller wait 취소가 먼저 확정되면 뒤의 reply가 caller의 취소 결과를
  성공 결과로 바꾸거나 terminal을 다시 완료하지 않는다.
- Socket close와 context termination은 live waiter를 해당 lifecycle error로 끝낸다.
- Non-OK request 결과는 typed request error로 관측하며 error reply의 payload를 성공값이나
  error property로 받지 않는다. Go의 결과는 `(nil, error)`다.
