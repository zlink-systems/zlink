# Java RouteMesh inbound identity 재연결 CI 간헐 실패

## 판정

- 변경 분류: **B — 기존 결함**. Framework runtime 결함이 아니라 integration test가 재연결의
  선행 조건을 생략한 결함이다.
- 결과: 연결을 시작하지 않은 listener가 이전 연결의 `DISCONNECTED`를 관찰한 뒤 같은 endpoint에
  다시 연결하고, 새 inbound probe identity로 reverse request를 보낸다.
- Core 소스와 Framework runtime·스펙은 변경하지 않았다. 테스트의 timeout도 늘리지 않았다.

## CI 증거와 재현

대상 테스트 source blob은 아래 네 실행에서 모두
`5417c89471b000ccb7eeb6b3c751212535b83fae`로 같았다.

| 실행 | 대상 integration test | 이후 결과 |
|---|---|---|
| `34267904382`, attempt 1 | `ZlinkSubmitException`, 원본 107행 | Java job 실패 |
| `34267904382`, attempt 2 | 통과 | 별도 Kotlin test 실패 |
| `34268994937` | 통과 | 별도 stream connector test 실패 |
| `34269950430` | `ZlinkSubmitException`, 원본 107행 | Java job 실패 |

따라서 CI 빈도는 4회 중 2회 실패였다. Workflow console은 예외 class와 source line만 남기고
`ZlinkSubmitException`의 result·errno를 출력하지 않으며 test report도 artifact로 올리지 않는다.
같은 테스트에 임시 진단만 추가해 4개 CPU에 Gradle과 부하를 고정한 50회 실행에서는 3회 실패했고,
세 번 모두 두 번째 reverse request의 `NOT_CONNECTED`, errno 113(`EHOSTUNREACH`)이었다. 임시 반복과
진단 코드는 최종 diff에서 제거했다.

## 원인

[ZLinkRouteMeshInboundIdentityIntegrationTest.java](../../../framework/languages/java/zlink-framework-core/src/integrationTest/java/systems/zlink/framework/runtime/channels/ZLinkRouteMeshInboundIdentityIntegrationTest.java)는
`initiator.disconnect(endpoint)` 반환 직후 같은 endpoint에 `connect`하고, inbound probe 하나를 받은 즉시
listener에서 두 번째 request를 제출했다. 그러나 [Socket 공통 § `zlink_disconnect`](../../../core/doc/spec/core/socket/README.ko.md#zlink_disconnect)는
성공한 disconnect가 local 등록과 reconnect intent를 제거할 뿐 physical 자원 정리는 비동기이며, 뒤이은
connect와 겹칠 수 있다고 정한다. 이 경합에서 listener의 이전 routing map 정리가 끝나기 전에 재연결을
진행했고, 두 번째 request 시점에는 target RID route가 없었다.

[ROUTER §7](../../../core/doc/spec/core/socket/07-router.ko.md#7-raw-request-submit)은 `DONTWAIT` request의
routing map에 RID가 없으면 `NOT_CONNECTED`+`EHOSTUNREACH`이고 wait token이 없다고 정한다. Java
binding의 `CompletionOwner.submitRequest`는 request를 `DONT_WAIT`로 제출하며, writable wait가 아닌
결과는 그대로 `ZlinkSubmitException`으로 전달한다. CI의 원본 107행과 진단 재현의
`NOT_CONNECTED`+113이 이 경로와 일치한다.

## 소유권과 계약

- 소유 계층: physical close와 route map은 Core가 소유한다. Framework는 Core 상태를 추측하지 않고
  monitor event의 관찰 순서로 connection lifecycle을 판정한다.
- [Transport liveness §5](../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md)는
  disconnect 호출 성공만으로 physical close 완료를 판정하지 않고, close snapshot 또는 disconnect
  event를 관찰한 뒤 같은 endpoint의 새 connection을 만들도록 정한다.
- [MeshNode §7.1](../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md#71-peer-연결)은
  fixed RID 재연결에서 이전 pipe 종료를 확인한 뒤 새 connection을 target selection에 포함하도록 정한다.
- [Core monitoring §3·§9](../../../core/doc/spec/core/06-monitoring.ko.md)는 monitor가 상태 전이 commit 순서로
  event를 기록하고, 성립한 inproc 연결의 종료를 `DISCONNECTED`로 보고하도록 정한다.

## 교차언어 대조와 변경

- Java production `ZLinkJavaRawMeshNode`는 endpoint disconnect 뒤 monitor의 terminal pair 관찰만이
  replacement를 열 수 있게 하고, close를 관찰하기 전 `replacePeerConnection`을 거부한다.
- .NET `ZLinkManagedMeshNode`와 C++ `raw_mesh_node_owner`도 `CONNECTION_READY`와 `DISCONNECTED` monitor를
  열어 connection candidate 상태를 갱신한다.
- Java만 수정한 이유는 production runtime 차이가 아니라 이 Java raw integration test만 2026-08-24에
  서로 다른 context 두 번 검증하던 구조에서 같은 socket의 disconnect/reconnect 구조로 바뀌면서 close
  관찰을 추가하지 않았기 때문이다.
- 기각: request 재시도, sleep 또는 timeout 증가. Route가 없는 `NOT_CONNECTED`는 wait 가능한
  backpressure가 아니며 경합을 숨기는 우회다.
- 채택: listener socket에 기존 public `SocketMonitor`를 열고, 기존 5초 bounded condition 안에서
  `DISCONNECTED`를 받은 뒤 reconnect한다. Request와 probe assertion은 그대로 유지한다.
- 규칙 수: “disconnect 반환이면 close 완료”와 “probe 수신이면 이전 route 정리 완료”라는 암묵적
  가정 2개를 “listener의 `DISCONNECTED` 관찰 뒤 reconnect”라는 계약 규칙 1개로 줄였다.

## 검증

- 사전 비고정 탐색 5회는 모두 통과했으나 반복 Gradle 시작 비용이 커 중단했다. ticket
  `0-1788897147-60435-codex-rm-Java_RouteMesh_inbound_identity_CI_flake`, `rc=137`.
- 수정 전 4 CPU 고정 부하 50회: 47 통과, 3 실패(`NOT_CONNECTED`+113). ticket
  `0-1788897607-2311-codex-rm-Java_RouteMesh_inbound_identity_pre-fix_`, `rc=1`.
- 수정 후 같은 조건 20회: 20 통과, 실패·skip 0. ticket
  `0-1788898379-42549-codex-rm-Java_RouteMesh_inbound_identity_post-fix`, `rc=0`.
- `:zlink-framework-core:integrationTest` 46개와 `check`: 모두 통과. ticket
  `0-1788898916-48718-codex-rm-Java_RouteMesh_final_full_integrationTes`, `rc=0`.
