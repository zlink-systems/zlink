# Java mesh transport 종료 귀속 수정 결과

Java raw mesh intent는 자신이 기록한 READY와 같은 nonzero connection ID·lane의 종료만
반영한다. 다른 attempt/lane의 DISCONNECTED가 transport set을 비우는 결함을 수정했다.
회귀 테스트 4개는 통과했으며, 최종 M6A class 3회에는 Core 의존 실패가 남아 있다.

## 원인과 변경 파일

- [ZLinkJavaRawMeshNode.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java)
  — 수정 전 `TransportIdentity.matches:7256-7263`는 ID·lane이 달라도 endpoint key가 같으면
  true를 반환했다. `markPeerIntentsClosed:6774-6788`의 removeIf가 이 transport들을 지우고
  set이 비면 intent를 닫았다. 기존 진단의 READY 1436/lane 0 → DISCONNECTED 1439/lane 1
  순서가 직접 재현 입력이다.
- 현재 같은 파일 `:7240-7256`은 endpoint 필드를 제거하고 기록된 ID·lane만 대조한다.
  `:6770-6785`는 기록에서 실제로 제거한 transport가 있고 남은 기록이 없을 때만 닫는다.
  READY 기록 없이 endpoint/RID로 close 요청을 완료하던 `closingUntrackedTransport`
  예외도 제거했다.
- 현재 `:6744-6751`은 close 요청 뒤 도착한 READY도 기존 transport set에 기록한다.
  닫는 중인 intent를 live로 다시 표시하지 않는다. 이 배치로 READY 전 close 요청도
  같은 기록·종료 규칙을 사용한다.
- [ZLinkJavaRawMeshNodeTransportIdentityTest.java](../../../framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeTransportIdentityTest.java)
  — 기존 monitor 처리 메서드를 동기 호출하여 event 순서를 고정하는 단위 회귀 테스트다.
  M6A 원본 테스트와 assertion은 변경하지 않았다.
- 이 결과 문서. Core·binding·다른 언어·보호된 문서와 다른 Java 작업 파일은 수정하지 않았다.
  Branch는 `main`이며 commit하지 않았다.

대안은 기존 READY 기록으로 종료를 귀속하는 방식과 별도 pair/generation 상태를 추가하는
방식이었다. 전자를 적용했다. Monitor ID는 이 node가 이미 처리한 READY와 종료를
correlation하는 값이며, physical pipe 선택·frame 필터·send/reply target·reconnect 정책으로
확장하지 않는다. Endpoint cleanup과 Core의 REJECT/HANDOVER 정책도 바꾸지 않았다.

**수정 전/후 규칙 수:** transport 귀속 2 → 1
(ID·lane 일치 또는 endpoint-key 일치 → 기록된 transport 일치).
미기록 transport의 close 예외 1 → 0. READY 기록 소유자는 기존
`peerIntentTransports` 하나이며 새 상태·타이머·매핑은 없다.

## 소유권·계약·교차언어·분류

- **소유 계층:** Java Framework raw mesh가 intent의 READY 관찰과 종료 귀속을 소유한다. Core가 physical pipe 선택·교체·재연결을 소유한다.
- **Spec 조항:** [transport-liveness §5](../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md) `:239-249` 및 영어 미러 `:262-275`, D-086 2(b); [mesh-node §7.1](../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md) `:286-313`.
- **교차언어 대조:** .NET과 Node도 기록된 connection에 해당하는 종료인지 확인한 뒤 peer 상태를 변경한다. Java의 endpoint fallback과 close 요청 중 READY 기록 누락이 구조적 차이였다. 아래에 실제 대조 범위와 차이를 적었다.
- **변경 분류:** **B — 기존 결함 수정.** 감독의 이번 fix 요청을 구현 승인으로 적용했다. 계약·timeout·기대 assertion은 변경하지 않았다.

.NET `ZLinkMeshPeerAdmission.cs:233-283`은 READY로 기록한 candidate의 connection ID와
remote endpoint를 대조하여 제거하고, 남은 candidate 여부를 반환한다.
`ZLinkManagedMeshNode.cs:8550-8600`은 rising READY를 기록하고, 해당 candidate의 종료이며
남은 candidate가 없을 때만 논리 종료 처리를 전달한다. 이 monitor 처리에서는 physical
direction으로 event를 다시 거르지 않는다.

