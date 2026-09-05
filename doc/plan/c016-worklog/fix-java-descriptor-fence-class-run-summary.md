# Java raw mesh M6A descriptor fence — Core 결함 확인으로 중단

**STOP: 설치된 rebuild8 Core의 REJECT 정책이 같은 socket·endpoint의 두 번째 connect에서
기존 admitted pipe를 종료한다.** 공개 C API만으로 첫 request/reply 성공 뒤 기존 연결의
DISCONNECTED와 재연결 반복을 확인했다. §4의 “기존 pipe 유지”와 충돌하므로, 요청의 STOP
조건에 따라 Java runtime 수정과 최종 전체 gate를 진행하지 않았다. D-092의 빠른 종료
관찰 자체를 결함으로 판정한 것은 아니다.

Java에도 endpoint 일치만으로 다른 lane·attempt의 종료를 intent 전체에 적용하는 기존
결함이 있다. 두 결함을 분리해 아래에 기록한다. Runtime·test의 최종 변경은 없으며,
변경 파일은 이 보고서 하나다. 임시 진단 로그 코드는 전부 제거했고 커밋하지 않았다.

## 환경과 재현 결과

- 작업 위치: `/home/hep7/project/zlink`, branch `main`.
- Java 작업은 `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`,
  `unset ZLINK_LIBRARY_PATH`, `flock -w7200 /tmp/zlink-jvm-gate.lock`으로 실행했다.
- 설치 Core SHA-256:
  `f20f5cdba0bc117b17db7f8e9fec25b47d79de29bdfbff1e88ceaa8a032d2640`.
  `.artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar` 안의
  `native/linux-x86_64/libzlink.so`·`.so.0` 모두 같은 hash다. C 재현의 `ldd`도
  `.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0`을 확인했다.
- Package·Core를 재빌드하지 않았다. 시작 시 있던 Spot lifecycle·shutdown·Node binding
  변경과 실행 중 추가된 타 작업의 Spot/Stream 변경은 수정하지 않았다.
- 요청된 `framework/languages/java/AGENTS.md`는 존재하지 않는다. 루트와
  `framework/AGENTS.md`, `doc/AGENTS.md`를 적용했다.

| 실행 | 결과 | 보존 자료(`/tmp/zlink-java-m6a-diagnosis/`) |
|---|---|---|
| M6A class, 기존 `ZLINK_JAVA_STREAM_TRACE=1` | 28개, observed-close 1 FAIL | `class-baseline.log` |
| M6A class, 기존 trace | 28개, 두 대상 2 FAIL | `class-trace.log`, `class-trace.xml` |
| descriptor-fence 단독, 기존 trace | 1/1 PASS | `single-trace.log`, `single-trace.xml` |
| M6A class, monitor·intent 임시 진단 추가 | 28개, 두 대상 2 FAIL | `class-diag.log`, `class-diag.xml`, `selected-diag.log` |
| 공개 C API, 기본 REJECT | 최초 request/reply PASS; 기존 양쪽 연결 종료, exit 2 | `c-reject.log` |
| 같은 C API, 양쪽 HANDOVER 대조 | READY 각 2회, DISCONNECTED 0, exit 0 | `c-handover.log` |
| 최종 `:zlink-framework-core:test contractTest` | 미실행 — Core 결함 STOP 조건 | — |

첫 실행 XML은 다른 Java 작업의 테스트가 덮어썼다. 이후 실행은 Gradle 종료 뒤 XML 복사까지
같은 lock 안에서 수행했다. 기존 stream trace로 admission 이후 메시지가 멈춘 것을 먼저
읽고, 그것으로 확인할 수 없는 native monitor/intent 경계에만 임시 진단을 넣었다.

## 테스트별 판정과 관찰 순서

### `descriptorFenceReplacesEndpointOnlyIntent`

**Java 기존 결함 B가 확인되며, 이후 동일 endpoint 재연결에는 아래 Core B도 걸린다.**
이번 재현의 실패는 첫 assertion(:574)이 아니라 **교체 뒤 마지막 assertion(:601)** 이다.
“첫 replace 전에 endpoint-only intent가 닫힌다”는 원래 설명은 이번 실행에서 재현되지 않았다.

`ZLinkJavaRawMeshNodeM6ATest.java:572-580`의 첫 연결과 첫 교체에서는 다음을 관찰했다.

1. Endpoint-only intent 1을 한 번 connect한다. Application READY `connectionId=1430`, lane 0.
2. 첫 `replacePeerConnection`은 `closed=false`, `closing=false`를 읽고 close를 요청한다.
   예상대로 `IllegalStateException`을 던진다. `awaitTransport`와 이 호출 사이에 intent를
   닫는 event나 두 번째 connect는 없다.
3. Pump가 intent 1 endpoint를 disconnect하고, application DISCONNECTED 1430을 읽어
   intent 1을 닫는다. 이후 descriptor intent 2를 설치한다.
4. Intent 2에서 application READY 1436/lane 0을 등록한 뒤, completion lane의
   DISCONNECTED 1439/lane 1을 읽는다. 이 event가 **intent 2의 transport set을 비우고
   closed=true로 만든다**. Application DISCONNECTED 1436은 그 다음에 도착한다.
