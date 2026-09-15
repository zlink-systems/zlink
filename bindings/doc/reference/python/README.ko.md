한국어 | [English](README.en.md)

[Python 바인딩 스펙](../../spec/python/README.ko.md) · [Python 바인딩 가이드](../../guide/python/index.ko.md)

# Python bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/python/reference/`에 있음)이 아니다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. 지금까지의 모든 wrapper binding과 마찬가지로 이 트리도 6개가
아니라 5개 category다 — `contracts/`엔 `service/` 패키지가 없다. SPOT/Actor는
framework 계층에만 존재한다. 아래 Contract 원본 열은 스펙 산문이 아니라 실제 파일
목록과 대조 확인한 것이다.

아래 모든 category에 적용되는 Python 고유 참고:

- **모든 contract 타입이 `typing.Protocol`이다**(구조적 타이핑, 대부분
  `@runtime_checkable`) — concrete 기반 class가 아니다. 실제 구현은
  `_runtime`/`_native` 아래에 있으며 직접 import되지 않는다. caller는 항상
  `Protocol` 형태만 본다.
- **factory 함수가 package 최상위에 있다**(`bindings/python/src/zlink/__init__.py`:
  `create_context`, `create_pair_socket`, `version`, `has`, `proxy`, `sleep`, ...),
  node/rust와 같은 package-root 관용구다, `contracts/core/`가 아니다.
- **native storage를 소유하는 모든 resource 타입이 sync·async context-manager
  프로토콜을 둘 다 지원한다**(`__enter__`/`__exit__` *그리고*
  `__aenter__`/`__aexit__`) — `with`/`async with` 둘 다 동작한다, 지금까지 다룬
  다른 모든 언어 중 유일하다.
- **이 binding의 어떤 socket type도 `set_routing_id`/`get_routing_id`/`routing_id`
  property를 선언하지 않는다** — `DealerSocket`도, `RouterSocket`도,
  `StreamSocket`도 아니다. `RouterSocketOptions.connect_routing_id`만 존재하며,
  이건 socket 자신의 identity가 아니라 *다음 outgoing connection*의 routing id를
  할당한다. 지금까지 다룬 다른 모든 언어는 최소한 Dealer/Router/Stream에
  routing-id setter/getter를 노출한다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`contracts/` + `__init__.py` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `__init__.py`(factory/자유 함수); `contracts/core/`: `context.py`, `options.py`, `routing_id.py`, `utilities.py`, `codes.py` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging/`: `message.py`, `received.py`, `topic_message.py`, `subscription_event.py`(별도 `operations.py`가 없다 — builder Protocol은 `contracts/sockets/operations.py`에 있다) |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets/`: `socket.py`, `message_socket_contracts.py`, `routed_socket_contracts.py`, `pubsub_socket_contracts.py`, `stream_socket.py`, `socket_options.py`, `operations.py`, `codes.py` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing/`: `poller.py`, `monitor.py`, `timer.py`, `codes.py` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors/`: `errors.py`, `results.py`, `codes.py` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
