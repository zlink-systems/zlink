# Java M6A admission·intent 완료 게시 경계

Java raw mesh의 admission·종료 관찰 race를 수정했다. Public receive poll에서 진행한
transport 전이를 관찰한 뒤 logical admission을 게시하며, 귀속되지 않은 이전 attempt의
종료는 현재 admission 완료를 취소하지 않는다. Endpoint 회수도 끝난 뒤 intent의 종료
완료를 공개한다. D-097의 단일 descriptor 게시 루프와 shutdown seal 의미는 유지했다.

## 실패 trace와 원인

증거 디렉터리는 `/tmp/zlink-java-m6a-seal-parity/`다. 두 지목 테스트의 실제 실패
XML·stream trace를 모두 보존했다. 임시 runtime 로깅은 추가하지 않았다.

### READY 이전 replacement

`fixed-10.log`와 `fixed-10-xml/zlink-framework-core/test/`의 M6A XML·stderr에
`replacementDoesNotSkipAConnectionBeforeItsReadyEvent`의 `ADMITTED` 대기 실패가 있다.
해당 RID의 전체 trace는 `pre-ready-failure.trace`다.

```text
intent 1: READY 203 -> DISCONNECTED 203 -> closed
intent 2 connect -> Hello -> wire Admit (logical ec15cbb3…)
late READY 209 -> adopts ec15cbb3… -> DISCONNECTED 209
peer-intent-closed intent=2 admittedClosed=true -> endpoint disconnect
READY 218 -> DISCONNECTED 218; Hello continues, no ADMITTED
```

수정 전 `ZLinkJavaRawMeshNode.java:4238-4246`은 monitor를 비운 뒤 `port.receive`를
호출했다. `ZLinkJavaRawServicePort.java:239-247`은 그 안에서 public poller를 진행한다.
그 진행에서 생긴 이전 attempt의 READY/종료는 다음 loop에서 처리되지만, 받은 Admit는
같은 loop에서 먼저 dispatch됐다. `registerTransportConnection`이 monitor identity가
없는 wire admission에 다음 READY의 identity를 연결하면서 새 Admit가 이전 attempt
209에 귀속됐다. 이어진 209의 종료가 admitted connection의 종료로 해석되어 새 intent를
회수했다. READY와 종료의 ID/lane 비교는 일치했으므로 기존 transport-identity 수정으로는
막지 못하는 경로였다.

### Descriptor fence replacement

`final-1.log`와 `final-1-xml/zlink-framework-core/test/`의 M6A XML·stderr에
`descriptorFenceReplacesEndpointOnlyIntent`의 `ADMITTED` 대기 실패가 있다.
해당 RID의 전체 trace는 `descriptor-failure.trace`다.

```text
intent 2 connect -> Hello -> Admit (logical f908afcb…, new lifecycle)
DISCONNECTED 182, no READY registration -> admission completion removed
DISCONNECTED 185 (completion lane)
READY 191 -> Update -> probe/ACK accepted -> PEER_READY on both nodes
isReadyPeer still false: admissionControlReadyConnections entry is absent
```

`cleanupTerminalTransport`는 `removeTransportConnection(event)`가 null이어도 RID만으로
`admissionControlReadyConnections`를 제거했다. 이전 attempt 182의 귀속되지 않은 종료가
새 Admit 완료를 취소한 것이다. `isReadyPeer:1316-1326`은 이 완료 표식과 liveness를
모두 요구하므로, 양쪽 ACK와 `PEER_READY` 뒤에도 public peer 목록은 `ADMITTED`가 아니었다.
이 실행은 요청에 적힌 최종 `assertThrows` 실패와 달리 같은 테스트의 admission 대기에서
실패했다. 원래 assertion 실패와 동일한 관찰 결과라고 바꾸어 기록하지 않는다.

### Endpoint 회수의 완료 게시

별도로, 수정 전 `markPeerIntentsClosed:6829-6831`은 `closedPeerIntents.add` → close
요청 제거 → endpoint 회수 순서였다. Caller의 `replacePeerConnection:930-951`은
ConcurrentHashMap 기반 완료 set을 보고 새 intent를 설치할 수 있었다. 이후 이전 cleanup의
`RouterSocket.disconnect(endpoint)`가 새 Core connect intent를 회수할 수 있는 경계였다.

