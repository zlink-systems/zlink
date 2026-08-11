---
title: "바인딩 Message 객체와 ownership 공통 계약 — 구현 전 초안"
---

# 바인딩 Message 객체와 ownership 공통 계약

> 이 문서는 **구현 전 초안**이며 현재 공개 계약이 아니다. 모든 binding의 구현과
> contract test가 이 문서에 맞게 변경되고 언어별 exact interface가 확정된 뒤 정식
> 공통 spec으로 승격한다.

이 문서는 application이 `Message`를 만들고 송수신할 때 모든 binding에서 같은
ownership과 수명 규칙을 관찰하도록 목표 계약을 정의한다. 메서드 이름과 오류를
표현하는 문법은 언어 관례를 따를 수 있지만, native storage의 소유자와 send 이후
`Message`의 상태는 달라지면 안 된다.

이 계약의 기준은 Core의
[Message 계약](../../../../core/doc/spec/core/02-message.ko.md)과
[socket 계약](../../../../core/doc/spec/core/socket/README.ko.md)이다. 정확한 Core C
signature는 `core/include/zlink/message/api.h`와 `core/include/zlink/socket/api.h`가
정의한다.

## 범위

이 문서는 다음 질문에 답한다.

- `Message`의 payload는 누가 소유하는가?
- bytes와 문자열로 만든 `Message`는 어느 시점에 복사되는가?
- send, publish, request와 reply를 시도하면 입력 `Message`를 다시 사용할 수 있는가?
- backpressure나 validation 실패가 발생하면 ownership은 어디에 남는가?
- 수신한 `Message`는 누가 닫아야 하는가?
- 재시도할 payload는 언제 복사해야 하는가?

`Received`, `TopicMessage`와 request completion은 message part의 묶음과 metadata를
표현한다. 이 문서는 그 객체에 포함된 각 `Message` part의 ownership만 정의한다.
Socket별 routing, timeout과 callback 완료 의미는 Core socket 계약과 언어별 socket
spec이 소유한다.

## Application에서 보이는 기본 규칙

`Message`는 Core message part 하나를 소유하는 객체다. Application이 새
`Message`를 만들거나 수신에 성공하면 그 객체가 native frame을 소유한다. send가
그 frame을 소비하면 같은 객체는 다시 읽거나 보낼 수 없다.

모든 binding은 다음 규칙을 같은 의미로 제공해야 한다.

1. `allocate(size)`는 Core가 관리하는 writable payload storage를 할당한다.
2. `from(bytes)`와 `from(string)`은 입력을 새 Core-owned frame으로 한 번 복사한다.
3. 수신 성공은 Core가 넘긴 frame을 payload copy 없이 `Message`가 소유하게 한다.
4. `Message`를 보내는 경로는 이미 소유한 native frame을 Core에 넘긴다. send를 위해
   같은 payload를 새 native frame에 다시 복사하지 않는다.
5. Builder가 `Message`를 받아들이면 send operation이 frame을 독점적으로 보관한다.
   이후 원본 객체의 수명은 operation이 관리하며, Application은 원본 객체를 읽거나
   다른 operation에 넘길 수 없다.
6. Core가 frame을 소비한 뒤에는 `Message`가 consumed 상태가 된다. Application은
   그 객체의 payload를 읽거나 같은 객체로 다시 send할 수 없다.
7. 같은 payload가 다시 필요하면 send 전에 `copy()`로 별도 `Message`를 만든다.
8. 소유한 `Message`를 보내지 않으면 Application이 명시적으로 닫는다. 언어의
   `Dispose`, `Drop`, context manager와 같은 수명 문법은 이 close 책임을 표현한다.

## Message 상태

Application이 관찰하는 상태는 다음과 같다.

| 상태 | 소유자와 사용 가능 범위 | 허용되는 다음 동작 |
|---|---|---|
| owned | `Message`가 유효한 native frame을 소유한다. Payload view는 이 상태에서만 유효하다. | 읽기, 쓰기, 명시적 copy, send 제출, close |
| held | Send operation이 frame을 독점적으로 보관한다. Core에는 아직 ownership이 이전되지 않았을 수 있지만 Application은 원본 객체를 사용할 수 없다. | operation의 submit, retry 또는 cleanup |
| consumed | Core 또는 send operation이 frame을 소비했다. `Message`는 payload를 소유하지 않는다. | idempotent cleanup만 허용 |
| closed | Application 또는 runtime이 frame을 해제했다. | idempotent cleanup만 허용 |

