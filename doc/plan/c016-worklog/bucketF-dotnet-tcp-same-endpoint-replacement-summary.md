# Bucket F — .NET TCP same-endpoint RID replacement admission

## 결론

실패는 **generation 1**에서 시작됐다. Generation 0은 local과 remote가 모두 `Admitted`까지
완료했다. 이전 intent를 제거하고 같은 TCP endpoint에 새 RID를 연결한 generation 1에서는 local이
`Connecting`에 머물렀고, remote는 local을 나타내는 peer를 만들지 못했다. Generation 2에는
도달하지 않았다.

원인은 admission 후보 선택이나 reciprocal handover barrier가 아니었다. Framework가 admitted
outbound peer를 제거하면서 Framework intent만 지우고 native endpoint reconnect intent는 남겼다.
같은 endpoint에 새 intent를 설치하자 Core가 이전 RID의 reconnect intent를 다시 사용했고, local은
새 RID로 `Hello`를 보내려 했지만 physical route는 이전 RID로 준비돼 있었다.

## 실패 trace와 정확한 경계

조사 중 기존 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`의 `mesh_peer_*` trace를 임시 file sink와
monitor probe에 연결했다. 실패 trace는
`/dev/shm/zlink-tmp-dotnet/bucketF-tcp-replacement-mesh-monitor.log`에 보존했다.

- `:1-9`: generation 0의 connect, 양쪽 `ConnectionReady`, remote의 `Hello` 수신, 양쪽
  `mesh_peer_admission_accepted`가 모두 끝났다.
- `:10-11`: local이 generation 0 peer를 제거했지만 transport 정리는
  `mode=rid physical=...remote-0...`이었다. Endpoint reconnect intent는 취소되지 않았다.
- `:21`: local이 같은 endpoint에 generation 1의 새 RID를 지정한 intent를 설치했다.
- `:22`: local의 새 `ConnectionReady`가 generation 1 RID가 아니라 제거한 generation 0 RID로
  들어왔다.
- `:24`: generation 1 remote의 inbound transport 자체는 local RID로 ready가 됐다. 그러나 그 뒤
  remote의 `mesh_peer_admission_received`가 한 건도 없고, local은 새 RID를 target으로 `Hello`만
  반복 제출했다(`:23,25-33`). 따라서 remote에는 admitted peer가 없고 local도 새 RID를
  admit할 reply를 받지 못했다.

수정 후보 trace
`/dev/shm/zlink-tmp-dotnet/bucketF-tcp-replacement-mesh-fixed-probe.log`에서는 이전 peer 제거가
`mode=endpoint`로 바뀌었다(`:10-11`). 이어 generation 1과 2가 각각 새 RID로
`ConnectionReady`를 냈고(`:22`, `:42`), remote의 `Hello` 수신과 양쪽 admission이 모두
완료됐다(`:25-29`, `:45-49`). 조사용 file sink와 `mesh_peer_monitor` probe는 최종 source에서
제거했다.

## 원인과 계약

원인 지점은
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:11846-11878`의
`DisconnectTransport`다. 수정 전 조건은 peer가 이미 admitted였으면 같은 endpoint를 쓰는
replacement intent가 없어도 `DisconnectRid`를 선택했다. `RemovePeerConnection`이
`_peersByIntent`에서 peer를 먼저 제거하므로(`:449-455`), 이 경로는 Framework 상태에서는 intent가
없어진 것처럼 보이면서 binding에는 endpoint reconnect intent가 남는 불일치를 만들었다.

다음 계약을 위반했다.

- Manual topology는 application이 한쪽 endpoint만 등록할 수 있다
  (`framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:467-469`).
  이 test의 unilateral connect도 remote의 별도 outbound intent 없이 그 한 연결에서 handshake를
  완료해야 한다.
- 재시작한 node의 새 RID와 generation을 새 연결 identity로 사용하고, 이전 generation의 늦은
  frame/event가 현재 연결을 바꾸지 못해야 한다(같은 문서 `:511-519`). Generation 1 physical
  connection에 generation 0 RID가 남은 것은 이 identity 경계를 직접 어겼다.
- Fixed-RID manual 재연결조차 application intent, 인증된 handover와 이전 pipe 종료를 확인한 뒤
  새 generation을 선택하며 duplicate 후보는 ready connection 하나만 남긴다
  (`framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:303-313`). 이번 사례는
  RID까지 새 값이므로 제거한 intent의 RID를 다음 connection이 물려받을 근거가 없다.

## 수정

`ZLinkManagedMeshNode.cs:11862-11878`에서 transport 종료 기준을 intent ownership에 맞췄다.

- 제거하는 peer가 outbound이고 같은 endpoint를 이미 사용하는 replacement intent가 없으면
  `Disconnect(endpoint)`로 native reconnect intent까지 취소한다. Peer가 과거에 admitted였는지는
  endpoint intent 보존 근거로 사용하지 않는다.
- 다른 Framework intent가 같은 endpoint를 이미 점유했으면 기존처럼 `DisconnectRid`를 사용해 새
  intent를 endpoint 단위로 끊지 않는다.
- Inbound peer는 local endpoint reconnect intent를 소유하지 않으므로 기존처럼 RID 단위로 끊는다.

Admission candidate matcher, `_readyOutboundEndpoints`, reciprocal settlement와
`RetireDuplicatePeer`의 duplicate 처리에는 손대지 않았다. 기존 test
`ServiceRuntimeFoundationTests.ManagedNode_Tcp_SameEndpoint_Replacement_RemainsAdmitted_Across_Repeated_Lifecycles`
가 세 RID generation을 반복하고 매번 local과 remote의 `Admitted`를 함께 검사한다(`:1515-1570`).
따라서 같은 동작을 고정하는 test를 추가하지 않았다.

## 검증 결과

모든 `dotnet test`는 지정된 `TMPDIR`, local Core `ZLINK_LIBRARY_PATH`, package hash 기반
`NUGET_PACKAGES`, shared compilation/node reuse 비활성화와
`flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다. 전체 suite는 실행하지 않았다.

| 범위 | 실행 | 결과 |
|---|---:|---:|
| TCP same-endpoint replacement focused | 5회 | 각 1/1, 합계 **5/5 통과** |
| `ServiceRuntimeFoundationTests` | 1회 | **59/59 통과** |
| `StatefulServiceRuntimeTests` | 1회 | **50/50 통과** |
| `ZLinkMeshPeerAdmissionTests` | 1회 | **7/7 통과** |
| `CanonicalActorJoinIngressReplyTests.RouteAdmission*` | 1회 | **2/2 통과** |

`StatefulServiceRuntimeTests` 결과에는
`BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`, 세
`RemoteActorStaleAuthority*`,
`RelocatedActorReplyCompletesTheOriginalRemoteCallerExactlyOnce`가 포함된다. Assertion과 timeout은
변경하지 않았다.

## BLOCKERS

없음.