`retirement-red.log`와 `retirement-red-xml/zlink-framework-core/test/`에는 요청 close와
admitted close 모두 다음 동기 assertion으로 실패한 증거가 있다.

```text
replacement became eligible before endpoint retirement
expected: <false> but was: <true>
```

이 별도 결함의 수정만으로는 M6A가 해결되지 않았다. 그 상태의 10번째 검증에서 위 READY
이전 replacement 실패가 나왔고, 전체 gate를 시작하기 전에 다음 원인을 분리했다.

### Crossed Hello/Admit의 늦은 READY

첫 전체 gate는 `crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness`
준비 단계의 `:158`에서 실패했다. Trace는 `gate-shutdown-crossed.trace`, 원본은
`gate-xml/zlink-framework-core/test/`의 ShutdownSeal XML·stderr다.

```text
cross-left Hello -> logical 74bbdfd8… (command infers INBOUND)
Hello/Admit/Update -> ACK -> ready
late READY 520 (actual OUTBOUND) -> new candidate ca507f25…
repeated Admit consumes ca507f25… -> liveness identity changes
```

`registerTransportConnection`은 늦은 READY를 기존 wire admission에 연결할 때 command가
추론한 방향까지 일치하도록 요구했다. Core가 선택한 route와 Hello/Admit의 방향 추론은
같은 사실이 아니므로 이 조건으로 새 logical identity를 만들 수 있었다. M6A의 기존
`connectionIdForAdmissionReusesCoreSelectedRouteAcrossCommands`와 같은 identity 보존
규칙을 READY 등록에도 적용했다. 이미 monitor에 등록한 identity는 이 경로의 대상이 아니다.

`direction-red-valid.log`의 동기 회귀는 `wire-admitted-before-ready` 대신 새 UUID를
반환하여 실패했고, 수정 후 ShutdownSeal 4개와 TransportIdentity 10개가 모두 통과했다
(`direction-green.log`). 기존 ShutdownSeal fixture·준비 조건·assertion은 변경하지 않았다.

## Parity diff 대조

| D-097 변경 | 지목 테스트의 호출 경로와 대조 |
|---|---|
| `publishLocalDescriptor`의 게시 루프 통합 | Serving은 기존에도 `trySendAdmissionControl`이었다. Weight Update만 direct `port.send`에서 같은 helper로 바뀌었다. 두 테스트는 weight를 변경하지 않는다. Update는 `markAdmissionControlReady`의 Admit 전용 조건도 통과하지 않는다. |
| `announceExpectedPeers`의 seal 조회 | 두 테스트는 host coordinator를 배선하지 않는 raw node fixture다. Predicate는 기본 `() -> false`이며 seal/unseal 전이가 없다. Coordinator 자체에도 unseal 연산은 없다. |
| Host shutdown의 raw node Draining 게시 | 두 테스트는 host `runDrain`을 호출하지 않고 raw node를 직접 close한다. |
| Hello 제출·admission 완료 trace | 두 테스트에서도 실행되어 caller/pump 스케줄에 영향을 줄 수 있다. 이번 표본만으로 parity 변경이 실패율을 올렸다고 입증하지는 못했다. |

`ea71eab005`와 작업 시작 HEAD `e596c0e7cd` 사이 Java tracked diff는 없다. 변경 전
trace 활성화 M6A 10회는 모두 28/28이었다(`repro-1`부터 `repro-10`의 log/XML).
Java 범위만 `git stash push -u` → M6A 1회 → `git stash pop`으로 비교한 결과도
28/28이었다(`baseline.log`, `baseline-xml/`, `baseline-wrapper.log`). Stash는 복원·제거됐다.
따라서 parity가 새로 도입한 descriptor 게시 규칙 결함으로 분류하지 않는다. 확인한
관찰·완료 게시 결함은 parity 이전에도 존재한다.

## 수정과 회귀

경로의 공통 앞부분은 `framework/languages/java/zlink-framework-core/src/`다.

