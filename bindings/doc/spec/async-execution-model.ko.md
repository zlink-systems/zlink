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

C에서 `ZLINK_POLLCOMPLETION`은 다음 `zlink_completion_recv()`가 한 건을 반환할 수 있다는
non-consuming level readiness다. `zlink_poller_wait()`는 completion을 소비하지 않는다.
Caller는 준비된 socket마다 `zlink_completion_recv(..., ZLINK_RECV_FLAGS_DONTWAIT)`를
`ZLINK_RECV_NO_DATA`까지 반복한다.

Raw completion을 공개하지 않는 고수준 binding은 socket마다 drain owner를 하나만 둔다.
Socket을 public poller에 `PollCompletion`으로 등록하지 않았으면 binding runtime이 owner다.
등록하면 `wait()` 호출 thread로 owner를 원자적으로 이전하고, 제거하거나 completion bit를
빼면 runtime owner로 되돌린다. 두 owner가 같은 queue를 동시에 비우지 않는다.

고수준 `PollCompletion`은 native queue에서 한 건 이상을 꺼내 live waiter를 끝내거나 detached
state를 정리한 뒤 반환하는 completion progress event다. 반환할 때 queue가 이미 비어 있을 수
있고 public awaitable의 새 상태 변화를 보장하지 않는다. `POLLIN`이 함께 준비돼도 application
DATA는 소비하지 않는다.

Public poller가 owner인 동안 completion-backed terminal은 그 poller의 `wait()` drain에
의존한다. Blocking request와 Go `Submit(context.Context)`를 함께 사용하려면 다른
thread·goroutine이 `wait()` loop를 계속 실행해야 한다. 같은 실행 thread에서 `wait()`와
blocking terminal을 직렬로 호출하지 않는다. Binding은 blocking terminal을 위해 owner를
가져오거나 별도 drain thread를 만들지 않는다.

## 5. Provisional registry와 완료 합류

Completion-backed terminal은 native `FINAL` 호출 전에 language operation state를 stable
`user_context`로 찾을 수 있도록 socket-local registry에 provisional 상태로 등록한다.
Completion ID는 native 호출이 반환하기 전에는 알 수 없으므로 ID를 key로 먼저 등록하지 않는다.

Submit과 completion은 다음 순서로 합류한다.

1. Native submit이 실패하면 ID `0`을 확인하고 provisional state를 제거한 뒤 submit error로
   terminal을 끝낸다. 이 경로에는 completion이 생기지 않는다.
2. Successful send가 ID `0`을 반환하면 inline success로 terminal을 끝내고 state를 제거한다.
3. Successful request 또는 nonzero send ID는 submit outcome, ID와 Core ownership을 provisional
   state에 원자적으로 publish한다.
4. Completion이 submit 반환보다 먼저 drain되면 `user_context`로 state를 찾아 result와 native
   aggregate ownership을 capture한다.
5. Submit publish와 completion capture가 모두 끝났을 때만 public terminal을 정확히 한 번
   끝내고 registry entry를 한 번 제거한다.

Pre-return completion을 처리한 `wait()`도 publish와 capture가 합류해 settle 또는 cleanup이
끝나기 전에는 `PollCompletion` progress를 반환하지 않는다. Blocking send와 reply는 completion을
만들지 않으므로 provisional registry를 사용하지 않는다.

## 6. Caller wait cancellation

고수준 cancellation은 Core operation 취소가 아니라 caller가 language terminal을 기다리는
행위의 취소다.

- Native 호출 전에 cancellation이 확정되면 Core를 호출하거나 provisional state를 남기지 않는다.
- Native 호출 중 cancellation과 submit failure가 경합하면 native 반환까지 cancellation claim만
  기록한다. Submit이 실패하면 정확한 submit error가 우선한다.
- Successful submit 뒤 cancellation이나 Future drop이 먼저 확정되면 live waiter만 한 번
  canceled 또는 detached 상태로 끝낸다. Registry state는 late completion이나 socket/context
  lifecycle cleanup까지 유지한다.
- Late completion은 native payload와 state를 정리하지만 public waiter를 다시 끝내지 않는다.
  Cancellation이 먼저 확정된 successful ID `0` send도 success로 다시 끝내지 않는다.
- Socket close나 context termination으로 completion을 받을 수 없으면 live waiter만
  shutdown 또는 terminated error로 끝내고 모든 registry state를 제거한다.

Request completion이 non-OK이면 고수준 binding은 typed request error만 공개한다. Error-reply
payload는 language message collection이나 error property로 옮기지 않고 user-visible error를
끝내기 전에 native completion을 정확히 한 번 닫는다. Go에서는 typed request error가
`(nil, error)`이고, Context cancellation이 먼저 확정되면 `(nil, ctx.Err())`다.

## 7. 구현 및 contract test 검증 요구

Public terminal, public poller event와 언어별 cancellation 결과만으로 다음을 확인한다. 각
항목은 contract test 하나로 이어진다.

**완료와 drain**

- C poller가 `ZLINK_POLLCOMPLETION`을 반환한 뒤 raw drain을 시작하면 queue의 record를 한 번씩
  받을 수 있고, poller wait 자체는 record를 소비하지 않는다.
- 고수준 poller가 `PollCompletion`을 반환하면 native completion을 한 건 이상 완전 처리했으며,
  그 event 뒤 raw receive 성공이나 public waiter의 새 상태 변화는 보장되지 않는다.
- Public poller owner에서 다른 thread·goroutine이 `wait()`를 실행하면 blocking request와 Go
  `Submit(context.Context)`가 진행하고, `wait()`가 없으면 binding이 queue를 임의로 가져오지 않는다.

**Submit과 completion 경합**

- Native submit 실패는 exact submit error로 한 번 끝나며 completion progress를 만들지 않는다.
- Successful ID `0` send는 한 번 성공하고, nonzero completion이 submit 반환 전에 drain돼도
  public terminal은 submit publish와 합류한 뒤 한 번만 끝난다.
- Successful request는 nonzero completion과 합류하며 result conversion 또는 cleanup 뒤 registry
  entry를 한 번 제거한다.

**Cancellation과 lifecycle**

- 호출 전 cancellation은 native operation을 시작하지 않고 caller terminal만 취소한다.
- Successful submit 뒤 wait cancellation이나 Future drop은 waiter를 한 번 끝내며 late completion은
  waiter를 다시 끝내지 않고 payload를 정리한다.
- Socket close와 context termination은 live waiter를 해당 lifecycle error로 끝내고 detached
  state와 native payload를 남기지 않는다.
