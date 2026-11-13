# q1673d — Java per-message cost (#1673)

결과: 미완료. 승인된 `40e6578ab3` revert를 커밋했고, 최종 소스는 `0f2e5a176b`와 같다. node 격리 수정은 유지했다. 30초 warmup 교대 측정에서 send는 기준 이하였지만 serial source·target은 모두 기준을 넘었다. 추가 runtime 최적화는 구현·커밋하지 않았다. 확정된 스펙 위반 2건은 사용자 지시에 따라 BLOCKERS로 보고하고 해당 부분의 수정을 중단했다.

작업 위치: `D:/worktree/zlink-1639-msg-java`, 브랜치 `framework-java/1639-per-message-cost`. 시작 HEAD `40e6578ab3c14f0139841b7c8f22f1b02e2bd1e6`; 비교 기준 `848ce30597627cd9bf3c86312fec920eed302932`; main 대조 `a7ac59c5bd4fea1539397c565bc59ce8b0e1c62d`.

커밋 sha: `01d3ed75e7c53d2f2707154e7ecffa79ca9f098f`. 커밋 본문에 `Refs #1673`을 포함했다. `git revert --no-edit 40e6578ab3` 실행 후 이 job에서 만든 revert 커밋의 본문만 보완했다. push·PR·merge·브랜치 전환·reset·stash·GitHub·Project 보드 작업은 수행하지 않았다.

## CPU 측정

WSL `/home/hep7/worktree/zlink-1673-java`에서 1024 B, 같은 JVM의 같은 workload로 warmup 30초 후 active 5초, 각 3회 중앙값을 측정했다. 매 회 `848ce30 send → 848ce30 serial → HEAD send → HEAD serial` 순서로 교대했다. 모든 측정은 `flock /tmp/zlink-1639-measure.lock` 안에서 수행했고, 측정 중 이 job의 빌드·테스트는 실행하지 않았다. application diagnostics는 두 버전 모두 WSL 사본에서만 OFF로 맞췄다. 판정 값은 비계측 jar의 process CPU 차이를 완료 메시지 수로 나눈 µs/msg이며, active 직전부터 완료·drain·수신 일치 확인까지를 포함한다. 처리량은 참고 값이다.

| pattern | version | source CPU µs/msg | target CPU µs/msg | msg/s | active 관측 구간 compiler CPU source/target (s) |
|---|---|---:|---:|---:|---:|
| send-saturation | 848ce30 | 30.921 | 24.138 | 82523.8 | 0.310/0.160 |
| send-saturation | revert HEAD | 25.764 | 17.802 | 117064.8 | 0.180/0.150 |
| request-serial | 848ce30 | 176.394 | 175.746 | 3083.8 | 0.220/0.120 |
| request-serial | revert HEAD | 219.338 | 221.615 | 2635.0 | 0.130/0.140 |

send source/target은 기준보다 각각 16.68%/26.25% 낮다. serial은 각각 24.35%/26.10% 높아 수락 상한 네 항목 중 두 항목을 충족하지 못했다. 30초 warmup 뒤에도 compiler CPU는 0이 아니므로 JIT 비용을 제거했다고 주장하지 않는다. 회차 편차도 크며 이 결과만으로 CPU 차이를 특정 함수의 원인으로 확정하지 않았다. 과거 2초 warmup 수치와 이번 수치를 직접 비교하지 않는다.

CPU 12셀은 모두 errors 0, completed=received였다. 원시 회차와 thread별 CPU는 `runs-cpu/`, 중앙값·행별 값은 `logs/cpu-summary.json`에 보존했다. JDK는 `/usr/lib/jvm/jdk-25.0.4.1+1`, 전후 동일 binding `zlink-1.19.0.jar` SHA256은 `aa68fdb1c91dea248d4d2d1a497d916f945fb786fefc083d3d582addd9c79c3b`다.

## 메시지당 counter

lane·할당 계측은 WSL 사본에만 넣었으며 커밋하지 않았다. 별도 serial 실행에서 source/target의 JFR를 수집했다. Java heap 할당은 `-XX:-UseTLAB`과 `jdk.ObjectAllocationOutsideTLAB` 전체 이벤트로 셌다. 아래 stage 수는 실제 benchmark classpath에서 `CompletionStage`를 구현하는 타입을 판별하므로 CompletableFuture 하위 타입도 포함한다. 초기 base CompletableFuture 집계 약 24개는 하위 타입을 제외한 부분 집계였으며, 아래가 최종 전체 stage 집계다. CF 내부 helper 수는 `CompletableFuture$*` 중 MinimalStage를 제외한 객체 수다.