| 변경 파일 | 최종 변경 |
|---|---|
| `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java` — `startPump:4231` | Public receive/poller 진행 뒤 기존 monitor drain을 실행하고 logical control·admission을 처리한다. 두 번째 drain·poller·pending-admission queue를 추가하지 않았다. |
| 같은 파일 — `cleanupTerminalTransport:6754` | 귀속되지 않은 종료는 현재 admission 완료를 변경하지 않는다. 귀속된 종료는 기존 `disconnectAdmitted(peer, connectionId)`가 소유한다. |
| 같은 파일 — `registerTransportConnection:7111` | 늦은 READY를 아직 monitor에 등록되지 않은 기존 admission ID에 연결한다. Hello/Admit가 추론한 방향의 일치 조건과 불필요해진 direction 인자를 제거했다. |
| 같은 파일 — `markPeerIntentsClosed:6802` | Endpoint 회수와 close 요청 정리를 끝낸 뒤 `closedPeerIntents.add`로 완료를 게시한다. |
| `test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeTransportIdentityTest.java:141` | 귀속되지 않은 종료의 admission 완료 보존, 요청/admitted close의 endpoint 회수 전 완료 미공개를 검증하는 동기 회귀와 늦은 READY의 방향 불일치 회귀를 포함해 4개를 추가했다. 기존 6개는 유지했다. |
| 이 결과 문서 | 실패 증거·원인·검증 결과를 기록한다. |

새 회귀는 기존 monitor 처리 메서드를 동기 구동한다. Endpoint 회수 검증은 public
RouterSocket interface의 test proxy에서 disconnect 진입 시 완료 상태를 조회한다.
`unmapped-red.log`의 추가 회귀는 `expected: <admitted-connection> but was: <null>`로
실패했고, `unmapped-green.log`에서 통과했다. 기존 M6A assertion은 변경하지 않았다.
Sleep·재시도·timeout 증가·임시 runtime 로그·새 physical 상태·frame filter는 없다.

대안으로 caller/pump 사이 새 lock 또는 별도 pending-admission queue를 검토했다.
기존 pump가 receive 진행에서 생긴 관찰을 반영하고 기존 완료 set의 게시를 마지막에
두면 해결되므로, 상태·동기화 소유자를 추가하지 않았다. 알 수 없는 종료를 현재 peer의
실패로 취급하는 예외도 제거했다. Core의 physical 선택·REJECT 재시도는 변경하지 않았다.

**수정 전/후 규칙 수:** admission 게시 경계 2 → 1(wire 즉시 게시/다음 loop monitor
귀속 → 진행의 관찰 후 게시), terminal의 완료 취소 규칙 2 → 1(귀속 불명 RID 제거/
정확한 connection 종료 → 정확한 connection 종료), intent 완료 경계 2 → 1(교체 허용
공개/endpoint 회수 → 회수 후 완료 공개), 늦은 READY의 wire identity 처리 2 → 1
(방향 일치 때 재사용/불일치 때 새 identity → Core-selected identity 재사용). Descriptor 게시 소유자와 seal 소유자는 각각
parity의 1 → 1이다.

## 소유 계층·계약·교차언어·분류

- **소유 계층:** Java Framework raw mesh pump의 logical admission·intent 종료 완료 게시. Core는 physical REJECT/HANDOVER·재연결·endpoint disconnect 실행을 소유한다.
- **Spec 조항:** mesh-node §6·§7.1, transport-liveness §5 `:239-249`, channel-topology `:483-487`, Core monitoring §4의 commit 순 event 전달과 polling §3. D-092/D-094/D-096의 physical 정책과 D-097 host-relocation-flow §14 1단계의 seal 뒤 Hello 금지·admitted peer의 Draining Update를 보존한다.
- **교차언어 대조:** .NET `ZLinkManagedMeshNode.cs:4956-4963`은 `Poller.Wait` 뒤 monitor를 반영하고 raw socket을 dispatch하며 intent 변경을 `RunState`로 직렬화한다. Node `raw-service-mesh-runtime.ts:1044-1050`도 admitted connection과 일치하는 종료만 peer 제거에 반영하며, Java처럼 늦은 READY에 기존 admission ID를 이식하지 않는다. Java의 poll 진행 위치·identity 귀속·caller/pump 공유 상태가 구조적으로 다르다.
- **변경 분류:** B — 기존 admission 관찰·intent 완료 게시 결함. 각 원인을 진단·보고한 뒤 감독의 이번 owner fix 요청 범위로 구현했다. Core·binding 보상이나 spec 변경은 없다.

## 검증

환경: `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`,
`unset ZLINK_LIBRARY_PATH`, `ZLINK_JAVA_STREAM_TRACE=1`,
`flock -w7200 /tmp/zlink-jvm-gate.lock`. 최종 실행은 XML·stderr 복사까지 lock 안에서
수행한다. 각 test task는 `--rerun`으로 실행한다.

