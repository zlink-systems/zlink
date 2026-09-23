[English](./framework-cpp-0.23.0.md) | [한국어](./framework-cpp-0.23.0.ko.md)

# ZLink C++ Framework 0.23.0 릴리스 노트

Framework 0.23.0은 binding 1.4.0과 Core 1.4.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- STREAM packet이 Actor를 식별할 수 있습니다. header는 `actor_slot`과 flag `0x20`을 사용하고, `$zlink.actor.bound`와 `$zlink.actor.unbound` control packet을 제공합니다. dispatch context는 bound Actor를 전달하며 connector는 `actors`, `actor(id)`, `onActorBound`/`onActorUnbound`로 Actor handle을 제공합니다. Actor 하나를 사용하는 application은 변경 없이 동작합니다. (#933)
- Logical Multicast가 routed target별 제출 실패를 target RID, topic, `stale_target`·`backpressure`·`shutdown` reason과 함께 `dispatch_error`로 기록합니다. publish terminal result는 변경되지 않습니다. (#928)
- Deferred Actor Join 전용 제한 네 가지(handler당 64개 operation, request 합계 8 MiB, request 1 MiB, reply 1 MiB)를 모두 제거했습니다. Cross-node Join request와 application reply는 service wire의 application payload 크기 규칙을 따릅니다. (#925)
- Dispatch failure가 message-flow trace와 structured log 모두에 `error_type`과 `error_message`를 기록합니다. (#929)
- Entry Spot join에 admission callback이 필요하지 않으며, 선택되지 않은 object role은 `None`입니다. Channel target 분류와 message-size diagnostics도 정렬된 Framework 기본값을 사용합니다. (#922)

## 공통 변경

- Guide의 코드 예제가 tutorial 또는 sample source에서 가져온 실행되는 snippet으로 바뀌었고, 긴 장을 더 작은 절로 나누었습니다. (#943)
- 각 예제가 참조하는 tutorial code를 명시하고, 실행 결과를 설명하며, `set_advertise_host`와 `InMesh`, STREAM client connector를 문서화했습니다. (#903, #904, #905, #920)
- 예제 README가 각 block의 실행 환경을 bash 또는 PowerShell로 표시하고, 검증을 한 곳에 모으며, 종료 절차를 설명하고, 한국어 산문을 격식체로 정리했습니다. (#890, #891)
- Java와 Kotlin example을 서로 다른 read-only mirror로 export합니다. (#894)
- engine example에 .NET server 하나와 Unity·Unreal·Godot(C#·C++)·Axmol·Cocos Creator(web) client가 포함됩니다. 모든 client는 같은 engine-lobby 계약을 따르며, 엔진별 미러 저장소(`zlink-engine-server`, `zlink-<engine>-examples`)로 제공되고, 가이드 사이트에 게임 엔진 통합 장이 있습니다. (#935, #980, #982, #983, #984, #987)
- examples mirror가 각 언어 package의 게시와 검증이 끝난 뒤 호출됩니다. (#884)
- Core 1.4.0 macOS dylib가 loader 기준 상대 경로를 사용하므로 release archive를 다른 위치로 재배치할 수 있습니다. (#962)

## C++ 변경

- 세 C++ 엔진 어댑터(Unreal plugin, Godot GDExtension, Axmol)가 `Subscribe`/`subscribe(packet_name)`으로 구독한 서버 push를 engine main thread에서 전달합니다. 이전에는 push가 앱에 전달되지 않았습니다. 요청 완료는 요청의 packet 이름을 싣습니다(Unreal은 빈 이름이었습니다). (#985)
- framework-cpp release에 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64`용 shared prebuilt archive를 포함합니다. `bootstrap.cmake`가 framework를 source에서 빌드하는 대신 해당 archive를 내려받으므로 이 경로의 consumer는 Conan이나 vcpkg가 필요하지 않습니다. (#855)
- 기본 C++ build가 cross-language host를 admission-free Entry Spot 계약에 맞춥니다. (#953)
- ShoppingMall OrderWorkflow가 relocation readiness를 이미 defer한 뒤 다시 요청하지 않으므로 sample이 강제 종료 없이 종료됩니다. (#930)

## 설치

[`framework-cpp/v0.23.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0)에 첨부된 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64` framework archive 중 환경에 맞는 것을 선택하거나 `bootstrap.cmake`를 실행해 해당 archive를 내려받습니다. `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. C++ binding 1.4.0과 Core 1.4.0이 필요합니다.

릴리스 태그는 [`framework-cpp/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0)입니다.