Consumed 또는 closed 객체에서 payload 크기, data view, property와 refcount를 요청하면
binding은 해당 언어의 invalid-object 오류를 보고해야 한다. 빈 payload로 보이게 하거나
새 message처럼 자동 초기화하면 ownership 오류가 숨겨지므로 허용하지 않는다.

## 생성과 복사

### 빈 Message와 크기 지정 할당

빈 생성자는 `zlink_msg_init()`과 같은 의미로 zero-length native frame을 만든다.
`allocate(size)`는 `zlink_msg_init_size()`와 같은 의미로 `size` bytes의 native payload
storage를 만들며, Application이 그 storage를 채울 writable view를 제공한다.

VM이나 JavaScript runtime을 사용하는 binding도 `allocate(size)`의 authoritative
storage를 managed array나 일반 runtime buffer로 대체하면 안 된다. 언어 runtime이
제공하는 view는 native payload storage를 가리켜야 한다.

### bytes와 문자열 입력

`from(bytes)`는 caller가 제공한 bytes를 새 native frame에 복사한다. 호출이 반환된
뒤 caller는 원본 buffer를 변경하거나 해제할 수 있다. 문자열 입력은 언어별 UTF-8
변환을 마친 뒤 같은 규칙을 적용한다.

이 복사는 입력 storage와 `Message` ownership을 분리하기 위해 필요하다. 이후 send는
완성된 native frame을 그대로 넘기므로 payload를 다시 복사하지 않는다.

### 명시적 copy

`copy()`는 source와 별도로 소비하거나 닫을 수 있는 owned `Message`를 반환한다. Core가
큰 payload의 storage를 reference count로 공유할 수 있으므로 물리적인 payload 복사는
필수 조건이 아니다. Application에서 관찰하는 조건은 source와 copy의 ownership이
서로 독립적이라는 점이다.

일반 socket에서 재시도하거나 여러 socket에 같은 payload를 보내야 하는 Application은
첫 submit 전에 필요한 수만큼 copy를 만든다. Binding이 일반 socket의 send 실패 뒤
원본을 암묵적으로 복원하거나 submit할 때마다 내부 copy를 만드는 방식은 이 계약을
만족하지 않는다. STREAM backpressure 재시도는 아래 예외 규칙을 따른다.

## Send operation의 ownership 이전

이 절의 send는 일반 send, routed send, publish, request와 reply처럼 Core에 message
part를 제출하는 모든 operation을 포함한다.

Builder가 `Message`를 받아들이면 send operation이 frame의 독점 보관 책임을 얻고
`Message`는 held 상태가 된다. Core로 ownership이 실제 이전되는 시점은 각 part의
native submit 호출이다. Reference 객체를 사용하는 언어에서도 held 상태인 원본을
읽거나 변경하거나 다른 operation에 제출하면 안 된다. 값을 이동하는 언어에서는
builder가 값을 받는 시점에 type system으로 같은 제한을 표현할 수 있다.

다음 코드는 공통 동작을 설명하는 contract pseudocode이며 실제 언어별 signature가
아니다.

```text
message = Message.allocate(size)
fill(message.mutableData())

operation = socket.send().message(message)
// operation이 message를 보관하므로 이 시점부터 message를 직접 사용하지 않는다.
result = operation.submit()
```

Binding은 native frame을 Core `*_part` 함수에 직접 전달한다. `Message`에서 bytes를
꺼내 새 frame을 만들거나, public builder의 재사용 계약을 유지하기 위해 내부 copy를
추가하면 안 된다.

### Builder validation과 submit 시작

Builder가 `Message`를 받아들이기 전에 null, closed 객체 또는 argument 범위 오류를
발견하면 입력은 owned 상태를 유지한다. Builder가 입력을 받아 held 상태가 된 뒤에는
Application에 ownership을 암묵적으로 돌려주지 않는다.

`submit()`은 held part 전체에 대한 preflight validation을 마친 뒤 첫 Core 함수를
호출해야 한다. Preflight validation이 실패하면 operation이 held part를 모두 닫고
원본 객체를 재사용 불가 상태로 만든다. 첫 Core 호출이 시작된 뒤에는 이미 전달한 part는
Core 결과에 따라 처리하고, 아직 전달하지 않은 part는 operation이 정리한다. 이 규칙은
언어마다 builder가 입력을 돌려주는 별도 public contract를 만들지 않게 한다.

### 일반 socket의 submit 결과

PAIR, PUB, DEALER, ROUTER와 해당 request/reply helper는 Core 호출의 성공과 실패 모두에서
제출한 part를 소비한다. `DONTWAIT` submit이 backpressure를 반환한 경우도 같다.