5. 마지막 assertion의 replace는 `intent=2 closed=true closing=false transports=null`을
   읽고 intent 3을 설치하므로 예외가 없다. 이후 READY/REJECT 종료 반복도 나타난다.

직접 원인:

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7256-7263`
  `TransportIdentity.matches`는 nonzero connection ID와 lane이 **달라도** endpoint key가
  같으면 true다. 따라서 실제로 다른 attempt/lane인 record도 같은 transport로 취급한다.
- 같은 파일 `:6774-6775`의 `removeIf`는 일치하는 transport를 전부 제거하고,
  `:6783-6788`은 set이 비면 intent를 닫는다. 이는 admitted connection의 종료와 endpoint의
  임의 종료를 구분하지 못한다. D-092 이전 final ack 지연이 감추던 관찰 순서가 드러났다.

빠른 DISCONNECTED를 무시하거나 final ack를 기다리는 것으로 해결하면 안 된다. Framework는
현재 connection의 종료를 올바르게 귀속해야 한다. 다만 현재 liveness spec은 monitor
`connection_id`를 physical fence로 사용하는 것을 명시적으로 금지하므로, ID 기반 새 pair
상태를 넣는 안은 채택하지 않았다. 아래 Core STOP 상태에서 Java 패치도 만들지 않았다.

### `observedInprocCloseDoesNotFenceDescriptorReplacement`

**Core 기존 결함 B가 공개 API로 확인되므로 STOP.** Java의 stale native endpoint 등록도
이 Core 경로를 유발한다. 실패는 replacement 이후 ADMITTED 대기(:649 → :1400)다.

1. Intent 1으로 admission이 끝난다. Peer close 뒤 application DISCONNECTED 101/lane 0이
   intent 1을 정상적으로 닫는다. D-092가 요구한 올바른 종료 관찰이다.
2. `cleanupClosedPeerEndpoint(:6795)`는 inproc이면 return하여 native connect intent를
   남긴다. Core는 종료 관찰 시 이미 자동 재연결을 예약할 수 있다.
3. Replacement peer가 같은 endpoint에 bind한다. `replacePeerConnection(:942-951)`은
   stale Java intent만 제거하고 새 native connect를 호출한다. 실제 로그의 명시적 connect는
   intent 1과 intent 2 각 한 번뿐이다. 이후의 다수 READY는 Java가 반복 connect한 결과가 아니다.
4. Local은 자동 재연결 READY 107과 새 connect READY 116을 관찰한다. 새 attempt 116의
   DISCONNECTED가 intent 2를 닫고, 122·128·134…에서도 같은 일이 반복된다.
   나중에는 replacement peer에서도 기존 application 107의 DISCONNECTED를 관찰한다.
5. Java endpoint 일치 결함은 READY 107과 116을 함께 제거한다. 그러나 이를 고쳐도 Core가
   실제 기존 pipe까지 종료하는 문제는 해결되지 않는다.

`peerIntents(:145)`, transport/closed/live set, `nextIntent(:172)`는 node instance의 field다.
두 테스트는 별도 context와 고유 endpoint를 사용한다. 이번 증거는 같은 테스트 안의 종료·
재연결 경쟁으로 설명되며, process-wide registry 누출이나 class 순서 의존 상태가 원인이라는
증거는 없다. Native monitor ID가 process-wide로 증가한다는 사실만으로 coupling을 뜻하지 않는다.

## Core 원인과 공개 C API 시퀀스

`core/src/runtime/sockets/router/router_admission.cpp:353-362`는
`same_local_endpoint_reconnect`이면 REJECT 검사를 우회한다. 기존 pipe가 여전히 정상이고
request/reply가 가능해도 같은 locally initiated endpoint라는 이유로 새 pipe를 선택한다.
`:418-429`는 `_handover`가 false인 같은 방향 연결의 이전 pipe를 standby로 두지 않고
종료하며, `:465` 부근 `finish_route_adoption`에서 `terminate(true)`를 호출한다.

상대는 REJECT대로 새 pipe를 닫지만 connector는 이미 자기 기존 pipe를 교체·종료했다.
그 종료가 다시 connect intent를 진행시킨다. **거부된 새 pipe의 종료 관찰·재연결은 §4/D-092에
맞지만, 기존 admitted pipe를 먼저 종료하는 예외는 §4에 맞지 않는다.**

재현은 Java·binding·Framework 코드와 internal Core API를 사용하지 않는다. 컴파일 가능한
전체 소스는 `/tmp/zlink-java-m6a-diagnosis/reject_same_socket.cpp`에 보존했다.

```c
ctx = zlink_ctx_new();
L = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);  /* RID "L", linger 0, probe 1 */
P = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);  /* RID "P", linger 0, probe 1 */
/* 양쪽 RID_DUPLICATE_POLICY는 기본 REJECT. monitor를 각각 연다. */
zlink_bind(L, "inproc://same-socket-local");
zlink_bind(P, "inproc://same-socket-reject");
zlink_connect(L, "inproc://same-socket-reject");
/* 양쪽 application READY 수집. L→P request, P reply, L completion OK 확인. */
zlink_connect(L, "inproc://same-socket-reject");
/* disconnect/close 없이 app recv와 monitor recv를 200ms 진행한다. */
```

관찰 결과:

```text
L READY id=8 count=1
P READY id=8 count=1
INITIAL REQUEST/REPLY OK
SECOND zlink_connect, same socket + endpoint; no disconnect/close
L READY id=14 count=2
L DISCONNECTED id=14 first=8
L DISCONNECTED id=8 first=8
P DISCONNECTED id=8 first=8
SUMMARY L ready=2449 disconnected=2448 original_closed=1;
        P ready=144 disconnected=2431 original_closed=1
