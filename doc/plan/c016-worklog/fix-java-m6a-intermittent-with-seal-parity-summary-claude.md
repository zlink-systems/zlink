# Java M6A 간헐 실패 진단 (Claude 병행 조사, 구현 없음)

같은 brief를 수행하는 codex job(`codex exec -m gpt-6-astra`, PID 57071, 21:54 시작,
log `zlink-work/c016/logs/fix-java-m6a-intermittent-with-seal-parity.log`)이 같은 working
tree의 `ZLinkJavaRawMeshNode.java`·`ZLinkJavaRawMeshNodeTransportIdentityTest.java`를
22:01·22:15·22:21에 수정했고, brief의 deliverable 경로에 자기 요약을 이미 썼다. 두 writer가
같은 파일을 고치는 상태라 이 조사는 **runtime·test 파일을 변경하지 않았고** 결과를 이 별도
파일에 남긴다. Branch `main`, commit 없음. 증거: `/tmp/zlink-java-m6a-intermittent/`.

## 결론

1. **Parity diff(D-097)는 지목된 두 테스트의 경로에 없다.** `publishLocalDescriptor` 통합은
   weight/channel/Draining 변경에서만 실행되고(두 테스트는 호출하지 않음; Serving 게시는 이전에도
   `trySendAdmissionControl`), seal predicate는 기본 `() -> false`이며 fixture가 host coordinator를
   배선하지 않고, shutdown Draining 게시는 host `runDrain` 전용이다. 남는 차이는 `STREAM_TRACE`
   gate 뒤의 문자열 두 줄(Hello 제출·admission 완료)뿐이다. Parity 이전 코드에서 28/28 ×3은
   표본이 작아 실패율 차이를 입증하지 않는다.
2. 수정 없는 parity tree에서 M6A class 29회(1–23 무부하, 24–29 20-core busy-loop 부하) **모두 28/28**.
   부하 + codex의 22:01 변경(`markPeerIntentsClosed` 게시 순서 변경)이 들어간 tree에서 21회 중 **8회 실패**,
   실패 형태는 세 가지다. 모두 기존 결함이며 parity diff가 만든 것이 아니다.

## 재현 표

환경: `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`,
`ZLINK_JAVA_STREAM_TRACE=1`, `flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon
:zlink-framework-core:test --tests '…ZLinkJavaRawMeshNodeM6ATest' --rerun -i`. Core native
SHA-256 `2055a5819059c91be6afc8c50073f22001bb59598ecf7424045918306ef9f9a0`.
로그 `repro-N.log`, XML `repro-N-xml/`, 실패 trace `fail-runN-*-trace.txt`.

| run | tree | 부하 | 결과 |
|---|---|---|---|
| 1–23 | parity diff 원본 | 없음 | 28/28 ×23 |
| 24–29 | parity diff 원본 | 20 busy loop | 28/28 ×6 |
| 30–31 | codex 변경 시각(22:02–22:03)과 겹쳐 판정 불가 | 부하 | 28/28 ×2 |
| 32–52 | codex `markPeerIntentsClosed` 순서 변경 포함 | 부하 | 13 PASS / 8 FAIL (32, 35, 36, 37, 40, 46, 48, 50) |

| run | test | 실패 |
|---|---|---|
| 32, 40 | replacementDoesNotSkip…:689, descriptorFence…:587 | `ZlinkBindException` — 교체 peer의 같은 inproc endpoint bind |
| 35, 36, 37, 46, 48 | descriptorFence… / replacementDoesNotSkip… (`awaitReplacement:1414`) | 2초 내 이전 intent가 closed로 남지 않음 |
| 50 | observedInprocClose…:642 | `previous peer connection has not completed liveness close` (CLOSED·not-live 관찰 뒤 replace) |

## 실패 A — Core `zlink_close` 뒤 같은 inproc endpoint bind가 `EADDRINUSE` (run 32·40)

