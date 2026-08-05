---
title: "Rust 바인딩 구현 청사진"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Go](../go/README.ko.md)
<!-- bindings-nav:end -->

# Rust 바인딩 구현 청사진

> **이 장이 정의하는 것** — 기대되는 Rust crate 형태와 `contracts`/`runtime`
> 소스 배치 규칙.

이 문서는 기대되는 Rust crate 형태를 정의한다. 모든 공개 아이템을 빠짐없이 나열한
목록은 아니다. 구체적인 공개 계약 소스는 `bindings/rust/src/contracts/`이다.
`bindings/rust/src/lib.rs`는 의도된 공개 API를 re-export하는 crate projection이다.

Rust 구현이 정렬된 상태란, `contracts` 소스 트리, private runtime 트리, 공개 export
projection, rustdoc, 테스트, 샘플, perf runner, 런타임 동작이 이 청사진을 따르고
안정된 `core/include/zlink.h` 능력을 Rust 관용 API로 매핑하는 상태이다.

이 README는 `../README.md`의 공통 정책에 정렬된 후의 Rust 바인딩 형태를 기술하며,
동시에 Rust 리팩터 작업의 가이드이기도 하다. 리팩터 진행 중에는 이 문서를 사용해
각 공개 계약, 런타임 구현, 네이티브 브릿지 헬퍼, 테스트, 샘플, perf import가 어디에
속하는지 결정한다. Rust 바인딩이 정렬되었다고 선언되면, 소스 레이아웃, 공개
re-export, rustdoc, 테스트, 샘플, perf, 런타임 동작이 이 문서와 일치해야 한다.

Rust 리팩터는 호환을 깨는 정리 작업이다. 리팩터 이전의 공개 표면을 유지하기 위한
호환 shim, deprecated wrapper, 중복 생성 경로, 옛 re-export alias는 남겨두지
않는다.

이 바인딩은 공통 바인딩 아키텍처 맵을 Rust 명명 규칙으로 따른다. `contracts`와
private `runtime` 모듈이 소스 소유권을 조직화하고, `lib.rs`가 어떤 모듈 경로를
공개 crate API로 만들지 결정한다.

