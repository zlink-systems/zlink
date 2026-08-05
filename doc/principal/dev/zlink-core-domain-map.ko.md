# ZLink Core 도메인 지도

> [POSDDD](./posddd.ko.md)와 [ZLink 시스템 개발 원칙](./zlink-system-design-principles.ko.md)이
> "어떻게 판단하는가"를 다룬다면, 이 문서는 "ZLink core는 실제로 어떤 모듈로 구성되고, 그
> 경계 안에서 무엇이 entity고 무엇이 value object인가"를 다룬다. `zlink-system-design-principles.ko.md`의
> "실제 사례 — zlink_close 설계" 절이 API 하나로 보여준 걸, 이 문서는 core 전체로 넓힌다.
>
> **범위.** 지금은 **core**(context·socket·message·eventing)만 다룬다. framework 계층
> (Spot·Actor·Mesh·Channel·Stream·PubSub·Location Store 등)은 core보다 범위가 훨씬 크고
> 빠르게 변하고 있어서 별도 조사·별도 문서가 필요하다 — 이 문서 끝의 "앞으로 채울 것"에 남겨
> 둔다.
>
> **독자**: ZLink core를 만들거나, core의 공개 계약에 기대어 바인딩·프레임워크를 만드는
> 개발자다. "이 개념이 어느 경계 안에 있고, 식별자로 비교해야 하는지 값으로 비교해야
> 하는지"에 대한 답을 찾는다.
>
> 이 문서의 모든 사실은 `core/doc/spec/`·`core/doc/internals/`·`core/doc/guide/`의 정본
> 문서에서 그대로 가져왔다 — 새 분류 기준(entity/value object/bounded context)만 이 문서가
> 더한다. 정본과 이 문서가 어긋나면 정본이 맞다.

---

## 1. Core의 모듈 구조

`core/doc/internals/posd-module-structure.ko.md`가 이미 다섯 계층으로 정리해 두었다 —
그대로 인용한다.

| 계층 | 책임 |
|---|---|
| Public C API | argument, handle, ownership과 result mapping |
| Socket semantics | socket type별 routing, multipart와 request correlation |
| Runtime core | context, session, pipe, mailbox command와 lifecycle |
| Engine | ZMP·RAW framing과 handshake |
| Transport | TCP, WebSocket, IPC, inproc과 TLS I/O |

같은 문서의 원칙: "Public API가 transport type이나 protocol parser를 직접 분기하지 않는다.
Runtime core는 socket type별 정책을 알지 않으며 engine은 application payload 의미를
해석하지 않는다." 이 다섯 계층은 서로 다른 **bounded context가 아니다** — 같은 개념(예:
"socket")이 계층을 넘어도 의미가 갈리지 않는다. 이건 [POSDDD](./posddd.ko.md)의 "다른
계층, 다른 추상화" 원칙이 적용되는 **하나의 bounded context 내부 계층 구조**다.

진짜 bounded context 경계는 다음 절에서 다루는, **Core와 Framework 사이**에 있다.

## 2. 진짜 경계 — Core ↔ Framework

`core/doc/spec/core/09-runtime-boundary.ko.md`가 이 경계를 이미 정본으로 명시하고 있다.

**Core 공개 ABI에 있는 것** (§1): context/IO-thread lifecycle, message
allocation/ownership/multipart/routing ID, 8개 socket type, bind/connect/disconnect/
endpoint/connection lifecycle, TCP/WS/TLS transport, monitor/poll/poller,
timer/thread/stopwatch/atomic-counter/proxy, request/handshake/reconnect timeout.

**Framework가 소유하고 Core엔 없는 것** (§2): MeshName/ChannelName, MeshNode
lifecycle/peer admission, ready batch/claim/receive batch/reply token, Spot/Actor/Instance
activation, Actor transfer/bound STREAM session/service drain, MeshNode monitor.

이 경계가 진짜 bounded context인 이유는, 같은 원시 관측값을 두 계층이 **서로 다른
모델로** 해석하기 때문이다. `core/doc/internals/runtime-boundary.ko.md`:

> "Core connection identity는 physical lifetime을 구분하는 raw 관측 값이다." — 이 값은
> Mesh lifecycle generation도, descriptor revision도, Actor authority owner generation도,
> Location authority store version도 **아니다**.

Core의 "연결이 살아있다"는 사실 하나를 Framework는 최소 네 가지 다른 개념(mesh
lifecycle generation, descriptor revision, actor authority owner generation, location
authority store version)으로 재해석한다. 이걸 섞으면 — 예를 들어 Core connection identity를
Actor authority generation처럼 다루면 — [POSDDD](./posddd.ko.md)가 말하는 back-door
누출과 같은 사고가 난다: 두 계층이 인터페이스 없이 "이건 같은 값이다"라고 몰래 가정하는
것이다. `spec/core/09-runtime-boundary.ko.md` §4는 이 경계에서의 책임도 나눈다: "Core
errors represent raw socket/transport/protocol/OS failures; Framework converts these
into typed terminal results — Core does not judge accepted service work, handler
completion, Actor transfer, checkpoint, or host termination progress." Framework가 Core
에러를 그대로 위로 전파하지 않고 번역하는 이 지점이 [POSDDD](./posddd.ko.md) "이름 짓기"
절의 anti-corruption layer에 해당한다.

## 3. Entity — Context, Socket, Session

셋 다 식별자(핸들)로 식별되고, 생성부터 소멸까지 상태가 바뀐다.

### Context

- **식별**: `zlink_ctx_new()`가 반환하는 불투명 핸들.
- **소유**: I/O 스레드 풀, reaper, endpoint registry, monitor/timer용 control runtime
  (`core/doc/internals/architecture.ko.md`, "Context와 thread").