Node `raw-service-mesh-runtime.ts:1041-1059`는 기록된 peer connection ID와
DISCONNECTED identity가 같을 때 `removePeer`를 호출한다. `:1544-1554`의 통지를 받은
`node-raw-mesh-backend.ts:391-400`이 endpoint·RID에 해당하는 intent를 제거한다.
즉 backend의 endpoint 제거 전에 runtime 소유자가 connection 귀속을 확인한다.
`monitorConnectionId:1631-1650`에는 ID가 없는 event에 대한 endpoint fallback이 남아 있으나,
nonzero ID가 다른 event를 endpoint 일치만으로 같게 취급하지 않는다.
Java는 기존에 lane도 저장하므로 그 기록의 ID와 lane을 함께 확인한다.
.NET/Node의 candidate 구성과 Java intent의 자료구조가 같다는 의미는 아니다.

## 검증 환경과 결과

- 실행 위치: `framework/languages/java`.
- 환경: `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`,
  `ZLINK_JAVA_STREAM_TRACE=1`, `flock -w7200 /tmp/zlink-jvm-gate.lock`.
- 기존 진단의 stream trace를 먼저 읽었으며 임시 runtime 로그는 추가하지 않았다.
  매 실행 로그와 XML 복사를 같은 lock 안에서 수행했다.
- Maven `systems.zlink:zlink:0.17.0` JAR의 `native/linux-x86_64/libzlink.so` SHA-256:
  `f20f5cdba0bc117b17db7f8e9fec25b47d79de29bdfbff1e88ceaa8a032d2640`.
  이전 rebuild8 진단과 같다. 이 작업은 Core/package를 다시 만들지 않았다.
- 증거 위치: `/tmp/zlink-java-mesh-transport-identity/`.

| 검증 | 결과 | 증거 |
|---|---|---|
| 새 단위 회귀 class + READY 전 교체 focused test | 5/5 PASS | `unit-final.log`, `unit-final/*.xml` |
| 최종 M6A class 1회 | 26 PASS / 2 FAIL | `final-class-1.log/xml` |
| 최종 M6A class 2회 | 27 PASS / 1 FAIL | `final-class-2.log/xml` |
| 최종 M6A class 3회 | 26 PASS / 2 FAIL | `final-class-3.log/xml` |
| `:zlink-framework-core:test contractTest --continue` 1회 | Core test: Gradle 집계 1,244개 중 2 FAIL; contract 96/96 PASS; exit 1 | `gate.log`, `gate.exit`, `gate/` |

최종 gate의 Core XML leaf testcase 집계는 1,231개이며 실패는 같은 2개다.
위 1,244는 Gradle 콘솔·HTML의 집계를 그대로 사용했다.
Contract는 core 27, kotlin 17, provider-abstractions 4, testkit 48개로 모두 통과했다.
새 단위 회귀 4개도 전체 gate에서 모두 통과했다.

단위 회귀는 다음 동작을 검증한다.

| Test | 결과 |
|---|---|
| `foreignTerminationPreservesIntentUntilItsRecordedTransportDisconnects` — 다른 ID·lane, 각각의 단독 불일치와 ID 0은 보존; 자기 종료는 닫음 | PASS |
| `intentClosesOnlyAfterEveryRecordedTransportTerminates` — 하나의 종료·중복 종료는 다른 기록 보존; 마지막 CLOSED는 닫음 | PASS |
| `closeRequestDoesNotAttributeAnUnrecordedTerminationToIntent` — 기록 없는 종료로 close 요청을 완료하지 않음 | PASS |
| `readyObservedAfterCloseRequestStillOwnsItsTermination` — close 요청 뒤 READY 기록, foreign 종료 보존, 자기 종료 완료 | PASS |

### M6A 테스트별 결과

최종 코드에 대해 각 회차에서 test task에 `--rerun`을 지정했다.

