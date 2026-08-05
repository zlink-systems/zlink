---
title: "C++ 바인딩 최종 구조"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: .NET](../dotnet/README.ko.md) | [다음: Java](../java/README.ko.md)
<!-- bindings-nav:end -->

# C++ 바인딩 최종 구조

> **이 장이 정의하는 것** — 바인딩 리팩터링이 끝난 뒤 C++ 라이브러리가 가져야 할
> `Contracts`/`Runtime` 형태와 필수 의미 범위.

이 문서는 바인딩 리팩터링이 끝난 뒤 C++ 라이브러리가 가져야 할 형태를 정의한다.
모든 메서드를 빠짐없이 열거하지 않는다. 구체적인 공개 계약은
`bindings/cpp/include/zlink/Contracts/`에 있다.

완성된 구조에서는 `Contracts/`, 설치되는 헤더 투영, 테스트, 샘플, perf 러너, 런타임
동작이 모두 `core/include/zlink.h`의 안정적인 코어 기능을 C++ 관용 타입으로 매핑한다.

이 README는 `../README.md`의 공통 정책을 C++ 바인딩이 최종 상태에서 어떻게 해석하는지
정의한다. 완성된 C++ 바인딩은 과거 표면을 보존하기 위한 기존 include 경로, 별칭 메서드,
다른 투영, 래퍼 계층을 유지하지 않는다. 이 문서가 리팩터링된 C++ 바인딩의 수용 기준이다.

이 바인딩은 공통 바인딩 아키텍처 지도를 C++ 네이밍으로 따른다. `Contracts/`는 설치되는
공개 헤더를 소유하고, `Runtime/`은 `src/` 아래 비공개 구현을 소유한다. 폴더 이름은 저장소
구성을 위한 것이지 사용자가 의존해야 하는 네임스페이스 분절이 아니다.