- **불변 조건**: 정확히 한 번만 종료한다(`core/doc/spec/core/01-context.ko.md:141-142`,
  "각 context는 정확히 한 번만 종료해야 합니다"). 종료 뒤 핸들 재사용은 문서로만
  금지돼 있고, 런타임이 걸러주는 장치는 없다("이 호출이 반환된 후에는 context 핸들을
  사용하지 마세요", :151-152) — 아래 Socket과 비교하면 **같은 "종료"인데 계약 강도가
  다르다**([zlink-system-design-principles.ko.md](./zlink-system-design-principles.ko.md)의
  zlink_close 사례가 이미 짚은 지점).
- **상태 전이**: 생성 → 활성 → (`zlink_ctx_shutdown` 또는 모든 소켓 종료) → `zlink_ctx_term`
  대기 해제 → 종료. `zlink_ctx_term()`은 그 context에서 만든 모든 소켓이 닫힐 때까지
  블록할 수 있다(`spec/core/01-context.ko.md:139-140`).

### Socket

- **식별**: `zlink_socket(ctx, type)`가 반환하는 불투명 핸들.
- **소유**: option, endpoint, session, pipe (`core/doc/internals/runtime-boundary.ko.md:49`).
- **불변 조건**: "지금 종료할 수 있는가"는 진행 중인 콜백이나 admit된 API 호출 여부에
  달려 있다 — 이 판단의 유일한 통로가 `zlink_close()` 자신이다
  ([zlink-system-design-principles.ko.md](./zlink-system-design-principles.ko.md) 재확인).
  이미 닫힌 소켓을 다시 닫으면 `ZLINK_CLOSE_SHUTDOWN`으로 걸러진다(`zlink_errno.h:173`) —
  Context와 달리 런타임이 지켜주는 계약이다.
- **상태 전이**: 생성 → (bind/connect) → 활성(송수신) → close 요청(신규 송수신·콜백
  등록 차단) → session·pipe 종료 → 핸들 무효화(`internals/runtime-boundary.ko.md:64-67`).
  close 뒤 늦게 도착한 engine 콜백은 종료된 상태를 바꾸지 않는다.
- **생성 시 필수 전제**: socket은 반드시 유효한 context에 속한다 — 독립적으로 존재할
  수 없는 entity다(모든 소켓은 context와 연결돼야 한다, `spec/core/01-context.ko.md:118-119`).

### Session (runtime 내부, 공개 API에는 직접 노출 안 됨)

- **식별**: 하나의 transport connection.
- **역할**: 그 connection의 protocol engine과 reconnect state를 관리한다
  (`internals/runtime-boundary.ko.md:51`). pipe가 socket queue와 engine 사이의 message
  flow를 이어준다.
- DEALER/ROUTER는 논리적 peer 하나가 Application·Completion 두 개의 transport
  connection(pair)을 갖는다 — pair id·pair generation·lane·peer identity를 함께 검증하고,
  한쪽 lane이 죽으면 둘 다 종료한다(`internals/runtime-boundary.ko.md`, "DEALER/ROUTER" 항).
  이건 Session이라는 entity 하나가 아니라 **pair 자체**가 상위 불변 조건의 단위라는 뜻이다.

Session은 공개 계약(`core/include/`)에 핸들로 노출되지 않는다 — Public Contract 경계
안쪽, Runtime Boundary 계층의 entity다. 호출자는 이 entity의 존재를 몰라도 된다
([POSDDD](./posddd.ko.md) "가장 좋은 기능은 호출자가 있는 줄도 모르는 기능이다"의 실제
사례).

## 4. Value object — RoutingId, socket options, result 타입들

### RoutingId — 이름 하나에 개념이 셋

`zlink_routing_id_t`(size + 최대 255바이트 data)는 식별자가 아니라 값 자체다 — 같은
바이트열이면 같은 RoutingId다. 그런데 실제로는 **이름은 하나인데 가리키는 개념이
셋**이다. `core/doc`가 이미 이렇게 나눠 적어 두었다:

1. **로컬 소켓 identity** — `zlink_set_routing_id()`로 connect/bind 전에 설정, 안
   정하면 Core가 RFC 4122 UUID v4 비트 배치의 16바이트를 자동 발급한다("이 기본값은
   UUID 문자열이 아니라 raw 16 bytes다", `spec/core/socket/README.ko.md:729`).
2. **관찰된 peer identity** — `zlink_recv_part` 등이 돌려주는 `source_rid_out_`은
   스레드 로컬 뷰다. 다음 수신 호출 전에 호출자가 복사해야 유지된다. PAIR/DEALER는
   `NULL`을 돌려준다(`spec/core/socket/README.ko.md:605-634`).
3. **STREAM 전용 4바이트 connection id** — accept한 연결마다 부여하는 고정 4바이트
   값으로, 나머지 두 개(1~255바이트 가변 길이)와 크기 자체가 다르다
   (`spec/core/socket/08-stream.ko.md:22`). `zlink_disconnect_rid`는 STREAM에서 정확히
   이 4바이트 형태를 요구한다.

`core/doc/guide/glossary.ko.md:16`의 정의("ROUTER와 STREAM에서 연결된 peer를 식별하는
byte sequence")는 이 중 2번·3번만 가리키고 1번(로컬 identity, DEALER/PAIR에서도 쓰임)은
빠져 있다 — 용어집 범위가 타입의 실제 쓰임보다 좁다. [POSDDD](./posddd.ko.md) 이름 짓기
절의 "목적이 충분히 좁아서, 그 이름을 공유하는 모든 것이 같은 동작을 갖게 한다"는 세
번째 일관성 요건을 놓고 보면, RoutingId라는 한 이름 아래 세 개의 서로 다른 계약(가변
길이/스레드 로컬 뷰/고정 4바이트)이 있다는 뜻이다. 코드를 바꾸자는 게 아니라 — 이건
spec이 이미 명시적으로 구분해 둔 의도된 설계다 — **이 문서를 읽는 사람이 "RoutingId"란
말을 볼 때마다 셋 중 어느 것인지 확인해야 한다**는 뜻이다.

### 그 외 value object

- **socket options / ctx options** — `zlink_set_option`류가 다루는 설정값. 식별자
  없이 값 자체가 의미다.
- **result 타입들** (`zlink_close_result_t`, `zlink_submit_result_t`,
  `zlink_recv_result_t` 등) — `zlink_errno.h:90-92`가 스스로 "겹치지 않는 번호
  범위"라고 명시한 대로, 각 연산군에 스코프된 값 객체다. 오류 처리 원칙("잘게 쪼갠
  예외 타입을 여러 개 노출하지 않는다. 의미 있는 경계에서 하나로 모은다")과 맞대 보면,
  이 8개 result 타입은 "연산 하나당 결과 하나"라는 의미 있는 경계를 이미 지키고 있다 —
  자잘하게 쪼개진 게 아니라 8개의 서로 다른 연산군을 반영한 것이다.
- **zlink_socket_type_t** — 생성 시 한 번 정해지고 이후 불변인 값. `ZLINK_SOCKET_ANY`는
  실제로 생성 가능한 타입이 아니라 필터 API 전용 와일드카드라고 스펙이 명시한다
  (`spec/core/socket/README.ko.md:163-165`) — 이것도 "같은 타입 열거형인데 그중 하나만
  의미가 다르다"는, 작게나마 이름 짓기 규칙이 걸리는 지점이다.

## 5. Message — 어느 쪽에도 깔끔히 안 들어간다

`zlink_msg_t`는 entity도 value object도 아니다. **소유권을 명시적으로 나르는 값**이라는
세 번째 범주다.

- 식별자가 없다 — 두 메시지를 "같은 메시지인가"로 비교하는 API가 없다.
- 그런데 값(바이트 내용)만으로 완결되지도 않는다 — `zlink_msg_move`는 소유권을
  옮기고 원본을 빈 상태로 만들며, `zlink_msg_copy`는 작은 페이로드는 값으로 복사하지만
  큰 페이로드는 refcount로 저장소를 공유한다(`spec/core/02-message.ko.md:189-207`).
  `zlink_msg_adopt`는 move의 변형인데 목적지가 반드시 미초기화 상태여야 한다 — 어기면
  정의되지 않은 동작이다.
- `core/doc/internals/design-decisions.ko.md`가 이 설계 의도를 직접 밝힌다: "명시적인
  move, copy, close operation으로 allocator 선택을 노출하지 않으면서 C API 경계의
  ownership을 표현한다." — 이건 value object의 "값이 같으면 같다"도 아니고 entity의
  "식별자로 추적한다"도 아니다. **"지금 이 순간 누가 이 바이트를 소유하는가"가 유일하게
  중요한 질문**이고, API가 그 질문에 대한 답(소유/공유/미소유)을 타입 시스템이 아니라
  함수 이름(move/copy/adopt)으로 강제한다.

이 셋 중 어디에도 안 들어가는 게 결함은 아니다 — Rust의 `move` 의미론이나 C++의
`unique_ptr`/`shared_ptr`가 같은 문제(자원 소유권 표현)를 같은 방식(값이지만 선형적
소유권을 갖는 것)으로 푼다. 다만 [POSDDD](./posddd.ko.md)의 entity/value object
이분법을 기계적으로 이 타입에 적용하면 안 맞는다는 것 자체가, 이 문서가 실제로 가치
있는 이유다 — 도메인 지도는 개념이 자연스럽게 어디 속하는지를 보여줘야지, 억지로
두 상자 중 하나에 욱여넣으면 안 된다.

## 6. 앞으로 채울 것

이 문서는 core만 다룬다. 아래는 아직 지도가 없는 영역이다 — 각각 별도 조사가
필요할 만큼 크다.

- **framework 계층** — Mesh, MeshNode, Spot, Actor, Channel, Stream, PubSub, Location
  Store. `spec/core/09-runtime-boundary.ko.md` §2가 이 개념들이 Core 밖에 있다는
  것까지만 확인해 준다 — 그 안에서 무엇이 entity고 무엇이 bounded context인지는 이
  문서의 범위 밖이다.
- **bindings 계층** — 언어별 바인딩이 Core의 entity/value object 구분을 각 언어
  타입 시스템으로 어떻게 옮기는지(예: RoutingId를 `byte[]`로 노출할지 전용 타입으로
  감쌀지)는 언어마다 다를 수 있다.

---

> 이 문서에 없는 판단은 [POSDDD](./posddd.ko.md)와
> [ZLink 시스템 개발 원칙](./zlink-system-design-principles.ko.md)을 따른다. 이 문서가
> 다루는 것은 그 두 문서의 원칙을 core의 실제 코드에 적용한 결과다 — 코드가 바뀌면 이
> 문서도 같이 갱신해야 한다.