| Test | 1회 | 2회 | 3회 |
|---|---|---|---|
| `bilateralManualConnectKeepsOneReadyPeer` | PASS | PASS | PASS |
| `boundActorRequestDecodesOneFrameTerminalError` | PASS | PASS | PASS |
| `canonicalRelocationControlUsesRawInfrastructureLane` | PASS | PASS | PASS |
| `canonicalRelocationPrepareUsesItsRequestReplyLeg` | PASS | PASS | PASS |
| `closedExpectedPeerClassifiesDirectSendAsRouteNotConnected` | PASS | PASS | PASS |
| `command42ReceivesCommand43ThroughInfrastructureDispatcher` | PASS | PASS | PASS |
| `command44UsesOneWayInfrastructureDispatch` | PASS | PASS | PASS |
| `connectionIdForAdmissionReusesCoreSelectedRouteAcrossCommands` | PASS | PASS | PASS |
| `descriptorBackedObjectClientIsNotRequiredAndNodeDirectIsNotFound` | PASS | PASS | PASS |
| `descriptorBackedPeerIntentRequiresLifecycleAndSecurityFence` | PASS | PASS | PASS |
| `descriptorFenceReplacesEndpointOnlyIntent` | FAIL | PASS | FAIL |
| `disconnectedChannelRemainsUnavailableUntilAutoTargetIsRemoved` | PASS | PASS | PASS |
| `ephemeralBindPublishesTheActualListenerEndpoint` | PASS | PASS | PASS |
| `expectedRouteMismatchDiagnosticNamesEveryDifferentField` | PASS | PASS | PASS |
| `inboundHelloFromStoreExpectedPeerIsAdmittedWithoutLocalDial` | PASS | PASS | PASS |
| `infrastructureControlProgressesWhileApplicationDispatchIsBlocked` | PASS | PASS | PASS |
| `infrastructureControlRejectsApplicationAndUnboundedMultipart` | PASS | PASS | PASS |
| `manualObjectClientPairIsNotRequiredButWeightZeroServerMembershipConnects` | PASS | PASS | PASS |
| `messageFollowIsDeliveredAsInfrastructureWithoutApplicationDispatch` | PASS | PASS | PASS |
| `monitorConnectionKeyIgnoresEventSpecificValue` | PASS | PASS | PASS |
| `nodeRequestCompletesExactlyOnceThroughFrameworkReply` | PASS | PASS | PASS |
| `nodeSendUsesOnlyRawPublicBindingAndDispatchesOwnedParts` | PASS | PASS | PASS |
| `observedInprocCloseDoesNotFenceDescriptorReplacement` | FAIL | FAIL | FAIL |
| `oneWayAdapterClassifiesNativeSubmitRejection` | PASS | PASS | PASS |
| `oneWayAdapterDistinguishesRouteLossFromAdmissionTimeout` | PASS | PASS | PASS |
| `relocationControlUsesAdmittedPeerAndBypassesApplicationMailbox` | PASS | PASS | PASS |
| `replacementDoesNotSkipAConnectionBeforeItsReadyEvent` | PASS | PASS | PASS |
| `sourceWideAdmissionReadySelectsOnlyReadyPeers` | PASS | PASS | PASS |

### 남은 실패와 해석

- `descriptorFenceReplacesEndpointOnlyIntent`: 1·3회와 전체 gate에서
  `ZLinkJavaRawMeshNodeM6ATest.java:601`의 마지막 `assertThrows(IllegalStateException)`가
  “nothing was thrown”으로 실패했다. 첫 replace assertion `:574`는 통과했다.
  2회에는 전체 테스트가 통과했다.
- `observedInprocCloseDoesNotFenceDescriptorReplacement`: 3회와 전체 gate 모두
  `:649 → awaitState:1400`에서 replacement의 ADMITTED를 관찰하지 못했다.
- 앞선 [진단 기록](./fix-java-descriptor-fence-class-run-summary.md)의 공개 C API repro는
  rebuild8 Core가 같은 local endpoint의 새 attempt를 처리하면서 기존 admitted pipe까지
  종료함을 확인했다. 이번에도 같은 packaged Core를 사용했고 같은 assertion이 남았다.
  Java의 foreign 종료 귀속 회귀는 통과했으므로, 남은 실패는 해당 Core 결함과 일치한다.
  이번 실행에 새 native monitor 진단을 넣어 각 실패의 pipe 종료 순서를 다시 입증한 것은 아니다.

개발 중 최초 class 실행(`class-1.log/xml`)은 25 PASS / 3 FAIL이었다.
당시 미기록 close 예외를 제거한 뒤 close 요청 중 READY를 여전히 건너뛰어
`descriptorBackedPeerIntentRequiresLifecycleAndSecurityFence:485`와
`replacementDoesNotSkipAConnectionBeforeItsReadyEvent:690`가 닫힘 대기에서 실패했다.
READY 기록 배치를 수정한 최종 코드에서는 두 테스트 모두 class 3회와 전체 gate에 통과했다.
최초 실행은 최종 코드 3회 결과에 포함하지 않았다.

## BLOCKERS

- **전체 green gate 미달:** 위 두 M6A 사례의 실패가 남아 있다.
- **Core D-094 패키지 반영 대기:** `same_local_endpoint_reconnect` admission bypass 제거는
  별도 Core 작업이다. 반영된 Java binding package로 두 사례와 M6A class를 다시 검증해야 한다.
  현재 package로 D-094 수정 후 성공까지 검증했다고 주장하지 않는다.
- Java intent 종료 귀속 패치의 단위 회귀와 contract에는 남은 실패가 없다.