- trace `fail-run32-pre-ready-trace.txt`: intent 1 connect → close 요청 → READY 197 edge 뒤 test thread의
  `replacementPeer.start()`(`ZLinkJavaRawMeshNode.start:717` bind)가 예외.
- 원인(Core): `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1361-1371` `process_term`이
  reaper thread에서 `unregister_endpoints`를 실행한다. `zlink_close`(`socket_base_control.cpp:13-28`)는
  handoff만 하고 반환하므로 inproc endpoint 해제는 비동기다. `zlink_unbind`는
  `socket_base_endpoint.cpp:986+ term_endpoint_internal`에서 동기 `unregister_endpoint`다.
- 공개 C API repro `close_rebind_inproc.cpp`(bound ROUTER close → 즉시 같은 endpoint bind):
  부하 중 **3000회 중 2130회 `EADDRINUSE`**.
- spec: `core/doc/spec/core/socket/README.ko.md:609-622` "socket을 닫고 관련된 모든 자원을 해제한다",
  `:818` bind `EADDRINUSE`. 해제 시점이 비동기라는 문장은 없다 → Core 결함 또는 spec gap(D). Java
  binding `RouterSocket.unbind`는 존재하지만 raw node close에서 unbind를 먼저 호출하는 것은 상위 계층
  보상이라 승인 전 구현하지 않았다. .NET·C++·Node raw node도 unbind 없이 close한다.
- 영향 범위: `peer.close()` 직후 같은 endpoint에 bind하는 fixture(M6A :582-586, :684-688). Peer의
  DISCONNECTED를 먼저 관찰하는 테스트(observedInprocClose, ShutdownSealTest, TransportIdentityTest)는
  `process_term`이 `unregister_endpoints` 뒤에 pipe를 종료하므로 구조적으로 안전하다.

## 실패 B — closed intent가 뒤늦은 READY edge로 다시 열림 (run 35·36·37·46·48, 다수)

trace `fail-run36-trace.txt`(요약):

```text
peer-intent-connect intent=1 (endpoint-only) → peer-intent-disconnect intent=1 (close 요청 실행)
READY 200 edge → DISCONNECTED 200 → peer-intent-closed intent=1 admittedClosed=false   ← closed
READY 200 flags=0, DISCONNECTED 203 lane1, READY 203 flags=0
READY 206 edge rid=peer                 ← 새 attempt. markPeerIntentsActive가 intent 1을 다시 open (closedPeerIntents.remove)
DISCONNECTED 209 lane1, DISCONNECTED 206 → transports 비움, admitted 아님·close 요청 아님 → open 유지(fence-3 규칙)
Hello ×N, peer-intent-disconnect intent=1 (awaitReplacement가 다시 close 요청) → 기록 transport 없음 → 영구 미종료 → :1414
```

- 원인(Java): `ZLinkJavaRawMeshNode.java:6771-6785 markPeerIntentsActive`는 READY edge마다
  `closedPeerIntents.remove(intentId)`로 **closed를 되돌린다**. fence-3 이후 closed는 "요청 close 관찰 또는
  admitted close 관찰"이라는 종단 상태인데, 이 경로가 4번째 상태(closed→open→not-live, 다시 닫을 transport 없음)를
  만든다. 남은 규칙: closed는 종단이며 뒤늦은 READY는 transport만 기록한다(규칙 2→1).
- 206 attempt의 출처는 미확정이다. 공개 C repro `disconnect_then_rebind_reconnect.cpp`(connect → disconnect
  → peer close → 다른 RID로 같은 endpoint 재bind, 순서·간격 6가지, 각 200–300회)에서 disconnect는 전부 성공하고
  **새 READY edge 0건**이라, "Core가 명시적 disconnect 뒤 재연결한다"(README.ko.md:869 위반)는 이 형태로는
  재현되지 않았다. Java 쪽에서 두 번째 connect가 없는데 attempt가 생기는 원인은 codex 변경 tree에서만 표본이
  있어 감독 판단으로 남긴다.