| role/version | lane 진입/msg | queued/msg | heap 객체/msg | heap byte/msg | future/stage/msg | CF 내부 helper/msg |
|---|---:|---:|---:|---:|---:|---:|
| source/848ce30 | 4.015 | 0.0050 | 275.251 | 13527.302 | 27.031 | 16.019 |
| source/HEAD | 5.012 | 0.0020 | 274.814 | 13557.875 | 26.028 | 16.015 |
| target/848ce30 | 8.020 | 0.0061 | 362.606 | 18544.665 | 16.007 | 2.006 |
| target/HEAD | 8.016 | 0.0000 | 362.006 | 18608.102 | 14.988 | 2.000 |

heap 집계에는 binding·Framework·benchmark·HTTP metrics가 포함되며 native heap은 포함되지 않는다. LaneTurn/JFR 진단 객체는 제외했지만 계측이 JIT·escape analysis에 줄 수 있는 영향은 남는다. 따라서 이 heap 값으로 비계측 CPU 증가를 직접 환산하지 않았다. 모든 JFR의 DataLoss는 0이고 4개 할당 진단 셀의 완료·수신 수가 같고 오류는 0이었다.

추가 hand-off 진단은 source/target JFR를 같은 serial 셀에서 함께 수집했다. Thread.threadId가 producer와 execution 시점에 달라진 횟수이며 OS context switch 수가 아니다. 관측 경계는 lane 실행, binding reply 관찰, service completion dispatcher, serial drain/worker다. Core 내부의 모든 thread 이동과 benchmark caller의 재개까지 포함한 수치는 아니다.

| role/version | binding reply/msg | completion dispatcher/msg | serial drain/msg | serial worker/msg | lane 실행 thread 변경/msg | 관측 hand-off 합계/msg |
|---|---:|---:|---:|---:|---:|---:|
| source/848ce30 | 0.9999 | 1.0000 | 0.0000 | 0.0000 | 0.0034 | 2.0033 |
| source/HEAD | 1.0000 | 1.0000 | 0.0000 | 0.0000 | 0.0007 | 2.0007 |
| target/848ce30 | 0.0000 | 0.0000 | 1.0000 | 1.0000 | 0.0023 | 2.0023 |
| target/HEAD | 0.0000 | 0.0000 | 1.0000 | 1.0000 | 0.0004 | 2.0004 |

handoff 2셀도 errors 0·completed=received·DataLoss 0이다. source의 추가 lane 진입 1회는 대부분 inline이었다. stage 생성과 관측 hand-off는 증가하지 않았으므로, 추가 future나 thread 인계가 serial 회귀의 주원인이라는 가설은 이 계측으로 지지되지 않는다.

## 추가 비용 위치와 구현 판단

아래 순위는 코드 추가와 counter 차이가 확인된 위치의 조사 우선순위다. 함수별 CPU µs/msg 순위는 확정하지 못했다. 2단계의 CPU 귀속은 미완료이며, 이 표를 원인 확정이나 3단계 최적화 완료로 제시하지 않는다.

| 순위 | 현재 HEAD file:line | 확인한 추가 작업 | 판단 |
|---|---|---|---|
| 1 | `channels/ZLinkChannelSocketRegistry.java:598`, selector `:628` | source selector 1회 뒤 node operation turn 1회; 기준 대비 lane 진입 +1/msg | source port·topology·liveness를 node별로 격리하는 E2/E3 경계다. owner를 context 단위로 다시 합치면 격리 실패를 복원한다. 약 0.002/msg 이하의 lane thread 인계로 serial CPU +43 µs/msg를 설명할 근거는 없다. |
| 2 | `internal/service/ZLinkServiceM6AWireCodec.java:348` | source native application frame의 mutableDataBuffer 경로에서 DirectByteBuffer 1개와 NativeMemorySegmentImpl 1개/msg가 기준보다 더 materialize됨 | codec 소스 자체는 기준과 같으며 JIT/계측 영향과 runtime 비용을 분리하지 못했다. 전체 source heap 객체는 감소하고 byte는 약 30.6/msg 증가했다. CPU 원인으로 확정하지 않았다. |

추가되지 않은 잔여 경로: `binding/ZLinkJavaRawServicePort.java:478`의 target turn 약 4/msg, `binding/ZLinkJavaRawMeshNode.java:4634,4776,4656`의 claim·topology 조회·다음 readiness 확인, `internal/service/ZLinkServiceLivenessRegistry.java:51`의 관리 진입 약 1/msg, `internal/handlers/ZLinkHandlerInstanceOwner.java:86`의 target owner 진입 2/msg. 이들은 기준에도 존재하며 0f2e5a의 추가 비용으로 합산하지 않았다.

