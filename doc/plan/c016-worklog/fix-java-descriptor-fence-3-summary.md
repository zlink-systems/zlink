# Java M6A descriptor fence(3) — intent 종료 소유자를 physical attempt에서 connection으로 옮김

`descriptorFenceReplacesEndpointOnlyIntent`(:601)와 `observedInprocCloseDoesNotFenceDescriptorReplacement`(:649)의
class-run 실패는 같은 Java 결함 하나다. Java raw mesh node가 **intent의 종료를 "기록한 physical attempt의 종료"로
판정**했고, inproc에서는 종료 뒤 Core connect intent를 회수하지 않았다. Core(D-092/D-094 포함, 설치본
`2055a581…`)는 공개 계약대로 동작했다. 수정은 `ZLinkJavaRawMeshNode.java`와 그 단위 회귀 하나이며, 커밋하지 않았다.

## 재현과 관찰 순서

환경: `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`,
`ZLINK_JAVA_STREAM_TRACE=1`, `flock -w7200 /tmp/zlink-jvm-gate.lock`. 설치 Core/패키지 native
SHA-256 `2055a5819059c91be6afc8c50073f22001bb59598ecf7424045918306ef9f9a0`(Maven jar 내부 `.so`와
`.artifacts/wsl/install/.../libzlink.so.0` 동일). 증거: `/tmp/zlink-java-descriptor-fence-3/`.

기존 stream trace는 monitor 경계를 찍지 않아 두 앞선 job이 매번 임시 로그를 넣었다. 이번에는 §4.1 규칙대로
monitor event·intent connect/disconnect/closed 전이를 **gated stream trace로 정식 승격**했다
(`ZLinkJavaRawMeshNode.java:902, :994, :6676, :6809` — `STREAM_TRACE ? … : null`, off일 때 무비용).

수정 전 class run 3회: 1회 observed-close 1 FAIL; 2·3회 두 테스트 2 FAIL(`class-{1,2,3}.log/xml/stderr`).

### `descriptorFenceReplacesEndpointOnlyIntent` (run 3, local 측 monitor)

```text
peer-intent-connect intent=1 (endpoint-only)      READY edge 4033784/lane0
peer-intent-disconnect intent=1  ← 첫 replace가 close 요청, 예외 정상(:574)
DISCONNECTED 4033784/lane0 → peer-intent-closed intent=1
peer.close(); replacement bind; awaitReplacement → peer-intent-connect intent=2
DISCONNECTED 4033787/lane1 (이전 pair의 completion lane, 늦은 종료)
READY edge 4033790/lane0            ← intent 2의 첫 attempt A, intent 2에 기록
DISCONNECTED 4033790/lane0 → peer-intent-closed intent=2   ← **결함: attempt 종료를 intent 종료로 판정**
DISCONNECTED 4033793/lane1 (A의 completion lane)
[peer] READY edge 4033799 (attempt B) → HELLO/ADMIT/liveness → 양쪽 PEER_READY, local ADMITTED(intent 2)
:601 replace(gen+1) → intent 2 closed → 예외 없이 intent 3 설치 → 실패
READY edge 4033799 at local      ← B의 edge는 마지막 단언 뒤에 도착
```

attempt A는 local ROUTER가 아직 이전 pair(RID `peer`)의 종료를 마치기 전에 도착한 재연결이라 REJECT되고,
Core connect intent가 B로 재시도해 수렴한다 — D-094가 명시한 동작("old pipe terminate 처리 전 도착한 재연결이
REJECT 뒤 intent 재시도로 수렴"). Core 06-monitoring §3 마지막 항도 "connect intent가 남아 있으면 DISCONNECTED 뒤
자동 재연결은 계속된다"(:543)고 정한다. Java는 이 attempt 종료를 intent 종료로 기록했고, B의 READY edge(:6760
`markPeerIntentsActive`가 closed를 되돌리는 경로)는 wire admission보다 늦어 단언 시점에 intent가 closed였다.

