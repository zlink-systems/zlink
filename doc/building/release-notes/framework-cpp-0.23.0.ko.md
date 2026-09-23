[English](./framework-cpp-0.23.0.md) | [한국어](./framework-cpp-0.23.0.ko.md)

# ZLink C++ Framework 0.23.0 릴리스 노트

Framework 0.23.0은 binding 1.4.0과 Core 1.4.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Stream Connector에 요청 hook 두 개를 추가했습니다. request sending hook은 요청을 호출한 흐름에서 frame을 만들기 전에 동기로 실행되어 공통 metadata를 더할 수 있고, reply received hook은 응답·실패·timeout·연결 종료로 요청이 끝날 때 dispatch mode에 따라 실행됩니다. Actor handle로 보낸 요청에도 적용됩니다. (#1009)
- Stream Connector가 flow를 만들거나 보내거나 노출하지 않습니다. connector의 진단 수준 옵션(`DiagnosticsLevel`·`setDiagnosticsLevel` 등)과 message의 flow 속성, `flowFrom`을 제거했습니다. 흐름은 서버가 STREAM ingress에서 시작합니다. (#1010)
- Stream Connector의 수신 등록·send·request는 connector와 Actor handle에서, 대기 표면은 connector에서 packet 이름을 명시하는 형태와 payload 타입에서 정하는 형태를 모두 제공합니다. Actor handle의 대기 표면은 두지 않습니다. (#1012)
- 현재 binding이 아닌 Actor slot을 실은 packet(unbind 뒤 늦게 도착한 packet)은 session handler에 전달하지 않습니다. Request는 같은 sequence의 `Error`(`InvalidOperation`)로 끝나고 Send는 버려집니다. STREAM `Error` payload의 `code`는 네 언어 모두 오류 종류 이름을 snake_case로 쓴 값(`invalid_operation`, `unavailable` 등)입니다. (#1025)
- 서버 message-flow 기록이 STREAM session의 수신·분배·reply·push에 `stream_session_id`(session routing ID의 소문자 hex, 구조화 log key `session`)를 싣습니다. (#1011)
- Spot 구독 handler에 topic이 없거나 비면 등록 경로(명시 등록, 패키지 스캔)와 무관하게 host가 시작되지 않고, 오류가 handler 이름을 알려 줍니다. 이전에는 패키지 스캔이 그런 handler를 조용히 건너뛰었습니다. (#1004)
- STREAM packet이 Actor를 식별할 수 있습니다. header는 `actor_slot`과 flag `0x20`을 사용하고, `$zlink.actor.bound`와 `$zlink.actor.unbound` control packet을 제공합니다. dispatch context는 bound Actor를 전달하며 connector는 `actors`, `actor(id)`, `onActorBound`/`onActorUnbound`로 Actor handle을 제공합니다. Actor 하나를 사용하는 application은 변경 없이 동작합니다. (#933)
- Logical Multicast가 routed target별 제출 실패를 target RID, topic, `stale_target`·`backpressure`·`shutdown` reason과 함께 `dispatch_error`로 기록합니다. publish terminal result는 변경되지 않습니다. (#928)
- Deferred Actor Join 전용 제한 네 가지(handler당 64개 operation, request 합계 8 MiB, request 1 MiB, reply 1 MiB)를 모두 제거했습니다. Cross-node Join request와 application reply는 service wire의 application payload 크기 규칙을 따릅니다. (#925)
- Dispatch failure가 message-flow trace와 structured log 모두에 `error_type`과 `error_message`를 기록합니다. (#929)
- Entry Spot join에 admission callback이 필요하지 않으며, 선택되지 않은 object role은 `None`입니다. Channel target 분류와 message-size diagnostics도 정렬된 Framework 기본값을 사용합니다. (#922)

## 공통 변경

- SupportChat sample이 상담원의 방별 conversation Actor를 metadata `ConversationId` 대신 Actor slot으로 구분합니다. `JoinConversationReq`는 `conversationId`를, `JoinConversationRes`는 `actorId`를 싣고, 상담원은 방마다 Actor handle로 보냅니다. (#1016)
- Tutorial이 한 연결에 Actor 하나일 때(connector 수준 송수신)와 여럿일 때(Actor handle, Actor ID)를 다섯 언어에서 함께 보여 줍니다. (#1013)
- 가이드: STREAM 동작 원리 장을 다시 쓰고, 수신 가이드에 여러 Actor 구분을, connector 가이드에 요청 hook과 이름 두 형태를 더했습니다. 서버 가이드 17장(ZLink를 어디에 사용하나)을 제거했습니다. (#1015)
- Guide의 코드 예제가 tutorial 또는 sample source에서 가져온 실행되는 snippet으로 바뀌었습니다. (#943)
- 각 예제가 참조하는 tutorial code를 명시하고, 실행 결과를 설명하며, `set_advertise_host`와 `InMesh`, STREAM client connector를 문서화했습니다. (#903, #904, #905, #920)
- 예제 README가 각 block의 실행 환경을 bash 또는 PowerShell로 표시하고, 검증을 한 곳에 모으며, 종료 절차를 설명하고, 한국어 산문을 격식체로 정리했습니다. (#890, #891)
- Java와 Kotlin example을 서로 다른 read-only mirror로 export합니다. (#894)
- engine example에 .NET server 하나와 Unity·Unreal·Godot(C#·C++)·Axmol·Cocos Creator(web) client가 포함됩니다. 모든 client는 같은 engine-lobby 계약을 따르며, 엔진별 미러 저장소(`zlink-engine-server`, `zlink-<engine>-examples`)로 제공되고, 가이드 사이트에 게임 엔진 통합 장이 있습니다. (#935, #980, #982, #983, #984, #987)
- examples mirror가 각 언어 package의 게시와 검증이 끝난 뒤 호출됩니다. (#884)
- Core 1.4.0 macOS dylib가 loader 기준 상대 경로를 사용하므로 release archive를 다른 위치로 재배치할 수 있습니다. (#962)

## C++ 변경

- 세 C++ 엔진 어댑터(Unreal, Godot, Axmol)가 다른 connector와 같은 모양으로 push를 `on(packet_name, callback)`(Unreal `On(PacketName, Delegate)`)으로 받고, 요청은 호출할 때 넘긴 완료 callback으로 결과를 받습니다. 요청 hook을 제공하며, callback·hook 예외는 엔진 오류 로그에 기록하고 결과를 바꾸지 않습니다. (#1007, #1009)
- C++ connector: close 시 unbound callback이 실행되지 않던 결함, 연결 종료·재연결 때 대기 중이던 요청 결과가 지워지던 결함, `wait_for_sequence`가 연속 push를 놓치던 결함을 고쳤습니다. (#1009)
- 스펙에 선언된 `session_actor_manager_t::bound()`를 공개 API에 추가했습니다. (#1013)
- framework-cpp release에 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64`용 shared prebuilt archive를 포함합니다. `bootstrap.cmake`가 framework를 source에서 빌드하는 대신 해당 archive를 내려받으므로 이 경로의 consumer는 Conan이나 vcpkg가 필요하지 않습니다. (#855)
- 기본 C++ build가 cross-language host를 admission-free Entry Spot 계약에 맞춥니다. (#953)
- ShoppingMall OrderWorkflow가 relocation readiness를 이미 defer한 뒤 다시 요청하지 않으므로 sample이 강제 종료 없이 종료됩니다. (#930)

## 설치

[`framework-cpp/v0.23.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0)에 첨부된 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64` framework archive 중 환경에 맞는 것을 선택하거나 `bootstrap.cmake`를 실행해 해당 archive를 내려받습니다. `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. C++ binding 1.4.0과 Core 1.4.0이 필요합니다.

릴리스 태그는 [`framework-cpp/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0)입니다.