| 결과 | Message 상태 | Application의 다음 동작 |
|---|---|---|
| submit 성공 | consumed | 새 `Message`를 만들어 다음 payload를 보낸다. |
| backpressure를 포함한 submit 실패 | consumed | send 전에 보관한 copy로 전체 record를 다시 제출한다. |
| Builder가 입력을 받기 전 validation 실패 | owned | 입력을 수정한 뒤 같은 객체를 다시 사용할 수 있다. |
| Held 상태에서 submit preflight 실패 | closed | operation이 입력을 닫으므로 새 `Message`로 다시 시작한다. |

Binding이 exception, result enum 또는 `Result` 타입 중 무엇을 사용하더라도 이 상태
전이는 같아야 한다.

### STREAM backpressure 예외

STREAM send는 Core 계약의 예외를 그대로 노출한다. 성공과 backpressure가 아닌 실패는
part를 소비한다. `DONTWAIT` 호출이 `BACKPRESSURED`와 `EAGAIN`을 반환하면 Core가 part를
소비하지 않으므로 operation은 해당 `Message`를 held 상태로 유지한다. 같은 operation을
다시 submit할 수 있어야 하며, operation을 폐기하면 binding이 retained frame을 닫는다.

Binding은 이 예외를 모든 socket의 일반 규칙으로 확대하면 안 된다. 반대로 STREAM
backpressure에서 operation을 종료하거나 `Message`를 consumed 상태로 바꾸어 Core가
보장한 재시도를 막아도 안 된다.

### Multipart submit

Multipart operation은 첫 part부터 마지막 part까지 하나의 record로 제출한다. Core가
중간 또는 마지막 part에서 실패하면 이미 제출한 part와 열린 record를 소비하고
폐기한다. Binding은 submit 전에 전체 part의 독점 보관 책임을 얻는다. 첫 Core 호출이
시작된 뒤 실패하면 아직 Core에 전달하지 않은 나머지 part도 operation이 닫아서,
submit 반환 뒤 builder가 held `Message`를 남겨 두지 않게 해야 한다.

따라서 submit을 시작한 multipart payload는 결과와 관계없이 Application이 다시 사용할
수 없다. 재시도하려면 submit 전에 보관한 전체 record의 copy를 사용하여 첫 part부터
새 operation을 시작한다.

## 수신과 전달

수신 성공은 Core에서 Application 쪽으로 ownership을 옮긴다. Binding은 수신한
`zlink_msg_t`를 새 `Message`가 직접 소유하게 하고, payload를 managed buffer로 복사한 뒤
native frame을 닫는 방식으로 materialize하면 안 된다.

Application은 direct receive 결과와 callback으로 받은 `Message`를 같은 규칙으로
다룬다. 다른 send operation에 전달하면 해당 frame이 소비되고, 전달하지 않으면 수신
결과의 lifecycle 안에서 명시적으로 닫는다.

수신할 데이터가 없거나 수신 전에 오류가 발생하면 새 ownership이 생기지 않는다.
Caller가 재사용 가능한 output storage를 넘기는 언어에서는 실패 시 기존 output의
ownership과 payload를 변경하지 않는다.

## Close와 자동 정리

Owned `Message`의 close는 native frame의 ownership을 해제한다. Close 뒤 payload view는
유효하지 않다. Consumed `Message`에는 해제할 frame이 없으므로 binding의 dispose나
destructor가 같은 frame을 다시 닫으면 안 된다.

Message를 보관하는 builder는 deterministic cleanup을 제공해야 한다. 객체 참조와 자동
메모리 관리를 사용하는 언어는 `close()`, `Dispose()` 또는 같은 의미의 명시적 종료
동작을 제공한다. RAII나 값 수명을 사용하는 언어는 destructor 또는 `Drop`으로 종료를
보장한다. 언어별 exact interface는 그 언어의 표준 수명 문법에 맞는 이름을 정한다.

Submit하지 않고 builder를 종료하면 operation이 held `Message`를 모두 닫는다. STREAM
backpressure 뒤 재시도를 포기할 때도 같은 종료 동작을 사용한다. Caller가 held 상태의
원본을 따로 닫게 하거나 builder와 caller가 같은 frame을 함께 정리하면 안 된다.

GC와 finalizer는 누수를 막는 마지막 보호 수단으로 사용할 수 있다. 정상적인 lifecycle을
GC 시점에 맡기면 안 되며, 언어별 public API는 deterministic cleanup을 제공해야 한다.
RAII 언어는 destructor나 `Drop`으로 이 책임을 표현할 수 있다.