### `observedInprocCloseDoesNotFenceDescriptorReplacement` (run 2)

```text
intent 1 admitted (READY edge 101). peer.close() → DISCONNECTED 101 → peer-intent-closed intent=1
   ← cleanupClosedPeerEndpoint가 inproc이면 return: Core connect intent(재연결 대기) 잔존
replacement bind → READY edge 107 (Java connect 없음 = Core 자동 재연결)
replacePeerConnection → peer-intent-connect intent=2 → READY edge 116
같은 socket이 같은 listener에 intent 둘 → 116/122/128/134/140/146/152 … READY edge·DISCONNECTED 반복(D-096의 충돌)
→ liveness probe는 accepted이나 상대에 닿지 않아 ADMITTED 미관찰(:649)
```

## 원인 (file:line, 수정 전 기준)

1. `ZLinkJavaRawMeshNode.java:6770-6788` `markPeerIntentsClosed` — 기록한 transport가 모두 종료되면 무조건
   `closedPeerIntents.add`. 요청한 close도, admitted connection의 종료도 아닌 pre-admission attempt의 REJECT 종료를
   intent 종료로 판정했다.
2. `:6789-6792` `cleanupClosedPeerEndpoint` — inproc이면 endpoint를 회수하지 않아 Core connect intent가 남고,
   교체 connect가 두 번째 intent가 됐다(D-096 후속 질문의 "framework 결함" 그 경우).

Core 대조(읽기만): `socket_endpoint_runtime.cpp:101-127` `erase_pipes`는 explicit disconnect에서 connect intent와
대기 중 재연결을 함께 회수하고, `socket_base_api.cpp:1761-1780`은 unexpected peer detach에만 `reconnect_inproc`을
예약한다. 따라서 Java가 endpoint를 disconnect하면 Core 재연결은 남지 않는다 — Core 결함 없음, STOP 조건 아님.

## 수정

`ZLinkJavaRawMeshNode.java`

- `:6737-6751` `cleanupTerminalTransport`가 "이 종료가 peer의 admitted connection을 닫았는가"를 반환하고
  (`disconnectAdmitted(peer, id)`가 `topology.disconnect` 결과를 반환, `:6991`), `:6726` DISCONNECTED 처리가 그 값을
  `markPeerIntentsClosed(event, admittedConnectionClosed)`로 넘긴다. RID 없는 종료는 `false`.
- `:6787-6819` `markPeerIntentsClosed` — 기록 transport가 비면 live만 해제하고, **close를 요청했거나(transport-liveness
  §5) admitted connection의 liveness close(mesh-node §7.1 3항)일 때만** closed로 표시하고 endpoint를 회수한다.
  REJECT된 pre-admission attempt는 intent를 열어 두어 Core 재시도의 다음 READY edge를 그대로 기록한다.
- `:6821-6834` `cleanupClosedPeerEndpoint` — inproc 예외 제거. closed intent는 transport와 무관하게 Core connect
  intent를 회수한다(요청 close 경로의 두 번째 disconnect는 ENOENT로 무시, TCP와 같은 처리).

`ZLinkJavaRawMeshNodeTransportIdentityTest.java` — 헬퍼를 `terminate`(admitted 아님)/`terminateAdmitted`로 나누고
회귀 2개 추가: `rejectedAttemptTerminationKeepsIntentUntilAdmittedConnectionCloses`(동기 monitor 구동: REJECT 종료 →
open·not live, 재시도 READY → live, admitted 종료 → closed), `observedInprocCloseRetiresTheCoreConnectIntent`(실제
inproc: peer close 관찰 → 같은 endpoint 재bind 200 ms 동안 부활 없음 → 교체 intent 하나로 ADMITTED). 기존 4개는 유지.
M6A 원본 assertion은 바꾸지 않았다.