비교한 대안: (1) registry와 node owner를 다시 공유하는 방식은 직접 재현한 node 격리 결함 때문에 제외했다. (2) ingress·descriptor·Spot·completion을 owner 하나에 다시 합치는 방식은 사용자에게 거절된 40e6578의 변경을 복원하므로 적용하지 않았다. (3) C2 선택 상태의 snapshot/우회 조회나 caller가 queued work를 직접 drain하는 방식은 현재 counter로 CPU 원인임을 입증하지 못했고 상태·실행 규칙 변경으로 성능 실패를 숨기지 않기 위해 구현하지 않았다.

## 결정 소유와 BLOCKERS

아래 경로는 모두 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/` 기준이다.

| 결정 | spec 소유 조항 | 현재 Java 판정 위치 | 한 곳의 소유자 |
|---|---|---|---|
| source 선택·등록·최초 제출 | 08-messaging-hot-path §3 E2/E3/E4, 06-state-ownership-and-lanes §2/§4 | registry `:628` selector, `:598` node turn; RawSpotNode `:303` 이후 → RawMeshNode `:306` → RawServicePort `:478`의 동일 node turn 재사용 | selector는 registry, operation은 해당 node transport owner |
| application worker 실행 | 08-messaging-hot-path §4.1 I4, 02-handler-turn-and-execution-gate §2 | `execution/ZLinkSerialExecutionQueue.java:1282`의 drain dispatcher와 `:1294`의 worker 제출; shared Spot 경로 `:227,:230`도 같은 두 단계 | application execution gate가 권한을 정하며 필요한 worker 진입은 한 번 |
| durable terminal의 caller continuation 게시 | 06-state-ownership-and-lanes 「완료 신호와 블로킹 호환 경계」, 08-messaging-hot-path §3 E5 | `binding/ZLinkJavaDurableRequest.java:120`은 completion을 반환하고 `:125,:191`에서 owner scope 안의 completion.complete가 등록된 dependent를 실행 | terminal 결과 소유자와 caller continuation 게시 경계를 분리하며, caller는 owner scope가 끝난 새 turn에서 실행 |

BLOCKERS B1 — `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:1282,1294`: 두 버전 모두 target의 claim 이후 handler 진입에 drain dispatcher와 worker의 두 실행 자원 전환이 있다(각 1/msg). 08 §4.1 I4가 허용한 worker 전환 하나와 다르다. 0f2e5a가 새로 만든 차이는 아니지만 현재 경로에 남은 닫힌 계약의 구현 결함(B)이며, 사용자 지시대로 해당 부분은 수정하지 않았다.

BLOCKERS B2 — `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java:120,191`: durable completion에 미리 붙인 continuation이 owner turn 위에서 실행된다. 40e6578에 있던 regression fixture를 WSL에서만 복원해 확인했다. main에는 awaitTurn이 없어 공통 runNowOrQueue 진입으로 맞췄으며 guard·assertion은 바꾸지 않았다. 최종 동일 fixture는 main/branch 각각 1/1 실패했고 assertion은 owner.isOnLane()==false가 실제 true라는 것이었다. settle(:191) → completion.complete → registered continuation의 stack을 XML로 보존했다. 닫힌 완료 게시 규칙과 다른 기존 결함(B)이며 해당 부분은 수정하지 않았다. fixture는 Windows 소스에 추가하거나 커밋하지 않았다.

소유 계층: Framework의 node transport owner가 node port·topology·liveness를 소유하고 registry는 source 선택을 소유한다. handler execution 권한은 기존 gate가 소유한다. Core/binding의 선택·retry·errno·HWM·reconnect 결정을 추가로 구현하지 않았다.
spec 조항: 06-state-ownership-and-lanes §2/§4/완료 신호 경계, 08-messaging-hot-path §3 E2~E5·§4.1 I4, 02-handler-turn-and-execution-gate §2. 보호된 spec·e2e·sample·internals 변경 0.
교차언어 대조: C++ `raw_mesh_node_owner.hpp:467–480`의 node별 lane·port·poller·topology·liveness, .NET `ZLinkManagedMeshNode.cs:66–68`의 node별 lane, Node `node-raw-mesh-backend.ts:385`의 node별 RawServiceMeshRuntime을 확인했다. Kotlin은 같은 Java backend를 사용한다. 이 대조는 node 격리 소유를 확인한 것이며 B1/B2의 다른 언어 경로 전체를 검증했다는 뜻은 아니다. 다른 언어 source 수정·cross-language contract 전체 실행은 하지 않았다.
변경 분류: B — 승인된 비용 회귀 변경의 revert만 커밋했다. B1/B2는 구현 미해결로 보고했다. 새 public API·언어 예외·스펙 개정은 없다.

## 테스트와 남은 실패

격리 재현: 같은 `ZLinkJavaMeshNodeIsolationTest`를 848ce30 사본에 넣으면 B 왕복의 `CompletableFuture.allOf(...).get`에서 TimeoutException(:128)으로 1/1 실패했다. revert HEAD는 1/1 통과했다. A의 held turn·send pending 및 B의 왕복 진행 assertion을 유지했다.

전체 명령은 WSL에서 `gradlew --no-daemon --max-workers=2 --continue -Pzlink.includeSamples=false test contractTest fakeBackendTest integrationTest`이며 main·branch 각각 한 번 실행했다.

| label (Java/Kotlin 포함) | main tests/실패/skip | branch tests/실패/skip |
|---|---:|---:|
| test | 2921/0/19 | 2831/0/19 |
| contractTest | 91/0/0 | 91/0/0 |
| fakeBackendTest | 3/0/0 | 3/0/0 |
| integrationTest | 74/0/7 | 74/0/7 |

main 6분 9초, branch 6분 35초; 전체 label의 main 대비 새 실패 0. 두 버전 모두 Java/Kotlin 전체 format(`scripts/format/format.sh --check java`, google-java-format와 ktfmt), Java/Kotlin API snapshot, public contract module boundary가 통과했다. 전체 label XML과 합계는 추가 집중 실행 전에 별도 tar/JSON으로 보존했다.

추가 durable contract 재현은 main/branch 공통 1/1 실패(B2)다. 이것은 전체 label의 통과로 해당 계약을 보장할 수 없다는 검사 누락의 증거이며, B2를 수정 완료나 전체 수락 완료로 선언하지 않는다. 처음 main fixture에 branch 전용 awaitTurn을 그대로 쓴 compile 실패는 로그로 보존했고, 공통 API로 입력을 맞춘 뒤 실제 assertion 실패를 대조했다. 검사 기준은 완화하지 않았다.

이번 job에서 SpotManagerTest의 manual replacement 간헐 실패는 발생하지 않았다. 이전 m1673c 이력의 원인이 해소됐다고 판정하지 않는다. 현재 관련 선택 위치는 RawMeshNode.java:1195–1203이다.

남은 실패: serial CPU 상한 source/target 미충족; B1의 두 worker 전환; B2의 main/branch 공통 durable continuation 실패. 비계측 serial CPU의 함수별 귀속은 미완료다. 3단계 추가 비용 제거와 목표 달성은 미완료 상태로 중단했다.

## 변경 파일과 판정 위치

변경은 승인된 revert의 16파일(227행 추가·433행 삭제)뿐이다. Kotlin source 자체의 변경은 없으며 Kotlin 검증은 포함했다. 전체 경로는 `q1673d/changed-files.txt`에 보존했다.

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaInstanceSpotRegistry.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/ZLinkCompletionBridge.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceLivenessRegistry.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceTopologyRegistry.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequestOwnerTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaMeshNodeIsolationTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeAdmissionSourceTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeDescriptorOrderTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaSourceTransportOwnerTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkChannelSubmitTurnTest.java`

