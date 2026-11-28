한국어 | [English](README.en.md)

[Python 바인딩 스펙](../../spec/python/README.ko.md) · [Python 바인딩 가이드](../../guide/python/index.ko.md)

# Python bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/python/reference/`에 있음)이 아니다.

Category는 [.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. 지금까지의 모든 wrapper binding과 마찬가지로 이 트리도 6개가
아니라 5개 category다 — `contracts/`엔 `service/` 패키지가 없다. SPOT/Actor는
framework 계층에만 존재한다. 아래 Contract 원본 열은 스펙 산문이 아니라 실제 파일
목록과 대조 확인한 것이다.

아래 모든 category에 적용되는 Python 고유 참고:

- 소켓과 option facade는 `typing.Protocol`이다. 이벤트 스냅샷과 enum은
  구체 타입이다.
- `create_context`와 `create_pair_socket` 같은 팩토리는 패키지 루트에서
  가져온다.
- 소켓은 `with`를 지원한다. `Context`와 `Message`는 `with`와
  `async with`를 모두 지원한다.
- DEALER·ROUTER·STREAM은 `set_routing_id`와 `get_routing_id`로 소켓의
  routing ID를 설정하고 조회한다. `RouterSocketOptions.connect_routing_id`는
  다음 outbound 연결의 peer routing ID를 지정한다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 사용하고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`contracts/` + `__init__.py` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `__init__.py`(factory/자유 함수); `contracts/core/`: `context.py`, `options.py`, `routing_id.py`, `utilities.py`, `codes.py` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging/`: `message.py`, `received.py`, `topic_message.py`, `subscription_event.py`(별도 `operations.py`가 없다 — builder Protocol은 `contracts/sockets/operations.py`에 있다) |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets/`: `socket.py`, `message_socket_contracts.py`, `routed_socket_contracts.py`, `pubsub_socket_contracts.py`, `stream_socket.py`, `socket_options.py`, `operations.py`, `codes.py` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing/`: `poller.py`, `monitor.py`, `timer.py`, `codes.py` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors/`: `errors.py`, `results.py`, `codes.py` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