## 언어별 표현 범위

언어별 이름과 ownership을 표현하는 문법은 달라도 된다. 다음 차이는 같은 의미로
인정한다.

| 언어 특성 | 허용되는 표현 |
|---|---|
| 값 이동을 type system으로 검사하는 언어 | Builder가 `Message` 값을 받아 compile time에 재사용을 막는다. |
| reference 객체를 사용하는 언어 | Builder가 입력을 받아들이면 객체를 held 상태로 바꾸고 이후 직접 접근에서 오류를 보고한다. |
| 명시적 resource lifecycle을 사용하는 언어 | `close()`, `Dispose()` 또는 context manager를 제공한다. |
| RAII를 사용하는 언어 | Destructor나 `Drop`이 owned frame을 해제한다. |
| exception이 없는 언어 | Result 또는 error 값으로 동일한 실패와 상태 전이를 표현한다. |

별도 `moveMessage()`를 선택해야만 Core와 같은 no-copy send를 사용하는 구조는 공통
기본 계약이 아니다. 기본 `Message` send가 ownership 이전 경로여야 한다. Bytes-like
편의 overload는 임시 native `Message`를 만들 수 있지만, caller의 원본 bytes까지
소비한다는 뜻은 아니다.

## 구현 요구사항

각 binding runtime은 다음 조건을 만족해야 한다.

- Public `Message` 하나는 유효한 native `zlink_msg_t` 하나의 ownership을 나타낸다.
- `allocate`와 receive는 native payload storage를 직접 노출한다.
- `from(bytes)` 이후의 send에는 payload copy가 추가로 발생하지 않는다.
- Send는 `Message`의 native frame을 Core에 직접 제출한다.
- Core 결과별 consume 또는 retain 규칙을 public 객체 상태에 즉시 반영한다.
- Received frame을 public `Message`로 만들 때 payload를 복사하지 않고 ownership을 adopt한다.
- Consumed frame을 다시 close하는 double-close와 owned frame을 해제하지 않는 leak을
  각각 contract test로 차단한다.
- Perf 전용 native send loop나 private batch API로 이 규칙을 우회하지 않는다. 일반
  Application과 Framework가 사용하는 public 경로가 같은 ownership 경계를 지난다.

## Contract test 요구사항

모든 binding은 언어별 test 문법으로 다음 결과를 검증해야 한다.

1. `allocate(size)`로 만든 payload view에 쓴 bytes를 send가 그대로 전달한다.
2. 일반 socket의 성공한 submit 뒤 원본 `Message` 접근과 재전송이 실패한다.
3. 일반 socket의 `DONTWAIT` backpressure 뒤에도 원본 `Message`가 consumed 상태다.
4. Builder가 입력을 받아들이기 전 validation 실패 뒤에는 원본 `Message`를 다시 사용할
   수 있다.
5. Held 상태에서 submit preflight가 실패하면 operation이 모든 입력을 닫는다.
6. STREAM의 `DONTWAIT` backpressure 뒤에는 같은 operation을 다시 submit할 수 있다.
7. `copy()`로 만든 source와 copy 중 하나를 보내도 다른 하나는 계속 owned 상태다.
8. Builder가 각 payload를 받아 held 상태로 전환한 뒤 multipart 전송에 실패하면,
   Core에 전달하지 않은 part를 포함한 모든 입력이 재사용 불가 상태가 된다.
9. Submit하지 않은 builder를 명시적으로 종료하면 held part가 모두 닫힌다.
10. STREAM backpressure 뒤 operation을 종료하면 Core가 보존한 part가 닫힌다.
11. Receive 성공으로 얻은 `Message`를 send하면 추가 payload copy 없이 ownership이 이동한다.
12. Receive 실패는 caller가 제공한 output `Message`의 기존 상태를 변경하지 않는다.
13. Consumed 또는 closed 객체의 payload 접근은 invalid-object 오류를 보고한다.
14. Close, destructor, finalizer와 submit이 같은 native frame을 두 번 해제하지 않는다.

Payload copy 여부는 perf 수치만으로 판정하지 않는다. Native allocation/copy 계측을 사용하는
contract test 또는 source-level boundary test로 send와 receive hot path를 검증한다.

## 현재 구현과의 gap

이 절은 목표 계약이 아니라 2026-08-11 기준 구현 상태를 기록한다. 구현이 바뀌면 이 표를
같이 갱신한다.