Maven `systems.zlink:zlink:0.17.0` JAR 내부 `native/linux-x86_64/libzlink.so` SHA-256:
`2055a5819059c91be6afc8c50073f22001bb59598ecf7424045918306ef9f9a0`.
Core·binding을 수정하거나 다시 빌드하지 않았다.

| M6A 실행 | 결과 | 실패 | Skip | Class 시간 | 증거 |
|---|---|---|---|---|---|
| 1 | 28/28 PASS | 0 | 0 | 1.14 s | `final-verified-1.log`, `final-verified-1-xml/` |
| 2 | 28/28 PASS | 0 | 0 | 1.132 s | `final-verified-2.log`, `final-verified-2-xml/` |
| 3 | 28/28 PASS | 0 | 0 | 1.249 s | `final-verified-3.log`, `final-verified-3-xml/` |
| 4 | 28/28 PASS | 0 | 0 | 1.178 s | `final-verified-4.log`, `final-verified-4-xml/` |
| 5 | 28/28 PASS | 0 | 0 | 1.164 s | `final-verified-5.log`, `final-verified-5-xml/` |
| 6 | 28/28 PASS | 0 | 0 | 1.101 s | `final-verified-6.log`, `final-verified-6-xml/` |
| 7 | 28/28 PASS | 0 | 0 | 1.161 s | `final-verified-7.log`, `final-verified-7-xml/` |
| 8 | 28/28 PASS | 0 | 0 | 1.128 s | `final-verified-8.log`, `final-verified-8-xml/` |
| 9 | 28/28 PASS | 0 | 0 | 1.001 s | `final-verified-9.log`, `final-verified-9-xml/` |
| 10 | 28/28 PASS | 0 | 0 | 1.038 s | `final-verified-10.log`, `final-verified-10-xml/` |

| 검증 | 결과 | 증거 |
|---|---|---|
| 최종 ShutdownSeal | 4/4 PASS, skip 0 | `final-focused.log`, `final-focused-xml/` |
| 최종 TransportIdentity | 10/10 PASS, skip 0 | 같은 focused 증거 |
| 첫 전체 gate | Core 1,255 중 1 FAIL; contract 96/96 PASS | `gate.log`, `gate-xml/`; 위 crossed admission의 방향 조건 결함 |
| 해당 결함 수정 후 최종 `:zlink-framework-core:test contractTest --continue --rerun` 1회 | Core 전체 XML 1,256/1,256 PASS; `TEST-*.xml` leaf 집계 1,243/1,243 PASS; contract 96/96 PASS; exit 0 | `final-gate.log`, `final-gate.exit`, `final-gate-xml/` |
| 변경 whitespace·기존 parity 보존·native hash | PASS | `git diff --check`, `final-integrity.txt` |

Contract는 core 27, Kotlin 17, provider-abstractions 4, testkit 48개다. Kotlin의 기존
`test` 출력은 이번 gate 집계에 포함하지 않았다. Gate는 전체 작업 동안 2회 실행했다.
첫 실패에서 새로운 원인을 분리·수정한 뒤 최종 tree로 1회 실행했으며, 실패한 gate를
green 결과로 덮어쓰지 않았다.

통과 trace에도 이전 attempt의 종료가 존재한다. 예를 들어
`verified-3-jvm-descriptor-replace-local.trace`는 READY 182 → DISCONNECTED 182 →
새 Admit → READY 191 → ACK 완료를 보존한다. 최종 ×10의 원본 trace도 각 XML 및
`.stderr`에 보존했다. 원본 M6A는 수정 전과 byte 동일하다.

## BLOCKERS

- 남은 test 실패와 구현 blocker는 없다.
- 작업 요청의 원래 `descriptorFenceReplacesEndpointOnlyIntent` 최종 assertion 실패는
  직접 재현하지 못했다. 같은 테스트의 admission 대기 실패 trace를 확보하고 원인을
  수정했으며, 최종 원본 class ×10과 전체 gate는 통과했다.
- Core·bindings·다른 언어·보호된 spec 문서는 변경하지 않았다. 기존 uncommitted Java
  parity 변경과 다른 사용자 변경을 보존했고, commit하지 않았다.