## 실패 C — CLOSED·not-live 게시와 closed 게시 사이의 창 (run 50)

trace `fail-run50-trace.txt`: `DISCONNECTED 101 → peer-intent-closed intent=1 admittedClosed=true`가
기록됐는데 caller의 `replacePeerConnection:947`은 intent 1을 closed로 보지 못했다.
`cleanupTerminalTransport → disconnectAdmitted`(peer CLOSED 게시)와 `livePeerIntents.remove`(not-live 게시)가
`closedPeerIntents.add`보다 먼저 일어나고, 그 사이에 trace 로깅과(codex 변경 뒤) Core `router.disconnect`가
있다. 테스트는 CLOSED + not-live를 보고 replace를 호출한다. 한 사실("intent의 연결이 끝났다")이 세 곳에 게시된다.
codex의 22:01 변경은 endpoint 회수를 closed 게시 앞으로 옮겨 이 창을 넓혔다(그 변경이 막으려는 "새 intent를 이전
cleanup이 끊는" 문제도 실제다). 제안: endpoint 회수 뒤 `closedPeerIntents.add` → `livePeerIntents.remove` 순서로
게시해 not-live를 본 관찰자는 항상 closed를 본다(규칙 3→1). 미구현.

## 네 줄

- **소유 계층:** A는 Core socket lifecycle(inproc endpoint 해제 시점). B·C는 Java raw mesh의 logical intent
  종료 상태기계. 206 attempt 출처가 Core면 A와 같은 Core 소관.
- **Spec 조항:** Core `socket/README.ko.md:609-622`(close), `:869`(disconnect는 재연결 intent 제거);
  framework `05-transport-liveness.ko.md:239-249`, `03-mesh-node.ko.md` §7.1(3).
- **교차언어 대조:** .NET(`ZLinkManagedMeshNode.cs:8617-8700`)은 DISCONNECTED에서 남은 candidate가 없을 때만
  peer를 Connecting으로 되돌리고 endpoint를 끊지 않는다. Node(`raw-service-mesh-runtime.ts:1044-1058`)와
  C++(`raw_mesh_node_owner.cpp:3836-3860`)는 monitor connection id 일치로만 peer를 제거한다. closed intent를
  READY edge로 되돌리는 규칙은 Java에만 있다.
- **변경 분류:** A — Core 결함/spec gap(D), Core·binding 수정 금지라 BLOCKER. B — Java 기존 결함(B), 미구현.
  C — Java 기존 결함(B), 미구현. Parity diff 자체는 원인이 아니다.

## BLOCKERS

- **동시 writer.** codex PID 57071이 같은 파일을 수정 중이며 deliverable 경로를 점유했다. 두 diff를 병합할
  주체를 감독이 정해야 한다. 이 조사는 파일을 바꾸지 않았다.
- **Core close 비동기(A).** Java만으로는 `peer.close()` 직후 같은 endpoint bind를 보장할 수 없다. Core가 close에서
  inproc endpoint를 동기 해제하거나, spec이 비동기를 명시하고 fixture가 관찰 가능한 조건을 기다리도록 바뀌어야 한다.
- **×10 acceptance 미달.** 실패는 부하 하에서만 나왔고(무부하 23/23 PASS), 부하 표본은 codex 변경이 섞인
  tree다. 원본 parity tree의 부하 표본은 6회(모두 PASS)뿐이다.
- `:zlink-framework-core:test contractTest` 전체 gate는 tree가 바뀌는 중이어서 실행하지 않았다.
- `/tmp/zlink-java-m6a-intermittent/` 안의 C repro 2개는 설치 Core(`~/.cache/zlink/core/0.17.0/linux-x64`)로
  빌드했다. Core·bindings·spec·다른 언어는 읽기만 했다.