| 절 | 다루는 내용 |
|---|---|
| [공개 계약 소스](#공개-계약-소스) | Contracts/Runtime 소스 위치, 언어 기준, 비동기 표면 정책 링크 |
| [저장소 레이아웃](#저장소-레이아웃) | 정렬된 디렉터리 트리와 파일 세분화 정책 |
| [.NET 계약 카테고리 투영](#net-계약-카테고리-투영) | .NET 레이아웃을 C++ 네이밍으로 투영하는 방법 |
| [공개 계약 한눈에 보기](#공개-계약-한눈에-보기) | 영역 → 공개 객체 → 소유 헤더 표, 구체 파사드 예시 |
| [코어 기능 소유 규칙](#코어-기능-소유-규칙) | 새 기능을 추가할 때 따르는 절차 |
| [라이브러리 형태](#라이브러리-형태) | RAII, snake_case, Pimpl, 빌더 필수화 규칙 |
| [계약/런타임 배치 규칙](#계약런타임-배치-규칙) | 공개 선언과 런타임 헬퍼의 경계 |
| [빌드 및 패키징 정책](#빌드-및-패키징-정책) | `zlink_cpp` 타깃 빌드·링크·설치 규칙 |
| [계약 폴더 레이아웃](#계약-폴더-레이아웃) | `Contracts/` 하위 카테고리별 소유 범위 |
| [표준 인터페이스 규칙](#표준-인터페이스-규칙) | recv 시그니처, 빌더, handler 이름 규칙 |
| [64-bit byte HWM과 monitoring 계약](#64-bit-byte-hwm과-monitoring-계약) | `byte_count_t` 표현과 monitor snapshot field |
| [기능 범위](#기능-범위) | 완성된 공개 헤더가 다루는 그룹 |
| [수명과 ownership](#수명과-ownership) | 리소스 클래스 해제·move·수신 저장소 규칙 |
| [에러와 result 정책](#에러와-result-정책) | 실패 표현과 result 도메인 |
| [성능 정책](#성능-정책) | hot path·링크 대상 제약 |
| [완성 구조 요구사항](#완성-구조-요구사항) | 완성 선언 전 확인 항목 |
| [Actor와 Spot 라우트 결과](#actor와-spot-라우트-결과) | 라우트 결과 타입과 Actor 대상 send/request |

## 공개 계약 소스

- 공개 계약: `bindings/cpp/include/zlink/Contracts/`.
- 런타임 구현: `bindings/cpp/src/Runtime/`.
- 공개 진입점 투영: `bindings/cpp/include/zlink.hpp`.
- 설치되는 투영: `bindings/cpp/include/zlink.hpp` 및
  `bindings/cpp/include/zlink/Contracts/...`.
- 컴파일된 라이브러리: C++ 바인딩은 코어 네이티브 `zlink` 라이브러리와 별도로 `zlink_cpp`
  같은 C++ 라이브러리 타깃을 빌드하고 설치한다.
- 언어 기준: C++20.
- 네임스페이스: 모든 공개 타입은 `zlink` 아래에 둔다. service 타입은 `zlink::service`
  아래에 둔다.
- 내부 구현: 네이티브 브리지 헬퍼, 콜백 트램펄린, 요청 진행 헬퍼, 비공개 `detail` 헬퍼,
  비공개 구현 헤더, `.cpp` 파일은 `bindings/cpp/src/Runtime/` 아래에 둔다.
- 문서의 역할: 이 README는 형태, 경계, 필수 의미 범위를 정의한다. 정확한 멤버 목록은
  `Contracts/`가 소유하며, 설치되는 헤더는 이를 의도적으로 투영한다.

- C++는 더 이상 header-only 바인딩으로 모델링하지 않는다.
- 두 번째 `bindings/cpp/src/zlink/Contracts/` 트리를 만들지 않는다.
- 공개 계약은 설치되는 헤더에 남고, 구현은 `.cpp` 파일과 비공개 런타임 헤더 뒤로 옮긴다. 계약/런타임 분리는 그대로다.
- `Contracts/`는 사용자 표면을 선언하고, `src/Runtime/`은 그 표면을 위한 구현 지원을 담는다.
- Java나 .NET의 인터페이스 중심 레이아웃을 C++에 그대로 복사하지 않는다.
- C++는 설치되는 헤더, RAII 클래스, 구체 값, 불투명 구현 상태를 자연스러운 경계로 사용한다.

- C++20은 bindings 라이브러리의 최소 지원 범위다.
- bindings 라이브러리는 `async_result_t<T>` 기반 완료 객체와 callback submit을 제공할 수 있지만, coroutine awaiter, framework handler executor, framework dispatcher를 소유하지 않는다.
- framework coroutine은 bindings의 완료 객체나 callback 완료를 framework 실행 경계에서 감싸서 제공한다.
- 언어별 비동기 실행 표면 기준은 [바인딩 비동기 실행 표면 정책](../async-coroutine-policy.md)을 따른다.

## 저장소 레이아웃

완성된 C++ 바인딩은 다음 경로를 일관되게 사용한다. 아래 파일 이름은 목표 소유 지도이며,
공개 개념이나 런타임 책임이 독립적으로 변할 이유가 있을 때만 카테고리를 더 쪼갠다.

```text
bindings/cpp/
+-- CMakeLists.txt
+-- include/
|   +-- zlink.hpp
|   +-- zlink/
|       +-- Contracts/
|       |   +-- Core/
|       |   |   +-- capability.hpp
|       |   |   +-- context.hpp
|       |   |   +-- context_options.hpp
|       |   |   +-- routing_id.hpp
|       |   |   +-- utilities.hpp
|       |   +-- Messaging/
|       |   |   +-- message.hpp
|       |   |   +-- received.hpp
|       |   |   +-- topic_message.hpp
|       |   |   +-- subscription_event.hpp
|       |   |   +-- operation_contracts.hpp
|       |   |   +-- request_result.hpp
|       |   +-- Sockets/
|       |   |   +-- socket_contracts.hpp
|       |   |   +-- message_socket_contracts.hpp
|       |   |   +-- routed_socket_contracts.hpp
|       |   |   +-- pubsub_socket_contracts.hpp
|       |   |   +-- stream_socket.hpp
|       |   |   +-- socket_options.hpp
|       |   |   +-- results.hpp
|       |   +-- Eventing/
|       |   |   +-- monitor.hpp
|       |   |   +-- poller.hpp
|       |   |   +-- poll_event.hpp
|       |   |   +-- timers.hpp
|       |   |   +-- events.hpp
|       |   |   +-- status.hpp
|       |   +-- Service/
|       |   |   +-- spot_node.hpp
|       |   |   +-- spot.hpp
|       |   |   +-- actor.hpp
|       |   |   +-- spot_node_models.hpp
|       |   |   +-- actor_models.hpp
|       |   |   +-- operation_contracts.hpp
|       |   +-- Errors/
|       |       +-- errors.hpp
|       |       +-- results.hpp
+-- src/
|   +-- Runtime/
|       +-- zlink_cpp.cpp
|       +-- Core/
|       |   +-- capability.cpp
|       |   +-- context.cpp
|       |   +-- utilities.cpp
|       |   +-- operation_detail.hpp
|       |   +-- runtime_helpers.hpp
|       |   +-- types_impl.hpp
|       +-- Messaging/
|       |   +-- message.cpp
|       +-- Errors/
|       |   +-- error.cpp
|       +-- Eventing/
|       |   +-- monitor.cpp
|       |   +-- poller.cpp
|       |   +-- timers.cpp
|       +-- Sockets/
|       |   +-- base_socket.cpp
|       |   +-- pair.cpp
|       |   +-- dealer.cpp
|       |   +-- pubsub.cpp
|       |   +-- router.cpp
|       |   +-- stream.cpp
|       |   +-- detail.hpp
|       +-- Options/
|       |   +-- socket_options.cpp
|       +-- Service/
|       |   +-- actor.cpp
|       |   +-- actor_ops.cpp
|       |   +-- detail.hpp
|       |   +-- request_reply.cpp
|       |   +-- spot.cpp
|       |   +-- spot_node.cpp
|       |   +-- actor_detail.hpp
|       |   +-- spot_state.hpp
|       |   +-- spot_submit.hpp
|       +-- Native/
|           +-- socket_handle.hpp
|           +-- native_message_parts.hpp
|           +-- native_parts.hpp
|           +-- native_options.hpp
|           +-- native_send_result.hpp
+-- native/
+-- samples/
+-- tests/
+-- perf/
```

`CMakeLists.txt`는 컴파일된 C++ 바인딩 타깃(예: `zlink_cpp`)을 정의하고 코어 네이티브
`zlink` 라이브러리에 링크한다. 샘플, 테스트, perf 바이너리, 애플리케이션은 비공개 런타임
소스를 직접 컴파일하지 않고 이 타깃에 링크한다.

`Contracts/`는 `bindings/cpp/include/zlink/` 아래에 설치되는 공개 계약 표면이다.
`Runtime/`은 `bindings/cpp/src/Runtime/` 아래의 비공개 구현 지원이다. `zlink`
네임스페이스와 `zlink.hpp`는 그 계약을 C++로 투영한 결과다. `Contracts`나 `Runtime`을
네임스페이스 분절로 노출하지 않는다.

런타임 헬퍼 헤더는 공개 계약 API가 아니다. 공개 샘플, perf, 테스트는 `<zlink.hpp>`를
include하고 C++ 바인딩 라이브러리에 링크한다. 런타임 헬퍼 경로는 include하지 않는다.
`include/zlink/message.hpp`, `include/zlink/services/spot.hpp`,
`include/zlink/sockets/dealer.hpp` 같은 래퍼 헤더는 완성된 레이아웃의 일부가 아니다.
완성된 트리는 이들을 포워딩 헤더로 대체하지도 않는다.

monitor, poller, timer 계약은 공통 `Eventing/` 카테고리 아래에 둔다. `Contracts/Monitoring/`
은 완성된 공개 계약의 일부가 아니며, 완성된 트리는 `Monitoring/` 포워딩 헤더를 유지하지
않는다.

파일 단위 세분화는 `../README.md`의 공통 정책을 따른다. 독립적인 공개 개념 하나, 또는
긴밀한 operation/모델 묶음 하나당 파일 하나를 둔다. 아주 작은 marker, delegate, enum,
pass-through 헬퍼 파일은 공개 형태가 더 잘 읽힐 때 인접한 계약 파일에 합친다.

## .NET 계약 카테고리 투영

C++ 바인딩은 `.NET` 공개 계약 카테고리 레이아웃을 분류 표준으로 사용한다. 이는 카테고리와
책임의 투영이지 C# 형태를 복사한 것이 아니다. C++은 C++20 명명 규칙, 헤더, RAII facade,
이동 의미론, 구체 값 타입을 유지한다.

`.NET`의 파일 목록을 이 문서에 그대로 옮기지 않는다. `.NET`의 단일 기준은
[.NET 바인딩 청사진](../dotnet/README.ko.md), 특히 Contract Folder Layout과 Runtime Folder
Layout 섹션이다. 이 C++ README는 그 카테고리의 C++ 투영만 정의한다.

카테고리 소유 규칙은 엄격하다. 런타임 구현이 다른 위치에 두기 쉽다는 이유로 공개 C++ 타입이
다른 카테고리로 이동하지 않는다. 런타임 헬퍼 코드는 `src/Runtime/` 아래에서 더 세분화될 수
있지만, 공개 계약 소유자는 해당 카테고리에 그대로 남는다.

- 이 투영은 C# 인터페이스 스타일을 엄격하게 따르지 않는다.
- `.NET`의 socket role 인터페이스는 공개 계약에서 socket role을 식별한다. C++가 기본적으로 `isocket_t`, `istream_socket_t`, `ISocket`, `IStreamSocket`을 노출하도록 요구하지 않는다.
- 사용자가 진짜 substitutable한 동작을 필요로 하지 않는 한 구체 RAII facade를 사용한다.
- substitutable한 role이 필요하면 인터페이스를 좁게 유지하고, send/receive/poll/dispatch 핫패스에서 회피 가능한 virtual dispatch가 없도록 한다.

## 공개 계약 한눈에 보기

완성된 C++ 바인딩은 인터페이스 전용 계층을 추가하지 않고도 공개 계약을 보이도록 만든다.
사용자는 `<zlink.hpp>`에서 출발해 다음 지도로 소유 계약 헤더를 찾는다.
.NET 기준의 원본 세부사항은 [.NET 바인딩 청사진](../dotnet/README.ko.md)에 두며,
이 문서에는 C++ 투영만 둔다.

| 영역 | 공개 객체와 역할 | 소유 계약 헤더 |
|------|-------------------|----------------|
| Core | `context_t`, context 옵션, routing id, version/역할 헬퍼 | `Contracts/Core/` |
| Messaging | `message_t`, `received_t`, `topic_message_t`, `subscription_event_t`, multipart 헬퍼 | `Contracts/Messaging/` |
| Sockets | `pair_socket_t`, `dealer_socket_t`, `router_socket_t`, `pub_socket_t`, `sub_socket_t`, `xpub_socket_t`, `xsub_socket_t`, `stream_socket_t`, send/recv/request/reply 빌더 | `Contracts/Sockets/` |
| Eventing | `socket_monitor_t`, monitor 이벤트, poller, poll 이벤트, timer, readiness 헬퍼 | `Contracts/Eventing/` |
| Service | `spot_node_t`, `spot_t`, `actor_ref_t`, actor 생명주기 모델, service operation 빌더 | `Contracts/Service/` |
| Errors | 공개 예외와 result 도메인 타입 | `Contracts/Errors/` |

위 지도는 공개 API 색인이다. 계약 표면 개요의 C++ 등가물이며, `IContext`, `ISpot`,
`IActor` 같은 추상 인터페이스를 의미하지 않는다. 공개 리소스 객체는 호출자가 진정한 대체
동작을 필요로 하지 않는 한 구체 RAII 파사드로 유지한다. 좁은 인터페이스는 codec, callback,
handler, poll target처럼 사용자가 자연스럽게 교체하는 역할에만 허용한다.

공개 계약은 두 단계로 읽는다.

1. `<zlink.hpp>`에서 시작해 C++ 바인딩이 포함하는 공개 계약 카테고리를 본다.
2. 해당 `Contracts/...` 헤더를 열어 구체 공개 타입과 그 공개 멤버 목록을 살핀다.

예를 들어 완성된 SPOT 표면은 인터페이스/구현 쌍이 아니라 구체 파사드로 보인다.

```cpp
namespace zlink::service {

class spot_t {
public:
    spot_t(spot_t&&) noexcept = default;
    spot_t(const spot_t&) = delete;

    send_operation_t send();
    reply_operation_t reply();
    int recv(received_t& out, recv_flags_t flags = recv_flags_t::none);
    void set_send_ready_handler(std::function<void()> handler);
    void close();
};

} // namespace zlink::service
```

`zlink.hpp`는 이 파사드들의 공개 목차 역할을 한다.

```cpp
#include "zlink/Contracts/Core/capability.hpp"
#include "zlink/Contracts/Core/context.hpp"
#include "zlink/Contracts/Core/context_options.hpp"
#include "zlink/Contracts/Core/routing_id.hpp"
#include "zlink/Contracts/Messaging/message.hpp"
#include "zlink/Contracts/Messaging/received.hpp"
#include "zlink/Contracts/Messaging/topic_message.hpp"
#include "zlink/Contracts/Messaging/subscription_event.hpp"
#include "zlink/Contracts/Messaging/operation_contracts.hpp"
#include "zlink/Contracts/Sockets/message_socket_contracts.hpp"
#include "zlink/Contracts/Sockets/routed_socket_contracts.hpp"
#include "zlink/Contracts/Sockets/pubsub_socket_contracts.hpp"
#include "zlink/Contracts/Eventing/poll_event.hpp"
#include "zlink/Contracts/Eventing/poller.hpp"
#include "zlink/Contracts/Service/spot_node.hpp"
#include "zlink/Contracts/Service/spot.hpp"
#include "zlink/Contracts/Service/actor.hpp"
#include "zlink/Contracts/Errors/errors.hpp"
```

런타임 세부사항은 파사드 뒤에 둔다. 공개 헤더는 불투명 구현 상태를 이름지을 수 있으나
네이티브 핸들, 콜백 트램펄린, part 루프, request 펌프, marshalling 헬퍼를 노출하지 않는다.

```cpp
namespace zlink::service {

class spot_t {
public:
    spot_t(spot_t&&) noexcept;
    spot_t(const spot_t&) = delete;
    ~spot_t();

    send_operation_t send();
    reply_operation_t reply();
    void close();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace zlink::service
```

이 구조는 공개 표면을 훑어보기 쉽게 유지하면서 C++ ownership 의미를 보존한다.
`spot_t`, `spot_node_t`, `actor_ref_t`가 계약이고, `src/Runtime/...`과 비공개
`zlink::detail` 헬퍼는 구현 지원이다.

## 코어 기능 소유 규칙

C++가 노출하는 모든 안정 코어 기능은 다음 소유 규칙을 따른다.

1. 올바른 `bindings/cpp/include/zlink/Contracts/` 카테고리에 공개 타입 또는 메서드를
   추가한다.
2. `bindings/cpp/include/zlink.hpp`와 의도적으로 설치하는 투영 헤더를 갱신한다.
3. C++ 도메인 소유자를 결정한다: context, message, socket, monitor, timer, service,
   SPOT, actor, error, option 중 하나.
4. raw C 핸들 접근, `*_part` 루프, callback userdata, 트램펄린 상태, 네이티브 marshalling
   헬퍼는 `src/Runtime/` 헤더와 `.cpp` 파일에 둔다.
5. 새로운 기능이 사용자 워크플로 또는 측정에 영향을 줄 때는 공개 헤더 테스트와 최소 하나의
   샘플/perf 갱신을 추가한다.
6. 새로운 공개 API가 얕은 C 래퍼에 머무르지 않는지 확인한다. ownership, 검증, 형태를
   개선하지 않고 단순히 위임만 한다면 내부로 유지한다.
7. 사용하지 않게 된 공개 이름은 남기지 않는다. 별칭 deprecated, 포워딩 오버로드, 대체
   공개 헤더는 이후 문서가 이 C++ 정책을 명시적으로 바꾸지 않는 한 두지 않는다.

명시적인 Spot routing-id 확보는 C++ 바인딩이
`spot_node_t::get_or_create_spot(routing_id_t)`로 노출하며,
`zlink_spot_node_spot_get_or_new(...)`에 직접 매핑한다. 이 메서드는 소유된 `spot_t`
파사드와 생성 플래그를 반환한다. 이 동작을 `spot_lookup()`과 `create_spot()`을 조합해
구현하지 않는다.

## 라이브러리 형태

C++ 바인딩은 코어 C 계약 위에 얹힌 작은 네이티브 C++ 라이브러리처럼 느껴진다.

- 공개 리소스 객체는 문서화된 수명에 따라 네이티브 핸들을 소유하거나 빌려 쓰는 RAII
  클래스다.
- 소멸자는 호출자가 네이티브 close 순서를 몰라도 리소스를 해제한다. 리소스 소멸자와 그
  외 단순하지 않은 메서드는 `.cpp` 파일에서 out-of-line으로 정의한다.
- 작은 값 타입 연산은 네이티브 ownership, callback 상태, request 상태, marshalling
  세부를 노출하지 않을 때 inline으로 남겨도 된다.
- 공개 메서드는 `snake_case`를 사용한다.
- message, routing id, received metadata, topic message, result, error, enum, option
  같은 공개 값 타입은 구체로 유지한다.
- 공개 리소스 헤더는 네이티브 핸들 레이아웃, callback 상태, request 상태, ABI 민감 저장소가
  계약에 새어 나가는 것을 막기 위해 Pimpl 등 불투명 구현 상태를 쓴다.
- 템플릿, 오버로드, move 의미는 호출자 ownership을 단순화하거나 복사를 피할 때만 쓴다.
  명확한 도메인 타입의 대체로 템플릿 기계장치를 노출하지 않는다.
- 가상 인터페이스는 호출자가 대체 동작을 필요로 할 때만 쓴다. 기본적으로 모든 핸들을
  추상 인터페이스로 감싸지 않는다.
- multipart send, publish, request, reply, actor, SPOT operation은 빌더를 필수로 한다.
  이렇게 해야 네이티브 request 상태가 숨고 ownership이 분명해진다.

## 계약/런타임 배치 규칙

- 공개 선언과 사용자에게 보이는 동작은 `Contracts/`에 둔다.
- 공개 free function, static 헬퍼, extension 스타일 헬퍼, 빌더 편의 헬퍼는 사용자가 직접
  호출할 수 있을 때 `Contracts/`에 둔다.
- 런타임 핸들 소유자, socket 커널, request 펌프, callback 트램펄린, part 루프 헬퍼는
  `src/Runtime/`에 둔다.
- FFI 선언, raw C 핸들, 네이티브 struct mirror, marshalling 헬퍼, 플랫폼 로딩 코드는
  `src/Runtime/Native/`에 둔다.
- `zlink.hpp`는 `Contracts/`를 투영한다. `Runtime/` 헬퍼 경로를 공개 include 스타일로
  만들지 않는다.
- 계약 헤더는 비공개 런타임 헤더를 include하지 않는다. 공개 클래스가 구현 상태를
  필요로 하면 불완전한 `impl` 타입이나 다른 불투명 비공개 멤버만 노출하고 동작은
  `.cpp`에서 정의한다.
- 런타임 구체 클래스는 사용자 진입점이 아니다. 공개 동작이라면 `Contracts/`가 선언하고
  `Runtime/`이 구현한다.

## 빌드 및 패키징 정책

C++가 header-only를 벗어나면 바인딩은 컴파일된 산출물을 하나 더 가진다. 따라서 완성된
바인딩은 다음 빌드 규칙을 유지한다.

- C++ 바인딩은 `zlink_cpp` 라이브러리 타깃을 빌드한다.
- `zlink_cpp`는 코어 네이티브 `zlink` 라이브러리에 링크되며 코어 라이브러리와 버전 호환성
  규칙을 가진다.
- Linux, macOS, Windows 패키지는 지원되는 아키텍처와 런타임 툴체인마다 C++ 라이브러리를
  빌드한다.
- CMake install/export 메타데이터는 애플리케이션이 공개 헤더와 컴파일된 C++ 바인딩 타깃을
  함께 소비할 수 있게 한다.
- 샘플, 테스트, perf 러너는 애플리케이션이 사용하는 동일한 설치 스타일 C++ 타깃에
  링크한다. 비공개 런타임 소스 경로에 의존하지 않는다.
- 런타임 검색 경로, DLL 조회 규칙, 패키징된 네이티브 산출물은 테스트한다. 이제 애플리케이션은
  코어 네이티브 라이브러리와 C++ 바인딩 라이브러리를 함께 로드하기 때문이다.
- 공개 헤더는 ABI 민감 구현 저장소 노출을 피한다. 공개 메서드 시그니처는 C++ 관용을 유지해도
  되지만, 네이티브 핸들 레이아웃, callback 상태, request 상태, marshalling 버퍼는 설치되는
  헤더 밖에 둔다.

## 계약 폴더 레이아웃

`Contracts/`는 공개 C++ 선언의 소스 소유 지도다. `zlink.hpp`는 이 카테고리들을 `zlink`
네임스페이스로 투영한다.

- `Core/`: context, context 옵션, routing id, utility 리소스, 그리고 version 또는
  역할 헬퍼 같은 공개 free function.
- `Messaging/`: message, received metadata, topic message, subscription event, stream
  packet callback, 빌더 payload 헬퍼. codec helper는 C++ 바인딩 package에 포함하지
  않고, framework 수준 직렬화는 framework codec extension에서 다룬다.
- `Sockets/`: socket 동작, socket family, 타입 지정 옵션, request/reply, publish/subscribe
  표면.
- `Eventing/`: monitor, monitor snapshot/event, poller, poll event, timer, 공개 poll
  헬퍼.
- `Service/`: SPOT node, SPOT handle, 토폴로지 모델, actor ref,
  actor 생명주기, operation 빌더.
- `Errors/`: 예외 또는 타입 지정 error-result 도메인.
- enum, flag, result 타입은 의미를 정의하는 카테고리 안에 둔다. 문법별로 묶기 위한
  `Enums/` 폴더를 만들지 않는다.

## 표준 인터페이스 규칙

- data-plane `recv`, routed recv, subscribe, subscription-event 수신은 호출자가 제공하는
  출력 저장소(예: `received_t&`, `topic_message_t&`, `subscription_event_t&`)를 사용한다.
- `message_t::from(...)`은 호출자가 넘긴 바이트를 독립적으로 복사한다. 호출자가 소유한 버퍼를
  복사 없이 메시지로 넘겨야 할 때는 고급 API인
  `external_message_t::from(span, free_fn, hint)` 오버로드를 사용한다. 이 오버로드는 버퍼를
  메시지에 맡기고, 메시지가 버퍼를 해제할 때 `free_fn(data, hint)`를 한 번 호출한다.
- send, routed send, publish, request, reply, SPOT operation, Actor location/session
  operation은 move-only fluent 빌더를 반환한다.
- 빌더 시작 메서드는 대상 identity, topic, channel, routing id, request sequence만
  받는다. payload, flag, timeout, callback, async submit 선택은 빌더 단계에서 한다.
- SPOT 채널 대상 operation은 `send_to_channel(...)`과 `request_to_channel(...)`을 쓴다.
  SPOT topic publish는 `publish(topic)`을 그대로 쓴다.
- handler 등록 메서드는 `set_..._handler` 이름을 쓴다. 예를 들어 send readiness는
  `set_send_ready_handler(...)`, raw STREAM packet 처리는 `set_packet_handler(...)`,
  monitor 이벤트는 `set_monitor_handler(...)`, SPOT dispatch는
  `set_dispatch_handler(...)`를 쓴다.
- `on_...` 이름은 완성된 C++ API에서 공개 등록 메서드가 아니다. 이는 필요할 때 내부 또는
  protected 훅을 위해 예약한다.
- operation 시작 메서드와 같은 이름의 단일 payload 단축 오버로드를 추가하지 않는다.
  `send(message)`, `send(routing_id, message)`, `publish(topic, message)`,
  `send_to_channel(channel, message)`, `send_to_spot(..., message)`는 공개 계약 멤버가
  아니다. 호출자는 `send(...).message(message).submit()`을 쓴다.
- multipart payload는 `message(...)`를 반복 호출해 쌓는다. `messages(...)` 편의 메서드는
  동일한 빌더 계약에 위임하고 `Contracts/`에 선언될 때만 허용한다.
- Dealer socket은 `request_frame(...)`이나 `reply(request_token, parts)` 같은 프로토콜
  envelope 헬퍼를 노출하지 않는다. Dealer는 `request()`로 request를 시작할 수 있지만 API
  수준 peer routing id가 없으므로 임의 token에 reply할 수 없다.
- `send_no_wait`, `publish_with_flags`, `request_async` 같은 operation 시작 오버로드
  계열을 추가하지 않는다. operation 이름은 하나로 유지하고 변형은 빌더가 흡수한다. 종단
  빌더 메서드의 언어별 이름은
  [바인딩 비동기 실행 표면 정책](../async-coroutine-policy.md)을 따른다.
- `on_send_ready(...)`, `on_packet(...)`, `on_event(...)` 같은 표준 이름 우회나 operation
  별칭을 두지 않는다. 호출 지점은 계층화된 별칭 대신 표준 공개 계약을 그대로 쓴다.

## 64-bit byte HWM과 monitoring 계약

- HWM과 Auto HWM planning unit은 `byte_count_t`로 표현한다.
- 이 값 타입은 `uint64_t` byte만 보관하며 `bytes(...)` 생성 함수와 `bytes()` 조회 함수로 단위를 드러낸다.
- 이전 `message_count_t`는 alias나 adapter로 유지하지 않는다.
- `0`은 HWM에서 무제한을 뜻하며, 수동 기본값은 `4,096,000 bytes`다.

```cpp
auto options = socket.options ();
options.send_hwm (zlink::byte_count_t::bytes (send_limit)); // Send pipe의 byte HWM을 정한다.
options.recv_hwm (zlink::byte_count_t::bytes (0));          // 0은 무제한 receive HWM이다.

auto context_options = context.options ();
context_options.auto_hwm_msg_unit_bytes (
  zlink::byte_count_t::bytes (planning_unit)); // Auto HWM 계산용 64-bit byte 입력이다.
```

Monitor snapshot은 Core monitoring ABI v2를 투영한다. Planned, applied, deferred와 in-flight
HWM field는 `_bytes` 접미사와 `uint64_t`를 사용한다. Deferred field의 유효 여부는 별도
boolean으로 제공한다. Pending message와 profile slot은 count 진단값으로 남으며 byte field와
이름을 공유하지 않는다.

## 기능 범위

완성된 C++ 바인딩의 공개 헤더는 다음 그룹을 다룬다.

- Core: context, version/역할 헬퍼, context 옵션, shutdown, 자동 HWM 재계산,
  `atomic_counter_t`, `stopwatch_t`, `thread_t`.
- Messaging: message ownership, 빌더 multipart 입력, received metadata, topic message,
  subscription event, routing id, callback 타입.
- Socket family: pair, dealer, router, pub, sub, xpub, xsub, stream, stream-bound
  actor snapshot, 공통 옵션, 타입 지정 socket 옵션, bind/connect/disconnect, TLS,
  callback, request/reply 표면.
- Eventing: socket monitor, monitor event, monitor snapshot, poller, one-shot `poll(...)`,
  poll event, timer,
  readiness flag.
- Services: SPOT node, SPOT handle, 토폴로지 snapshot, actor ref,
  actor 생명주기, actor operation.
- Errors: 코어 result 도메인을 보존하는 타입 지정 예외 또는 error-result 표면.

C++ 표면은 raw 네이티브 핸들, `*_part` 루프, callback userdata, 내부 inproc endpoint,
request 펌프 객체를 공개 개념으로 노출하지 않는다.

## 수명과 ownership

C++ 호출자는 C 핸들 정리를 추론하지 않아도 된다.

- 리소스 클래스는 소멸자에서 네이티브 핸들을 해제하고, close가 실패할 수 있을 때는 명시적
  `close` 또는 동등한 생명주기 메서드를 지원한다.
- mutable 핸들을 공유 소유하는 대신 move-only 리소스 클래스를 선호한다.
- 메시지 값은 효율적인 move를 지원하고, 복사를 요청할 때 명시적 copy를 지원한다.
- data-plane 수신과 subscribe 경로는 호출자가 제공하는 저장소를 쓴다.
- Actor join 요청 수신처럼 service 제어/입장 수신 경로는 C++ 호출자에게 더 명확하면
  optional이나 타입 지정 결과 반환을 써도 된다. 다만 data 없음과 강한 수신 실패는 여전히
  구분해야 한다.
- callback은 네이티브 callback 수명과 사용자 callable 수명을 내부에서 일관되게 유지한다.

## 에러와 result 정책

바인딩은 예외 또는 타입 지정 result 객체 중 무엇을 써도 되지만, 공개 형태는 코어 의미를
보존한다.

- data 없음과 일시적 backpressure는 강한 실패와 구분해 유지한다.
- request, submit, recv, bind, connect, config, handler, close 실패는 result 도메인
  의미를 유지한다.
- `pollout`은 send 복구 readiness 신호이며 일반적인 writable 비트가 아니다.
- ROUTER/PUB 기본값, SPOT HWM 기본값, SPOT dispatch worker 의미는 코어 헤더를 따른다.

## 성능 정책

- multipart 값은 코어 part substrate에서 직접 만든다.
- hot path에서 불필요한 힙 할당, 회피 가능한 복사, reflection 같은 동적 dispatch, 숨겨진
  대기, sleep, busy wait, 광범위한 lock, join을 피한다.
- perf와 샘플은 설치되는 공개 헤더만 include한다.
- perf와 샘플은 공개 C++ 바인딩 타깃에 링크한다. 비공개 런타임 object 파일이나 헬퍼 소스
  디렉터리에 링크하지 않는다.
- C++ perf 의미는 `bindings/c/perf`와 일치한다. 같은 패턴 의미, 같은 transport 의미, 같은
  클라이언트 수 정책을 따르며 비공개 fast path는 두지 않는다.

## 완성 구조 요구사항

완성된 C++ 바인딩은 다음 요구사항을 만족한다.

- 설치되는 헤더가 안정적인 사용자 대상 코어 기능을 모두 노출한다.
- C++ 바인딩은 공개 헤더 외에 컴파일된 C++ 라이브러리 타깃을 빌드하고 설치한다.
- `Contracts/Eventing/`이 유일한 공개 eventing 카테고리다. `Contracts/Monitoring/`은
  사라졌고, `zlink.hpp`는 Eventing 헤더를 include한다.
- 옛 래퍼 include 경로는 사라졌다. 애플리케이션, 샘플, perf, 테스트는 `<zlink.hpp>` 또는
  의도적인 `Contracts/...` 헤더만 include한다.
- 공개 헤더와 컴파일된 C++ 바인딩 타깃만으로 애플리케이션, perf, 샘플, framework 어댑터가
  필요한 것을 모두 갖춘다.
- 사용자는 비공개 헬퍼 헤더와 비공개 런타임 소스 경로가 필요 없다.
- 추상화가 실제 복잡도를 줄이지 않는 한 값 타입은 구체로 남는다.
- 공개 API는 네이티브 part 루프, raw 핸들, callback userdata를 숨긴다.
- handler 등록은 `set_..._handler` 이름을 쓰고, 공개 `on_...` 별칭은 두지 않는다.
- 공개 헬퍼/free function과 빌더 편의 메서드는 런타임 헬퍼가 아니라 `Contracts/`에
  선언한다.
- service 제어/입장 수신 예외는 data-plane의 호출자 제공 저장소와 다를 때 문서화한다.
- perf 테스트는 C perf와 동일한 측정 의미를 쓴다.

## Actor와 Spot 라우트 결과

C++는 Actor와 Spot 라우트 조회 결과를 구체 계약 타입으로 노출한다.

- `actor_route_t`는 해석된 Actor ref, `actor.node_rid`, `current_spot_rid`,
  `current_spot_kind`를 보존한다.
- `spot_route_t`는 `spot_rid`, `owner_node_rid`, `spot_kind`를 보존한다.
- `spot_kind`는 Entry Spot과 사용자 Spot을 구분한다. 잘못된 kind는 성공한 라우트 결과가
  아니다.
- `spot_node_spot_entry_t`와 `spot_node_actor_entry_t`는 코어 snapshot과 같은 Spot
  kind/현재 Spot 필드를 노출한다.

- C++는 resolve된 Actor ref를 인자로 받는 `spot_node_t::send_to_actor(actor_ref_t)`와 `spot_node_t::request_to_actor(actor_ref_t)`를 노출한다.
- `send_to_actor`는 submit이 성공하면 하나 이상의 message part 소유권을 넘기고, Actor 소유자 mailbox가 인계를 받으면 완료된다.
- `request_to_actor`는 submit이 성공하면 요청 part의 소유권을 넘기고, Actor handler가 만든 reply part를 callback 또는 awaitable 결과로 전달한다.
- C++는 제거된 Discovery route table이나 resolver API를 compatibility helper로 되살리면 안 된다.
