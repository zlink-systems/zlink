# Java ClientServer zero-weight 선택 실패 수정

## 판정

- 변경 분류: **B — 기존 결함**
- 결과: process-local ClientServer의 공개 weight가 0이면 admission과 liveness는 유지하되 새 ChannelName 호출의 선택 후보에서는 제외하며, 선택 가능한 target이 없으면 `NotFound`로 완료한다.
- Core 소스와 Framework 스펙은 변경하지 않았다.

## 원인

원인은 서로 독립적인 두 결함이 겹친 것이었다.

1. `1c5c0f9de3632fa455ac1706b8f594105e7ad3ae`에서 ClientServer target 선택이 `requestToChannel()` 호출 시점에서 `RequestCall.submit()` 시점으로 이동했지만, [ChannelMessagingTest.java](../../../framework/languages/java/zlink-framework-core/src/integrationTest/java/systems/zlink/framework/runtime/ChannelMessagingTest.java)의 zero-weight 사례는 call 객체 생성만 검사했다. 따라서 실패 경로를 실행하지 않아 “예외가 발생하지 않음”으로 실패했다.
2. Java는 공개 selection weight를 Core ROUTER의 `peerWeight`에 전달하고, descriptor와 monitoring weight도 다시 Core socket에서 읽었다. binding 0.17.3에서 공개 weight 0인 process-local server의 물리 연결이 admission 전 `PREPARING`으로 남았고, submit을 실제 실행하면 선택 제외가 아니라 “알려진 연결이 준비되지 않음”으로 분류되어 `Unavailable`이 되었다.

원인 위치는 [ZLinkChannelRuntimeConfigurator.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntimeConfigurator.java)의 ClientServer socket option 적용과 [ZLinkChannelSocketRegistry.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java)의 server descriptor·monitoring projection이었다.

## 소유권과 계약

- 소유 계층: 공개 ClientServer weight, descriptor 갱신, target 선택은 Framework channel runtime이 소유한다. Core peer weight는 물리 transport availability에만 둔다.
- [ClientServer Channel §5.1](../../../framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md)은 weight 범위를 `0..10000`으로 정하고, weight 0을 새 target 선택에서 제외한다.
- [Channel messaging §3.1, §8](../../../framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md)은 positive-weight Ready server만 후보로 삼고, 선택 가능한 target이 없으면 `NotFound`로 끝내도록 정한다.
- [Framework API §3](../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md)은 membership이 있으면 weight 0이어도 connection과 liveness를 유지하도록 정한다.
- [Java channel messaging interface](../../../framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md)는 `submit(...)`이 request 실행과 terminal reply 대기를 시작하는 경계다.

## 교차언어 대조

- .NET의 `LocalOnlyClient_DoesNotSelectZeroWeightServer`는 동일한 local dual-role 조건에서 admission 완료를 확인한 뒤 ready 후보 0과 `NotFound`를 검증한다. .NET socket option mapper는 공개 ClientServer weight를 Core socket option으로 전달하지 않고 server identity/descriptor에 유지한다.
- Node와 C++도 Framework descriptor의 `weight > 0` 조건으로 selection 후보를 정하며 zero-weight 후보를 fallback으로 선택하지 않는다.
- Java만 수정한 이유는 Java가 `requestToChannel()`의 deferred-submit 전환을 integration test에 반영하지 않았고, 동시에 Java ClientServer configurator가 공개 weight를 Core availability에 직접 결합해 이 실패를 재현했기 때문이다.

## 대안과 변경

- 기각: pending connection의 error-kind 판정에서 weight 0만 제외한다. `NotFound` assertion만 맞추고 admission/liveness 결함을 남기는 상위 계층 보상이라 C 우회에 해당한다.
- 채택: registration을 공개 weight의 단일 owner로 유지하고 descriptor·monitoring·selector가 그 값을 사용하게 했다. Core ROUTER는 transport 기본 availability weight를 유지한다. runtime weight 변경은 같은 state lane에서 registration과 descriptor revision을 갱신하고 기존 control update 경로로 전파한다.
- 회귀 테스트는 call 객체 생성이 아니라 `submit(...).toCompletableFuture().join()` terminal을 실행하고 `NotFound`를 검증한다. 단위 테스트는 공개 weight 0과 10000이 Core peer weight 100을 변경하지 않음을 고정한다.
- 규칙 수: 공개 weight를 selection·Core availability·monitoring의 세 결정에 재사용하던 3개 결합 규칙에서, Framework descriptor의 positive-weight selection 규칙 1개로 줄였다. Core availability는 별도 transport 기본 규칙을 유지한다.

## 검증

- focused unit + integration + Spring readiness: ticket `0-1788894568-63593-codex-zw-Java_zero-weight_focused_regression_incl`, 성공
- `:zlink-framework-core:integrationTest` 46개: ticket `0-1788894301-51400-codex-zw-Java_framework_core_complete_integration`, 성공
- `gradlew check`: ticket `0-1788894629-69723-codex-zw-Java_framework_final_Gradle_check_after_`, 성공
- 남은 실패: 없음