| Binding | 현재 구현 | 목표 계약과의 gap |
|---|---|---|
| C | `zlink_msg_t`와 Core `*_part` API를 직접 사용한다. | 기준 구현이다. Socket별 ownership 예외를 wrapper binding test의 기준으로 사용해야 한다. |
| C++ | `message_t`가 native frame을 소유한다. Direct rvalue send는 frame을 move하지만 기본 builder helper는 각 part에 `zlink_msg_copy()`로 temporary native view를 만들어 모든 결과에서 원본을 보존한다. 일부 move 경로는 실패한 native part를 원본에 복원한다. | 기본 builder가 borrowed copy를 만들지 않고 원본 frame을 직접 제출해야 한다. 일반 socket 실패에서 원본을 복원하는 경로도 Core의 consume 규칙과 맞춰야 한다. |
| .NET | `Message`가 `ZlinkMsg`를 소유한다. Single-part submit은 native part로 move한 뒤 실패하면 `RestoreFrom`으로 원본을 복원한다. Multipart helper는 전체 입력을 clone하고 clone만 Core에 제출하여 모든 결과에서 원본을 보존한다. | Single-part 일반 socket 실패를 consumed 상태로 반영하고, multipart clone-submit을 원본 ownership 이전으로 바꿔야 한다. STREAM backpressure에서만 operation이 Core가 보존한 frame을 유지해야 한다. |
| Java | `Message`가 FFM `MemorySegment`의 native frame을 소유한다. Send 성공에서만 `markTransferred()`를 호출하며 일부 retry loop가 실패한 같은 frame을 다시 제출한다. | Core가 실패에서 소비한 frame을 valid로 남기거나 재제출하지 않게 해야 한다. STREAM backpressure 예외를 분리해야 한다. |
| Node | `Message.allocate()`와 `Message.from()`이 JS `Buffer`를 소유한다. Send마다 새 `zlink_msg_t`를 할당하고 payload를 복사하며, public `Message`는 immutable reusable value로 남는다. Operation payload는 첫 submit에서 submitted 상태가 되어 결과와 관계없이 다시 submit할 수 없다. | `Message`를 native-owned one-shot 객체로 변경하고, send가 frame을 직접 소비하게 해야 한다. 수신 frame도 native ownership을 유지한 채 동일 객체 모델로 전달해야 한다. STREAM backpressure에서는 같은 operation을 다시 submit할 수 있게 해야 한다. |
| Go | `Message`가 native frame을 소유한다. 기본 `Message(...)`는 submit용 frame을 copy하여 실패 시 원본을 보존하고 성공 시 원본을 닫아 moved 상태로 바꾼다. `MoveMessage(...)`만 처음부터 ownership을 이전한다. | 기본 `Message` send의 추가 copy를 없애고 결과별로 달라지는 원본 상태를 Core 규칙에 맞춰야 한다. 별도 move 선택 없이 기본 경로가 ownership을 이전해야 한다. |
| Rust | `Message`가 inline native frame을 소유하고 builder가 값을 받아 submit에서 Core로 전달한다. 일반 socket의 submit 뒤에는 builder part를 drop한다. STREAM backpressure에서도 결과를 판정하기 전에 part를 drop하여 재시도할 operation을 남기지 않는다. | 일반 socket 구조는 목표에 가깝다. STREAM backpressure에서는 retained part를 held 상태로 유지하고 같은 operation을 다시 submit할 수 있어야 한다. Multipart cleanup 범위도 contract test로 고정해야 한다. |
| Python | `Message`가 native frame을 소유하지만 send materializer가 `_clone_native_msg()`로 part를 복제하여 caller 원본을 보존한다. | 기본 send가 원본 native frame을 직접 소비하게 하고, 재시도나 fan-out은 application의 명시적 `copy()`로 처리하게 해야 한다. |

현재 정식 공통 정책인 [바인딩 API 정책](../README.ko.md)은 일반 `message(...)` builder가
원본을 보존하고 별도 move 경로만 consume하도록 정의한다. 또한 closed 또는 moved-from
객체의 payload 접근 결과를 언어 관례에 맡기고, 한 번 submit한 builder의 재사용을
금지한다. 현재 builder 공통 계약에는 submit하지 않은 operation을 명시적으로 종료하는
동작도 없다.

이 초안은 기본 builder가 입력을 held 상태로 바꾸고 consumed 또는 closed 객체의 접근
오류를 공통으로 강제한다. STREAM backpressure에서는 같은 operation의 재-submit을
허용하며, held part를 정리하는 deterministic builder 종료 동작도 요구한다. 구현을
시작하기 전에 이 네 가지 정책과 언어별 exact interface를 함께 개정해야 한다.