수정 전/후 판정 위치 수: 거절된 40e6578의 소유 통합 대상은 이전 보고서 기준 4→11로 복원했다(승인된 revert). node 주변 state owner는 1→5, FromOwner helper는 2→4, 완료 게시 규칙 소유는 1→2로 복원된다. revert 후 진단 기준과 최종 소스는 같으므로 3단계 판정 위치는 11→11이며 감소하는 최적화를 달성하지 못했다. 소유 통합의 감소를 이 job 성과로 계산하지 않는다.

상태·queue·flag: 기존 StateLane 필드 4개가 복원됐고 각 lane의 기존 mailbox 1개·atomic 2개·close future 1개가 함께 복원된다. 40e의 추가 public terminal future 1개와 게시 helper는 제거됐다. 이 복원은 명시적으로 승인된 revert의 일부이며, 그 밖에 추가한 production 상태·flag·lock·queue·timer·retry는 0이다. persistent generation이나 filter flag를 새로 만들지 않았다.

## 산출물과 정리

`D:/t/jobs/fx/cx/q1673d/`에 로그·원시 결과·JFR·counter·reproducer를 보존했다. 로그/측정 파일 237개, 153,974,196 bytes를 원본과 크기·SHA256으로 대조했다(`archive-manifest.json`). WSL 계측 source와 script는 `wsl-assets/`, 원본 source는 head/reference/main tar에 보존했다. 보존 시 이 사본을 사용하는 Java·Node·Python 프로세스 0개를 확인했다. WSL 사본은 삭제했고 cleanup.json에 결과를 기록했다. Windows worktree는 clean이고 `git diff 0f2e5a176b HEAD`는 비어 있다.

리팩토링 점검: 성능 1건·POSDDD 2건·불필요 코드 0건 발견, 고친 항목은 승인된 비용 회귀 커밋 revert이며 추가 runtime 근본 수정은 없음, 넘긴 항목은 B1(ZLinkSerialExecutionQueue.java:1282,1294)·B2(ZLinkJavaDurableRequest.java:120,191); serial CPU 귀속·수락은 미완료.

