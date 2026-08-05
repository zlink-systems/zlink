한국어 | [English](README.en.md)

[C++ 바인딩 스펙](../../spec/cpp/README.ko.md) · [C++ 바인딩 가이드](../../guide/cpp/index.ko.md)

# C++ bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의 레퍼런스
트리가 `framework/doc/framework/cpp/reference/`에 있음)이 아니다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다([C++ 바인딩 스펙](../../spec/cpp/README.ko.md)이 이를 C++ naming으로
투영한 것이지, C# 형태를 복사한 게 아니다). dotnet 레퍼런스 트리와 마찬가지로 이 트리도
6개가 아니라 5개 category다 — `include/zlink/Contracts/`엔 `Service/` 폴더가 없다.
SPOT/Actor는 framework 계층에만 존재한다. dotnet과 마찬가지로 `Contracts/`의 산문은 이
디렉터리 목록에 없는 파일을 나열하고 있어, 아래 Contract 원본 열은 스펙 산문을 베낀 게
아니라 `find`로 대조 확인한 것이다.

아래 모든 category에 적용되는 C++ 고유 배치 참고 두 가지:

- **헤더 트리가 두 개 존재한다.** `include/zlink/{core,eventing,message,socket}/api.h`는
  `extern "C"` 선언이다 — bindings 구현자를 위한 저수준 part-substrate 계층이지 public C++
  contract가 아니다. public contract는 `include/zlink/Contracts/<Category>/*.hpp`이며, 단일
  진입 헤더 `<zlink.hpp>`가 이를 집계한다.
- **`proxy`/`proxy_steerable`는 Core가 아니라 Sockets category에 있다** — `socket_t&`를
  받는 자유 함수다(`Contracts/Sockets/socket_contracts.hpp`), dotnet의 `Zlink` static facade
  배치와 다르다. dotnet의 `Zlink.Sleep(...)`/`Zlink.MultipartClose(...)` 같은 자유 함수 편의
  helper에 대응하는 C++ 대응물은 오늘 시점엔 없다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를 먼저,
`.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`include/zlink/Contracts/` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `Contracts/Core/`: `context.hpp`, `context_options.hpp`, `routing_id.hpp`, `byte_count.hpp`, `capability.hpp`, `utilities.hpp` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `Contracts/Messaging/`: `message.hpp`, `received.hpp`, `topic_message.hpp`, `subscription_event.hpp`, `operation_contracts.hpp`, `request_result.hpp`(`lazy_message_parts.hpp`와 `operation_builder_base.hpp`는 `detail`이라 public 항목 없음) |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `Contracts/Sockets/`: `socket_contracts.hpp`, `message_socket_contracts.hpp`, `routed_socket_contracts.hpp`, `pubsub_socket_contracts.hpp`, `stream_socket.hpp`, `socket_options.hpp`, `results.hpp` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `Contracts/Eventing/`: `monitor.hpp`, `status.hpp`, `poller.hpp`, `poll_event.hpp`, `timers.hpp`, `events.hpp` |
| [Errors](05-errors.ko.md) | 작성 완료 | `Contracts/Errors/`: `errors.hpp`, `results.hpp` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