```

READY/종료 반복 횟수는 scheduling에 따라 달라진다. 핵심 assertion은 최초 request/reply가
성공한 connection 8이 application의 disconnect/close 없이 양쪽에서 종료됐다는 점이다.
HANDOVER는 대조 실행에만 지정했으며 Java 정책을 바꾸는 우회로 사용하지 않았다.

```bash
cd /home/hep7/project/zlink
g++ -std=c++17 -O0 -g \
  -I "$PWD/.artifacts/wsl/install/zlink-core/0.17.0/include" \
  /tmp/zlink-java-m6a-diagnosis/reject_same_socket.cpp \
  -L "$PWD/.artifacts/wsl/install/zlink-core/0.17.0/lib" \
  -Wl,-rpath,"$PWD/.artifacts/wsl/install/zlink-core/0.17.0/lib" \
  -lzlink -pthread -o /tmp/zlink-java-m6a-diagnosis/reject_same_socket
timeout 15s /tmp/zlink-java-m6a-diagnosis/reject_same_socket
# exit 2: 기존 admitted connection 종료 검출
timeout 15s /tmp/zlink-java-m6a-diagnosis/reject_same_socket handover
# exit 0: 기존 physical lane 종료 없음
```

## 소유권·계약·교차언어·분류

- **소유 계층:** Core ROUTER admission이 REJECT/HANDOVER 선택·물리 종료·재연결을 소유한다. Java raw mesh는 descriptor intent와 admission/liveness 종료 귀속을 소유한다.
- **Spec 조항:** Core `socket/README.ko.md` §4(:159-173), D-092; Framework `03-mesh-node`의 peer lifecycle, `01-channel-topology.ko.md:477-487`, `05-transport-liveness.ko.md:239-249`, `08-routing.ko.md:167-175`의 descriptor replacement와 close 관찰.
- **교차언어 대조:** C++ `raw_mesh_node_owner.cpp:615`, Node `node-raw-binding-port.ts:256`, .NET `ZLinkManagedMeshNode.cs:290`은 HANDOVER를 설정한다. Java `ZLinkJavaRawMeshNode.start:718-729` 및 `ZLinkJavaRawServicePort.openRouterOnLane:65-74`는 설정하지 않아 기본 REJECT다. C++ `raw_mesh_node_owner.cpp:802-806`은 stale 동일 endpoint를 바꿀 때 기존 native intent도 제거한다. Java만의 증상에는 이 구성·수명 차이가 있으며, 다른 언어가 성공한다고 Core REJECT가 올바른 것은 아니다.
- **변경 분류:** **B 진단** — Core REJECT 예외와 Java transport 종료 귀속의 기존 결함. A 계약 변경으로 assertion을 바꾸지 않았다. Runtime 패치는 없으며 감독의 구현 승인 단계로 진행하지 않았다.

**수정 전/후 규칙 수:** 변경 없음. Core의 “REJECT는 기존 pipe 유지”와 “같은 local endpoint는
REJECT에서도 교체·종료”라는 충돌하는 2규칙은 그대로이며, 소유자 수정 후보는 §4 정책 1개로
통합하는 것이다. Java의 ID/lane 일치 또는 endpoint 일치라는 이중 귀속 규칙도 미수정이다.

대안 비교: Java에 HANDOVER를 설정해 REJECT 경로를 피하거나 retry·timeout을 늘리는 안은
Core 계약 위반을 가리므로 제외했다. Core 정책 소유자의 예외 제거가 우선이다. Java에서는
그 결과와 현행 liveness 계약에 맞춰 기존 connection 관찰 소유자를 정리해야 하며, 새 monitor
pair registry를 추가하는 방식은 적용하지 않았다.

## BLOCKERS

1. Core 같은 socket·endpoint REJECT 재현을 Core 소유자가 처리해야 한다. 이 job은 `core/**`
   수정 금지 및 Core 결함 발견 시 STOP 지시를 준수한다.
2. Java `TransportIdentity.matches`와 inproc endpoint 등록 수명은 후속 B 진단 승인·수정 대상이다.
   Monitor ID는 현행 spec상 진단/correlation 전용이다. 물리 fence로 재사용하는 변경은 승인하지 않았다.
3. Green gate 미달: 대상 두 실패가 남아 있다. Class는 진단 목적으로 3회 실행했으나 통과 3회를
   달성하지 못했다. Core STOP 조건에 따라 최종 `:zlink-framework-core:test contractTest`는 실행하지 않았다.