**수정 전/후 규칙 수:** intent 종료 규칙 2 → 1 ("기록 attempt 종료" + "inproc은 endpoint 미회수" → "connection이
끝났을 때: 요청한 close 관찰 또는 admitted connection의 liveness close"), transport 예외 1 → 0. 새 상태·타이머·인덱스
없음(`closedPeerIntents`/`closeRequestedPeerIntents`/`peerIntentTransports` 재사용).

## 네 줄

- **소유 계층:** Core가 physical attempt의 REJECT·재시도·재연결을 소유(D-094, 06-monitoring §3). Java raw mesh는 logical
  connection의 종료 판정(요청 close 관찰, liveness close)과 그에 따른 connect intent 회수를 소유.
- **Spec 조항:** `05-transport-liveness.ko.md:241-249`(endpoint 단위 close 요청 뒤 관찰로만 교체; connection_id는 correlation
  전용), `03-mesh-node.ko.md:305-308` §7.1 재연결 3조건(liveness로 이전 pipe 종료 확정), `01-channel-topology.ko.md:485-487`
  (이전 intent가 liveness close로 닫히기 전 새 intent 설치 금지), Core `06-monitoring.ko.md:91-96, :543`. 테스트 기대치는
  §7.1과 충돌하지 않는다: admitted 뒤 다른 generation의 replace는 close 요청 뒤 관찰까지 예외여야 한다.
- **교차언어 대조:** Node `raw-service-mesh-runtime.ts:1044-1050`은 DISCONNECTED id가 admitted `peer.connectionId`와
  같을 때만 `removePeer`(candidate 종료는 candidate만 제거) — 이번 규칙과 동일. .NET `ZLinkManagedMeshNode.cs:8586-8594`는
  candidate가 모두 사라질 때 peer 전이, `:11217-11220`은 outbound peer 제거 시 transport 구분 없이 endpoint를 disconnect
  ("binding의 endpoint reconnect intent 취소"). C++ `raw_mesh_node_owner.cpp:802-806`도 stale 동일 endpoint 교체 전
  `disconnect(endpoint)`. Java만 attempt 종료를 intent 종료로 쓰고 inproc을 예외로 둔 구조적 차이였다.
- **변경 분류:** **B — 기존 결함 수정.** 감독의 fix 지시를 구현 승인으로 적용. timeout·retry·assertion 변경 없음.

## 결과

| 검증 | 결과 | 증거 |
|---|---|---|
| 수정 전 M6A class ×3 (trace 승격 포함) | 1회 27/28, 2·3회 26/28 | `class-{1,2,3}.*` |
| 단위 회귀 `ZLinkJavaRawMeshNodeTransportIdentityTest` | 6/6 PASS | `unit-1.*` |
| 수정 후 M6A class ×3 (`--rerun`) | 28/28 ×3 PASS | `fixed-class-{1,2,3}.*` |
| `:zlink-framework-core:test contractTest --continue` ×1 | exit 0 — core 1,274 tests 0 FAIL(XML 집계); contract 96/96(core 27, kotlin 17, provider-abstractions 4, testkit 48) | `gate.log`, `gate/` |

수정 후 trace(`fixed-class-1.stderr`): descriptor-replace는 intent 2의 attempt 182 DISCONNECTED에 `peer-intent-closed`가
없고 재시도 191 READY edge를 기록한 뒤 :601의 replace가 `peer-intent-disconnect intent=2`(요청 close→예외)로 끝난다.
observed-close는 `peer-intent-closed intent=1 admittedClosed=true` 뒤 교체 attempt 113 하나로 ADMITTED다.

## BLOCKERS

없음. 남은 관찰 사항(이번 범위 밖, 결함 주장 아님):

- liveness timeout·inbound close로 `disconnectAdmitted`가 peer를 CLOSED로 만드는 경로는 intent를 닫지 않으므로, 그 뒤
  교체는 여전히 close 요청 → 관찰 순서(§5)를 거친다. 기존 동작 유지.
- `drainPeerCloseRequests:996`의 inproc 분기(disconnect 실패 시 요청 유지)는 원인과 무관해 두었다.
- 커밋하지 않았다. 다른 job의 dotnet/node 변경과 `bindings/node/provenance`는 손대지 않았다.