| 절 | 다루는 내용 |
|---|---|
| [공개 계약 소스](#공개-계약-소스) | contracts/runtime/native 소스 위치와 문서 역할 |
| [저장소 레이아웃](#저장소-레이아웃) | 파일 단위 정책과 정렬된 디렉터리 트리 |
| [API 변경 워크플로](#api-변경-워크플로) | 신규 매핑·리팩터 절차, 제거해야 할 단축 경로 |
| [라이브러리 형태](#라이브러리-형태) | RAII 소유권, `Result`, Builder, `unsafe` 경계 |
| [계약 / 런타임 배치 규칙](#계약--런타임-배치-규칙) | 공개 선언과 private 헬퍼의 경계 |
| [계약 파일 레이아웃](#계약-파일-레이아웃) | `contracts/` 하위 카테고리별 파일 |
| [런타임 파일 레이아웃](#런타임-파일-레이아웃) | `runtime/` 하위 카테고리별 파일 |
| [생성 진입점](#생성-진입점) | crate root 생성자와 계약 메서드 목록 |
| [계약 카테고리 맵](#계약-카테고리-맵) | 카테고리 → 모듈 매핑 |
| [표준 인터페이스 규칙](#표준-인터페이스-규칙) | recv 시그니처, builder 시작 메서드, 이름 제약 |
| [Crate 레이아웃](#crate-레이아웃) | 공개 모듈 분류 |
| [필수 능력 커버리지](#필수-능력-커버리지) | 정렬 완료 시 보장해야 할 사용자 대상 능력 |
| [Spot Get-Or-Create](#spot-get-or-create) | `get_or_create_spot` 계약 |
| [Receive와 Subscribe 형태](#receive와-subscribe-형태) | 저장소 재사용, no-data 구분 |
| [에러 및 검증 정책](#에러-및-검증-정책) | FFI 경계 검증과 타입 기반 에러 |
| [성능 정책](#성능-정책) | hot path 제약 |
| [구현 체크리스트](#구현-체크리스트) | 정렬 선언 전 확인 항목과 필수 검증 명령 |
| [Actor와 Spot Route 결과](#actor와-spot-route-결과) | route 결과 값 타입과 Actor 대상 send/request |

## 공개 계약 소스

- 공개 계약 소스: `bindings/rust/src/contracts/`.
- Crate projection: `lib.rs`의 공개 re-export와 공개 모듈에 대한 rustdoc.
- 런타임 구현: `bindings/rust/src/runtime/` 아래 private 모듈.
- 네이티브 브릿지: `bindings/rust/src/runtime/native/` 아래 private 모듈, raw 핸들,
  콜백 trampoline, request progress 헬퍼, part-loop 헬퍼.
- 구체적인 crate-private 리소스 저장소는 `bindings/rust/src/internal.rs`에 둔다.
  계약 파일은 이 저장소 타입을 참조할 수 있지만 runtime 리소스 타입을 직접 import하지
  않는다. FFI 선언과 native 호출은 계속 `runtime/` 아래에 둔다.
- 문서의 역할: 이 README는 형태와 의미 커버리지를 정의한다. 공개 crate export가
  정확한 멤버 목록을 소유한다. 각 공개 아이템은 여전히 공통 계약 카테고리 중
  하나에 매핑되어야 한다.

애플리케이션, perf, 샘플은 private 모듈이나 raw FFI 바인딩에 의존하지 않는다.

## 저장소 레이아웃

Rust 바인딩을 변경할 때 이 경로를 일관되게 사용한다.

- 공개 계약: `bindings/rust/src/contracts/`.
- Crate projection: `bindings/rust/src/lib.rs`.
- 런타임 구현: `bindings/rust/src/runtime/` 아래 private 모듈.
- crate-private 구체 저장소: `bindings/rust/src/internal.rs`.
- 네이티브 브릿지/아티팩트: `bindings/rust/src/runtime/native/`,
  `bindings/rust/native/`, `bindings/rust/include/` 아래 private 모듈.
- 코덱 crate: 제공하지 않는다. Rust 바인딩은 raw `Message`와 byte payload API만
  유지한다.
- 테스트: `bindings/rust/tests/`.
- 샘플: `bindings/rust/samples/`.
- Perf: `bindings/rust/perf/`.

`lib.rs`의 공개 re-export는 의도된 것이어야 한다. Rust 모듈 경로는 export되는
순간 공개 API의 일부가 된다. `contracts`와 `runtime` 소스 트리는 `lib.rs`가
명시적으로 모듈을 노출하지 않는 한 구현 조직일 뿐이다. `zlink::runtime`이나 raw
네이티브 브릿지 모듈을 노출하지 않는다.

다음 트리는 정렬된 구현 구조이다. 공개 struct, enum, trait, error, free function,
builder 계약은 `contracts/`에 속하며 `lib.rs`에서 의도적으로 re-export된다. FFI
바인딩, 네이티브 struct mirror, 콜백 trampoline, request progress 헬퍼,
marshalling, unsafe part loop는 `runtime/` 아래 private으로 유지한다. crate-private
저장소 모듈에는 public wrapper가 소유해야 하는 구체 상태만 두며 FFI 표면을 선언하거나
호출하지 않는다.

파일 단위는 `../README.md`의 공통 정책을 따른다. 독립적인 공개 개념 하나 또는
긴밀한 operation/model 그룹당 파일 하나를 유지한다. 매우 작은 marker trait,
콜백 alias, enum 전용 모듈, pass-through 헬퍼 모듈은 공개 형태가 읽기 쉬워지는
경우 인접 계약 파일로 병합한다.

```text
bindings/rust/
+-- src/
|   +-- lib.rs
|   +-- internal.rs
|   +-- contracts/
|   |   +-- core/
|   |   |   +-- context.rs
|   |   |   +-- routing_id.rs
|   |   +-- messaging/
|   |   |   +-- message.rs
|   |   |   +-- received.rs
|   |   |   +-- topic_message.rs
|   |   |   +-- subscription_event.rs
|   |   +-- sockets/
|   |   |   +-- socket.rs
|   |   |   +-- pair_socket.rs
|   |   |   +-- dealer_socket.rs
|   |   |   +-- router_socket.rs
|   |   |   +-- pubsub_sockets.rs
|   |   |   +-- stream_socket.rs
|   |   |   +-- socket_options.rs
|   |   |   +-- socket_operations.rs
|   |   +-- eventing/
|   |   |   +-- monitor.rs
|   |   |   +-- poller.rs
|   |   +-- service/
|   |   |   +-- spot/
|   |   |   |   +-- spot_node.rs
|   |   |   |   +-- spot.rs
|   |   |   |   +-- actor.rs
|   |   |   |   +-- spot_operations.rs
|   |   |   |   +-- spot_models.rs
|   |   +-- errors/
|   |   |   +-- errors.rs
|   |   |   +-- results.rs
|   +-- runtime/
|   |   +-- messaging/
|   |   |   +-- message.rs
|   |   |   +-- domain.rs
|   |   |   +-- request_progress.rs
|   |   +-- sockets/
|   |   |   +-- socket_base.rs
|   |   |   +-- pair_socket.rs
|   |   |   +-- dealer_socket.rs
|   |   |   +-- router_socket.rs
|   |   |   +-- pub_socket.rs
|   |   |   +-- sub_socket.rs
|   |   |   +-- xpub_socket.rs
|   |   |   +-- xsub_socket.rs
|   |   |   +-- stream_socket.rs
|   |   +-- eventing/
|   |   |   +-- poller.rs
|   |   |   +-- timer.rs
|   |   +-- service/
|   |   |   +-- spot/
|   |   |   |   +-- spot_node.rs
|   |   |   |   +-- spot.rs
|   |   |   |   +-- actor.rs
|   |   +-- errors/
|   |   |   +-- native_errors.rs
|   |   +-- native/
|   |   |   +-- native.rs
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- include/
```

공개 소비자 projection은 `lib.rs`이다. 테스트, 샘플, perf는 공개 crate projection을
사용한다. 어떤 아이템이 공개적으로 re-export되었다면, 리뷰어는 그것이 속한 공통
계약 카테고리를 지목할 수 있어야 한다. 모듈이 네이티브 호출이나 unsafe 불변량
유지를 위해서만 존재한다면 `runtime/` 아래 private으로 유지한다.

## API 변경 워크플로

새로운 core 능력을 매핑할 때.

1. 공개 동작을 소유할 공통 계약 카테고리를 선택한다.
2. 안전한 소유 모듈에 공개 타입, 메서드, 함수를 추가하고, crate root에 보여야 할
   경우 `lib.rs` re-export projection을 갱신한다.
3. 구체적인 공개 타입이나 메서드를 먼저 추가하고, 실제로 대체 가능한 동작이
   필요한 경우에만 trait를 추가한다.
4. `unsafe`, raw 핸들, 콜백 userdata, part loop는 private 모듈 안에 유지한다.
5. 실패 가능한 작업은 typed error 정보를 담아 `Result`를 반환한다.
6. 공개 crate projection을 사용하는 테스트를 추가한다.
7. 샘플과 perf는 공개 API만 통해 갱신한다.
8. 사용 가능한 환경에서 포매팅과 clippy 스타일 검사를 실행한다.

기존 코드를 이 형태로 리팩터할 때.

1. 공개 동작 선언을 `src/contracts/<category>/`로 옮긴다.
2. 네이티브 기반 구현을 `src/runtime/<category>/`로 옮긴다.
3. unsafe FFI, 네이티브 로딩, raw 핸들, 콜백 trampoline은 `src/runtime/native/`
   아래에 유지한다.
4. 공개 코드의 직접적인 런타임 생성을 crate root 생성자 또는 계약 메서드로
   대체한다.
5. 런타임 모듈을 공개 API로 노출하는 호환 re-export를 제거한다.
6. deprecated wrapper, 중복된 operation-start 이름, 옛 명명 alias는 shim으로
   유지하지 않고 제거한다.
7. 테스트, 샘플, perf가 공개 crate export만 사용하도록 갱신한다.
8. rustdoc을 재생성/검토하여 private 구현 모듈이 공개 API로 나타나지 않도록
   한다.

리팩터는 아래 Rust 고유의 단축 경로가 제거되어야 비로소 완료된다.

- `contracts` 모듈은 `runtime`이나 `runtime::native`를 re-export하지 않는다.
- 계약 파일은 공개 service model을 기술하기 위해 런타임 리소스 타입을 import하지
  않는다.
- 공개 re-export barrel은 리소스 동작의 출처가 되지 않는다. 선언을 명명된 계약
  모듈과 런타임 구현 모듈로 분리한다.
- `lib.rs`는 계약 이름과 생성자를 export하며, 런타임 모듈을 export하지 않는다.
- 공개 rustdoc은 런타임 구현 모듈 경로를 공개 타입으로 노출하지 않는다.

## 라이브러리 형태

이 바인딩은 네이티브 런타임 위의 안전한 Rust crate처럼 느껴져야 한다.

- 공개 리소스 타입은 네이티브 lifetime을 소유하고 `Drop`을 통해 리소스를 해제한다.
- 실패 가능한 작업은 `Result<T, ZlinkError>`를 반환하거나, 명확성이 더 좋아질
  경우 더 구체적인 typed result를 반환한다.
- 메시지, routing id, received metadata, topic message, snapshot, option, enum,
  error 같은 구체값은 구체 타입으로 유지한다.
- Trait는 호출자에게 대체 가능한 동작이나 generic bound가 필요할 때만 사용한다.
  모든 구체 핸들마다 기본으로 trait를 정의하지 않는다.
- Multipart send, publish, request, reply, SPOT, actor 작업에는 Builder가
  필수이며, 이를 통해 네이티브 상태를 숨긴다.
- `unsafe`와 raw FFI는 private 모듈로 한정한다.

### Safe FFI RAII Wrapper 배치

네이티브 기반 Rust 리소스는 Rust에서 일반적인 safe FFI RAII wrapper 패턴을
사용한다. 공개 `struct` 핸들이 네이티브 lifetime을 소유하고, 공개 inherent
`impl` 메서드가 안전한 Rust 작업을 노출하며, `Drop`이 네이티브 리소스를 해제한다.

공개 inherent `impl` 표면은 대응하는 `contracts/` 소유 파일에 둔다. 메서드 본문이
즉시 런타임 헬퍼로 위임하더라도 공개 메서드 목록은 계약 파일에서 읽을 수 있어야
한다. 런타임 모듈은 C API 호출, `unsafe` 블록, raw 핸들, downcast, errno 매핑,
네이티브 struct 변환을 `pub(crate)` 헬퍼 함수 뒤에 숨긴다. 공개 리소스의 공개
메서드를 런타임 모듈에서만 발견할 수 있는 상태로 두지 않는다.

Trait는 호출자에게 대체 가능한 동작이나 generic bound가 필요할 때만 사용한다.
네이티브 기반 핸들을 인터페이스처럼 보이게 하려고 trait를 만들지 않는다. 구현체가
하나인 C 핸들 wrapper는 `contracts/`의 `pub struct`와 공개 inherent 메서드,
`runtime/`의 private 헬퍼 조합을 우선한다.

## 계약 / 런타임 배치 규칙

- 공개 struct, enum, trait, error, builder 계약은 매칭되는 `contracts/`
  카테고리에 속하며, 공개일 경우 `lib.rs`에서 re-export된다.
- 공개 free function, 연관 헬퍼 함수, convenience 메서드, builder 헬퍼 메서드는
  호출자가 직접 사용할 수 있을 때 공개 모듈에 속한다.
- 네이티브 기반 공개 리소스의 공개 inherent `impl` 블록은 계약 소유 파일에 둔다.
  본문은 얇게 `pub(crate)` 런타임 헬퍼로 위임할 수 있다.
- 런타임 핸들 소유자, request pump, 콜백 adapter, part-loop 헬퍼는 private
  또는 `pub(crate)`로 유지한다.
- FFI 바인딩, raw 포인터, 네이티브 struct mirror, marshalling 헬퍼, 플랫폼
  로딩 코드는 private FFI/런타임 소유자 안에 유지한다.
- `lib.rs`와 공개 rustdoc 모듈은 계약 카테고리를 projection하며, 런타임 모듈을
  노출하지 않는다.
- 런타임 구체 타입은 crate root 생성자 또는 계약 메서드 뒤에 있는 생성 대상이다.
  `lib.rs`는 그러한 생성자를 연결하기 위해서만 런타임 모듈을 import할 수 있으며,
  공개 시그니처는 계약 이름을 사용한다.

## 계약 파일 레이아웃

계약 소스는 [.NET 바인딩 청사진](../dotnet/README.ko.md)과 같은 분류를 Rust 명명 규칙으로 가진다.
동일한 개념적 파일 그룹화를 유지하여, 다른 바인딩을 아는 개발자가 Rust에서도 같은
공개 개념을 빠르게 찾을 수 있도록 한다.

- `core/`: `context.rs`, `routing_id.rs`, 그리고 core option/value 파일들.
- `messaging/`: `message.rs`, `received.rs`, `topic_message.rs`,
  `subscription_event.rs`, 공통 operation payload 타입.
- `sockets/`: socket 타입/trait, socket option 타입, send/request/reply builder
  계약, stream packet handler 계약, socket flag.
- `eventing/`: monitor, monitor event/status, poller, poll event, timer, event
  handler 계약.
- `service/`: SPOT node, Spot, Actor, topology model, service operation builder를
  담는 `spot/` 하위 모듈로 둔다.
- `errors/`: 공개 error 타입, result 도메인, error-code 매핑.

공개 리소스 동작을 위한 단일 통합 `models.rs`나 런타임 export barrel은 피한다.
의미를 부여하는 계약과 함께 묶일 수 있는 작은 DTO 형 struct와 enum은 그룹화할 수
있지만, 네이티브 기반 리소스와 operation builder는 이름이 부여된 계약 파일이
필요하다.

## 런타임 파일 레이아웃

런타임 소스는 동일한 개념적 카테고리를 미러링하지만 구현만 포함한다.

- `core/`: context 구현과 context option 헬퍼.
- `messaging/`: 메시지 materialization, request progress, request 실행,
  네이티브 버퍼 변환 헬퍼.
- `sockets/`: socket base 타입, socket kernel, 모든 socket family의 socket 구현,
  콜백 adapter, operation 구현 타입.
- `eventing/`: poller/timer/monitor 구현과 event materialization 헬퍼.
- `service/`: SPOT node, Spot, Actor, topology, service
  operation 구현.
- `errors/`: 네이티브 에러 변환과 검증 헬퍼.
- `native/`: FFI 바인딩, 네이티브 로딩, raw 핸들, unsafe 경계 코드.

런타임 모듈은 계약 타입을 import할 수 있지만, 계약 모듈은 런타임 모듈을 import하지
않는다. Crate root는 생성자에서 런타임 구현을 인스턴스화할 수 있지만, 런타임
구현 모듈이 아닌 계약 이름을 export한다.

## 생성 진입점

공개 생성은 crate root 생성자와 공개 계약 메서드를 통해 제공된다.

- `Context::new(...)`는 네이티브 기반 context 구현을 생성한다.
- `Context::create_pair_socket()`, `create_dealer_socket()`,
  `create_router_socket()`, `create_pub_socket()`, `create_sub_socket()`,
  `create_xpub_socket()`, `create_xsub_socket()`, `create_stream_socket()`은
  네이티브 기반 socket 구현을 생성한다.
- `Context::create_spot_node(...)`는 service 계층 구현을 생성한다.
- `Spot` 핸들은 `SpotNode::create_spot(...)`, `entry_spot()`,
  `get_or_create_spot(...)`, 또는 `spot_lookup(...)`을 통해 얻는다. 직접적인
  `Spot` 생성은 공개되지 않는다.
- Actor 핸들은 `SpotNode::create_actor(...)`을 통해 생성된다. 직접적인 Actor
  생성은 공개되지 않는다.
- `Poller::new(...)`, `Timer::new(...)`, 그리고 timer-on-SPOT 헬퍼가 eventing
  리소스를 생성한다.
- `AtomicCounter::new()`, `Stopwatch::start()`, `Thread::start(...)`는 호출자가
  소유하는 유틸리티 리소스를 생성한다.
- Version, capability 조회, strerror, proxy, sleep, multipart cleanup 헬퍼는 공개
  crate 함수이다. 이들 함수 뒤의 FFI 호출은 private으로 유지된다.

## 계약 카테고리 맵

이 카테고리들은 `bindings/rust/src/contracts/` 아래 소문자 모듈에 매핑되며,
공개 crate 아이템과 re-export의 리뷰 소유권 맵이다.

- `core/`: context, context option, routing id, version/capability 조회 헬퍼, 유틸리티
  계약.
- `messaging/`: message, received metadata, topic message, subscription event,
  stream packet 데이터, builder payload 헬퍼.
- `sockets/`: socket 동작, socket family, typed option, request/reply,
  publish/subscribe 표면.
- `eventing/`: monitor, monitor snapshot/event, poller, poll event, timer, 공개
  poll 헬퍼.
- `service/`: SPOT node, SPOT 핸들, topology model, actor
  ref, actor lifecycle, operation builder.
- `errors/`: typed error/result 도메인.
- Enum, flag, result 타입은 그 의미를 정의하는 카테고리에 둔다. 구문으로 선언을
  묶기 위해 `enums` 모듈을 만들지 않는다.

## 표준 인터페이스 규칙

- 데이터 플레인 `recv`, routed recv, `subscribe`, subscription-event receive는
  호출자가 제공한 `&mut Received`, `&mut TopicMessage`, 또는
  `&mut SubscriptionEvent` 값을 채우고 `Result<bool, RecvError>`를 반환한다.
- Send, routed send, publish, request, reply, SPOT 작업, Actor
  location/session 작업은 typestate builder를 반환한다.
- Builder의 start 메서드는 대상 identity, topic, channel, routing id 또는
  request sequence만 받는다. Payload, flag, timeout, callback, async submit
  선택은 builder의 상태 또는 단계이다.
- SPOT의 채널 지정 작업은 `send_to_channel(...)`과 `request_to_channel(...)`을
  사용한다. SPOT의 토픽 publish는 `publish(topic)`을 그대로 유지한다.
- Operation 시작 메서드와 동일한 이름의 단일 payload 단축 메서드를 추가하지
  않는다. `send(message)`, `send(routing_id, message)`,
  `publish(topic, message)`, `send_to_channel(channel, message)`,
  `send_to_spot(..., message)`는 공개 계약 멤버가 아니다. 호출자는
  `send(...).message(message).submit()`을 사용한다.
- Multipart payload는 반복된 `message(...)` 호출로 누적된다. `messages(...)`
  편의 메서드는 동일한 builder 계약에 위임하고 공개 crate 표면에 선언될 때만
  허용된다.
- Dealer 소켓은 `request_frame(...)`이나 `reply(request_token, parts)` 같은
  프로토콜 envelope 헬퍼를 노출하지 않는다. Dealer는 `request()`로 요청을 시작할
  수는 있지만, API 수준의 peer routing id가 없으므로 임의의 token에 대해 답신할
  수 없다.
- Message payload 팩토리는 실패 가능한 from-source 계약을 사용한다.
  `Message::try_from(...)`과 `TryFrom` 구현이다. `copy_from` 같은 copy 전용
  이름은 공개 계약의 일부가 아니다.
- Routing id 생성은 표준 `From` 구현을 사용한다. `from_bytes`,
  `from_string`, `from_u32`, `from_uuid_bytes` 같은 public helper는 공개 계약의
  일부가 아니다. hex 디코딩은 `from_hex` / `try_from_hex`를 유지할 수 있다.
- `send_no_wait`, `publish_with_flags`, `request_async` 같은 operation-start
  메서드 패밀리를 추가하지 않는다. 하나의 operation 이름을 유지하고 변형은
  builder가 흡수하게 한다. async 표면도 operation 시작점 이름을 늘리지 않고
  builder terminator 또는 `Future` 반환 표면으로 표현한다.

## Crate 레이아웃

Crate는 명확한 공개 모듈 또는 re-export를 노출해야 한다.

- Core: context, option, version/capability 조회 헬퍼, 유틸리티.
- Messaging: message, routing id, received metadata, topic message,
  subscription event, stream packet 데이터.
- Sockets: pair, dealer, router, pub, sub, xpub, xsub, stream, typed option,
  콜백, request/reply, publish/subscribe, stream packet API.
- Eventing: monitor, monitor snapshot/event, poller, poll event, timer.
- Service: SPOT node, SPOT 핸들, topology snapshot, actor
  ref, actor lifecycle, operation builder.
- Error: core 의미를 보존하는 typed error/result 도메인.

공개 crate는 자주 쓰이는 타입을 crate root에서 re-export할 수 있지만, private
FFI 모듈은 private으로 유지한다.

## 필수 능력 커버리지

공개 crate는 바인딩이 공통 .NET 기준 정책에 정렬되었을 때 다음 안정된 사용자
대상 능력을 커버해야 한다.

- Context lifecycle, option, shutdown, auto-HWM 재계산, version, capability 조회,
  strerror.
- Message ownership, multipart payload, routing id, received metadata, topic
  message, subscription event, stream packet 콜백.
- 모든 socket family와 그 typed option. `SubSocket::subscription_at(index)`와
  `XSubSocket::subscription_at(index)`는 해당 인덱스의 subscription filter와
  pattern 여부를 반환한다. 해당 인덱스가 없으면 `None`을 반환한다.
- Monitor, poller, timer, readiness 의미.
- SPOT node, SPOT 핸들, topology snapshot, actor, stream
  actor binding.

Rust 이름과 ownership 모델은 C와 다를 수 있지만, 동작은 core 능력의 의미와
일치해야 한다.

## Spot Get-Or-Create

Rust는 `SpotNode::get_or_create_spot(&RoutingId) -> Result<(Spot, bool),
ConfigError>`를 노출한다. 이것은 `zlink_spot_node_spot_get_or_new(...)`에
직접 매핑되며, `spot_lookup`과 `create_spot`을 조합해서 구현하지 않는다.

반환된 `Spot`은 호출자가 소유하며 일반적인 `Drop` lifetime 규칙을 따른다.
boolean은 논리적 spot을 생성한 호출에서만 `true`이다.

## Receive와 Subscribe 형태

- 데이터 플레인 receive와 subscribe API는 재사용 가능한 호출자 소유 결과
  저장소를 사용한다.
- non-blocking no-data는 hard receive 실패와 구별된다.
- SPOT readable dispatch 이벤트는 readiness 알림이다. 호출자는 매칭되는 receive
  API를 no-data가 될 때까지 비운다.
- 반환된 메시지 데이터는 명확한 ownership과 lifetime을 가진다. borrowed 데이터는
  네이티브 owner보다 오래 살아남지 않는다.
- Actor join request receive 같은 service control/admission receive 경로는
  재사용 데이터 플레인 저장소보다 명확할 경우 `Option`, nullable 등가물, 또는
  typed result-return 형식을 사용할 수 있다. 그래도 no-data와 hard receive
  실패는 구분한다.

## 에러 및 검증 정책

- 네이티브 고정 크기 id와 문자열은 FFI 경계를 넘기 전에 검증한다.
- routing id, actor id, endpoint, channel 이름, topic을 조용히 자르지 않는다.
- submit, request, recv, handler, close, bind, connect, config 에러 도메인을
  보존한다.
- 공개 에러는 문자열 파싱이 아니라 Rust 타입으로 검사할 수 있어야 한다.

## 성능 정책

- Hot path는 피할 수 있는 dynamic dispatch, 피할 수 있는 할당, 피할 수 있는
  바이트 복사, 숨겨진 sleep, busy wait, 광범위한 lock, thread join을 사용하지
  않는다.
- FFI 브릿지 코드는 core part substrate에서 직접 공개 Rust 값을
  materialize해야 한다.
- 핸들 단위로 progress를 공유할 수 있을 때 요청마다 thread나 timer를 하나씩
  두지 않는다.
- Perf, 샘플, 테스트는 공개 crate API만 사용한다.

## 구현 체크리스트

- 공개 export는 의도된 것이며 문서화되어 있다.
- Raw FFI와 unsafe 상태는 공개 API를 통해 누출되지 않는다.
- 리소스 ownership은 Rust 타입과 `Drop`으로 강제된다.
- Trait는 실제 추상화 지점에서만 사용된다.
- 공개 free function과 builder convenience 메서드는 런타임 헬퍼가 아니라 공개
  crate 모듈에 선언된다.
- Receive/subscription 의미는 공통 바인딩 정책과 일치한다.
- 데이터 플레인의 호출자 제공 저장소와 다른 service control/admission receive
  예외는 문서화된다.
- Perf 의미는 `bindings/c/perf`와 일치한다.
- `src/contracts`는 `src/runtime`에 대한 import 또는 export 의존성을 갖지
  않는다.
- `lib.rs`는 생성자 연결을 위해서만 런타임 모듈을 import하며, 런타임 모듈이나
  런타임 구현 타입 이름을 export하지 않는다.
- 테스트, 샘플, perf는 private 런타임 import를 사용하지 않는다.
- 네이티브 기반 리소스는 crate root 생성자 또는 계약 메서드를 통해 생성되며,
  공개 계약 타입으로 타이핑된다.
- 호환을 위해서만 유지되는 옛 alias, 중복된 operation-start 이름, deprecated
  wrapper는 남기지 않는다.

Rust 리팩터 이후 필수 검증. `bindings/rust/`에서 다음 명령을 실행한다.

- `cargo fmt --all --check`를 실행한다.
- `cargo test --workspace --all-targets`를 실행한다.
- clippy를 사용할 수 있을 때
  `cargo clippy --workspace --all-targets -- -D warnings`를 실행한다.
- `./tests/run_tests.sh`를 실행한다.
- 공개 예제나 생성 경로가 변경되었을 때 `./samples/run_samples.sh`를 실행한다.
- Hot path, receive, send, request, poller, timer, 또는 service 동작이
  변경되었을 때 smoke gate로 `./perf/run_benchmarks.sh`와
  `./perf/run_benchmarks_multi.sh`를 실행한다.
- rustdoc/공개 re-export를 검사하여, crate export가 런타임 구현 모듈이 아닌
  계약 타입을 노출하는지 확인한다.
- `src/contracts`, `tests`, `samples`, `perf`에서 `crate::runtime`,
  `runtime::native`, raw FFI 모듈, 또는 생성된 private 파일로부터의 import를
  검색한다. `src/lib.rs`는 별도로 확인하여, 런타임 import가 생성자 연결에만
  쓰이고 공개 시그니처에 나타나지 않는지 확인한다.

## Actor와 Spot Route 결과

Rust는 Actor와 Spot route 조회 결과를 공개 값 타입으로 노출한다.

- `ActorRoute`는 해석된 Actor ref, Actor node RID, 현재 Spot RID, 현재 Spot
  kind를 보존한다.
- `SpotRoute`는 Spot RID, 소유자 node RID, Spot kind를 보존한다.
- `SpotKind`는 Entry Spot과 사용자 Spot을 구분한다. 잘못된 kind는 성공한 route
  결과가 아니다.
- SpotNode snapshot 항목은 core snapshot과 동일한 Spot kind/현재 Spot 필드를
  노출한다.

- Rust는 resolve된 Actor ref를 인자로 받는 `SpotNode::send_to_actor(&ActorRef)`와 `SpotNode::request_to_actor(&ActorRef)`를 노출한다.
- send operation은 submit이 성공하면 하나 이상의 message part 소유권을 넘기고, Actor 소유자 mailbox가 인계를 받으면 완료된다.
- request operation은 submit이 성공하면 요청 part의 소유권을 넘기고, Actor handler가 만든 reply part를 전달한다.
- Rust는 제거된 Discovery route table이나 resolver API를 compatibility helper로 되살리면 안 된다.
