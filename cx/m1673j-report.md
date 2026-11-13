# #1673 Java S1 BP send terminal registry

등급: gpt-6.1-sol medium. 증거 파일은 `/root/jobs/fx/cx/m1673j/` 아래에 있다.

## 결과와 범위

- 기준 HEAD: `88e6dafa23`; 작업 브랜치 `framework-java/1673-per-message-cost-2`.
- 감독자가 승인한 안 1을 구현했다. BP send만 기존 `ZLinkServiceOperationRegistry`에 Void entry와 dispatcher node를 등록한다. Immediate OK와 초기 동기 거절은 등록하지 않는다.
- Mesh의 기존 registry를 RawServicePort에 전달한다. 독립 Dealer/Router는 각 socket lifecycle의 registry를 사용한다. 같은 send를 두 Framework registry에 등록하지 않는다.
- Binding admission·route loss와 runtime/socket close가 같은 `takeLocked`로 경쟁한다. Close가 먼저 꺼낸 send는 ShuttingDown으로 끝나고, 늦은 binding terminal은 무시한다.
- Send entry는 deadline 검사를 하지 않고 cancellation owner를 갖지 않는다. Minimal stage가 JVM caller 대기 취소를 send 수명에서 분리한다. Binding 재제출·대기에는 cancellation을 전달하지 않는다.
- BP terminal observer는 entry 등록 후 설치한다. Caller 완료는 기존 process service completion dispatcher에서 실행한다. 새 public API·별도 send queue·동기화 정책·per-send timer·executor·retry는 없다. 독립 socket에는 기존 registry instance를 추가하며 그 기존 pool/gate/maintenance task를 함께 사용한다. `submitSend`는 application public API가 아닌 기존 runtime-internal registry의 메서드다(기존 qualified export는 testkit용이다).
- `dispatchLocalInstanceSpot`은 수정하지 않았다. 스펙 문서·binding·Core·다른 언어 구현·Project 보드는 변경하지 않았고 push하지 않았다.

소유 계층: Binding은 물리 admission/재제출/route loss를 소유하고 Framework operation registry는 논리 terminal 권한과 caller completion을 소유한다.
스펙: **origin/spec/1639-hot-path-cost**의 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §7(242–258), §10(345–358), §11(399–419), §15(470–484); 같은 branch `08-messaging-hot-path.ko.md:96–98` E3/E4/E5. JVM cancellation은 checkout `03-cancellation-and-shutdown.ko.md:30–33`.
교차언어: 아래 표의 C++/.NET/Java/Kotlin 대조. C++/.NET 실행 검증은 이 Java job에서 수행하지 않았다. .NET 즉시 send의 선등록은 인계 당시의 기존 차이이며 별도 담당 범위다.
분류: B — 승인된 S1 계약에 대한 Java pending-send 수명 결함 수정.

## Red 증거와 시험 1–6

실제 baseline runtime에서 새 재현 세 개를 먼저 실행했다. `red-boundaries.log`는 3 tests/3 failures, `red-boundaries.xml`에 첫 실패와 assertion 위치가 있다. 첫 단독 close 재현은 `red-close.log`/`red-close.xml`이다.

| baseline 재현 | 실제 실패 | 수정 뒤 확인 |
|---|---|---|
| ServicePort BP send 후 close, binding terminal은 보류 | close 뒤에도 send가 끝나지 않아 5초 시험 상한에서 TimeoutException | ShuttingDown, late admission 무시 |
| BP send caller future cancellation | binding admission까지 cancelled가 되어 `assertFalse(binding.admission.isDone())` 실패 | caller wait만 취소, binding/entry 유지 |
| Source lane에서 binding admission 완료 | callback이 source lane을 상속해 `assertFalse(lane.isOnLane())` 실패 | source lane 밖에서 callback 실행, source port 재호출 성공 |

요청한 1–6의 검증 범위:

1. `ZLinkJavaSubmissionTest.closingServicePortEndsBackpressuredSendBeforeLateBindingTerminal` 및 `meshAndPortShareExactlyOnePendingSendRegistry`: 실제 service/mesh close와 등록 수 1, ShuttingDown, late terminal 무시. Mesh와 port가 같은 registry 객체임을 확인한다.
2. `onlyBackpressuredSendsRegisterAndRegisterBeforeObservingAdmission`: 이미 완료된 binding stage를 반환하기 전에 pendingCount가 1임을 확인한다. `closeWaitsForSubmitAndRegistrationInTheSocketTurn`: submit 중 gate를 유지한 채 closer를 시작하고, submit→등록 뒤에만 close가 진행함을 확인한다. Registry의 `closeBetweenRegistrationAndObserverWinsAgainstAlreadyCompletedAdmission`: observer 설치 전 close 승리. `closeAndBindingTerminalRaceCompletesEachSendExactlyOnce`: 32회 barrier 경쟁, 성공/route failure와 shutdown 중 하나, callback 1회, pending 0. Sleep 없음.
3. `ZLinkServiceOperationRegistryTest.sendIgnoresRequestDeadlineAndCallerCancellation`: 가상 clock 경계에서 request만 expire, request cancel은 기존 동작, send entry와 binding은 계속 pending, binding admission으로만 완료. Kotlin의 기존 `send coroutine cancellation leaves Java submission pending`도 최종 전체 실행 대상이다.
4. `onlyBackpressuredSendsRegisterAndRegisterBeforeObservingAdmission`: SOCKET/SERVICE 각각 OK 0, token 없는 동기 BP 거절 0, BP 1을 slot generation 합계로 확인한다. Dispatcher 등록은 같은 registerLocked에서 한 번 이뤄진다. `lateSendTerminalCannotCompleteReusedSlot`: discard한 entry의 같은 slot을 다음 send가 재사용한 뒤 이전 binding terminal이 다음 send를 완료하지 못함을 확인한다.
5. `pendingSendCallbackLeavesSourceLaneAndCanReenterRuntime` 및 `sendTerminalCallbackRunsOnDispatcherOutsideRegistryGate`: dispatcher worker, registry gate 미상속, source lane 미상속, runtime 재호출. `standaloneSocketCloseOwnsPendingSendAndDoesNotWaitForCallback`는 Dealer/Router 두 lifecycle에서 callback이 socket monitor를 상속하지 않고, callback 반환을 보류해도 close가 반환함을 확인한다. 특정 native completion thread와의 동일성은 요구하지 않는다.
6. 네 언어를 아래 공통 matrix로 대조했다. Java와 Kotlin 전체 unit/contract 실행 결과는 검증 절에 기록한다. C++/.NET의 runtime 공통 contract를 이 job에서 새로 실행하지는 않았다. 따라서 네 언어 실행 검증 완료로 주장하지 않는다. Node의 기존 STREAM 직접 await 발견도 인계 보고서의 미완료 범위로 유지한다.

후속 registry API/slot/가상 clock 검사는 구현 뒤 추가했다. 따라서 이 검사 전부가 baseline에서 실행된 red였다고 주장하지 않는다. 구현 전의 실행 가능한 red 증거는 위 세 개다.

첫 focused 실행 61개 중 registry 28개와 submission 32개가 통과했고 새 close 시험 하나가 callback의 CompletionException wrapper를 직접 Framework 예외로 비교해 실패했다(`focused-first.xml`). 시험이 기존 public `adaptOneWay`를 거친 오류 kind·원인 동일성을 확인하게 수정했다. 이어진 시험 코드의 private unwrap 호출 컴파일 오류(`focused-final.log`)는 public adapter 사용과 JDK CompletionException cause 검사로 고쳤다. 런타임의 실패 판정·timeout·assertion 조건은 완화하지 않았다. 최종 focused는 65/0/0/0(`focused-corrected.log`, `focused-summary.txt`). 이후 takeLocked 공통화는 최종 전체 rerun 검증에 포함했다.

## 결정 소유 표와 전후 판정 위치

Java 기준 경로는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`다. 수정 전 위치는 `88e6dafa23` 기준, 수정 후 위치는 최종 diff 기준이다.

| 결정 | 스펙 소유 | 수정 전 | 수정 후 / 단일 소유자 |
|---|---|---|---|
| Binding OK/BP 분리, BP만 등록 | §10 S1·§15 | `binding/ZLinkJavaSocketSupport.java:67–71,124–129`는 BP stage 직접 반환 | `binding/ZLinkJavaSocketSupport.java:70–82`: result의 BP 분기에서만 `operations.submitSend` |
| 등록 선행과 pending terminal | §10·E3/E4/E5 | request registry만 등록; send는 binding future를 직접 사용 | `internal/service/ZLinkServiceOperationRegistry.java:96–117`: entry+node 등록 후 admitted supplier/observer 호출 |
| Deadline/cancel 적용 대상 | §7, cancellation §1 | registry의 모든 entry가 request 정책; send는 외부 binding future | registry `:310–312,352–357`: typed Kind.REQUEST만 deadline/cancellation owner 보유. Send caller minimal stage의 취소는 분리된 대기만 종료 |
| 완료 권한과 표 정리 | §10 atomic take | registry takeLocked, cancel, expire, close가 각각 removal/unlink 구현(4곳) | registry `:251–256` takeLocked 1곳; cancel `:268`, expire `:312`, close `:232`가 호출 |
| Submit→등록 대 close | E3/E4·§7 | port turn에 binding submit만 있었고 mesh close의 operations.close는 lane 밖 | port sendMessages의 같은 lane에서 submit+등록, mesh close `binding/ZLinkJavaRawMeshNode.java:4533–4537`가 같은 port lane에서 registry close. 독립 sockets는 기존 synchronized send/close 사용 |
| Mesh pending entry의 lifecycle | §7·§10 | Mesh operations와 BP send 수명 연결 없음 | `binding/ZLinkJavaRawMeshNode.java:186–187,297`; port `:48–49,98–113,531`가 동일 operations 재사용. Port 기본 생성자만 독립 registry 생성 |
| Caller publication | §11 | service dispatcher를 BP send가 거치지 않음 | registry complete/completeExceptionally/close의 기존 dispatcher post 사용. `whenComplete` observer는 complete(id)/completeExceptionally(id)로 take·post만 실행 |

수정 전/후 규칙 수(판정 위치): pending entry를 표와 active list에서 꺼내는 구현 4→1. Binding 결과 OK/BP 판정은 SocketSupport 한 곳 그대로(1→1). Send/request 차이는 §7이 정한 Kind 하나로 표현한다. 별도 send queue·lock 정책·per-send timer·정책 flag 없음. 독립 socket registry의 기존 gate와 lifecycle당 maintenance task는 추가된다. 기존 bucket/slot/active list/dispatcher node를 재사용했고 close의 별도 buckets fill/count/head/tail 초기화와 cancel/expire의 removal/unlink 사본을 삭제했다.

## 교차언어 공통 matrix

| 항목 | C++ | .NET | Java 변경 뒤 | Kotlin |
|---|---|---|---|---|
| Immediate OK | `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1231–1234`에서 등록 전 반환 | `Runtime/Service/ZLinkManagedMeshNode.cs:9975`에서 submit 전에 PendingSend 등록하는 기존 차이 | 등록 0 | Java submission 경로 공유 |
| BP | 위 C++ `:1247–1251` register_operation 후 terminal observer | 위 .NET `:9975–9991` PendingSend/Observe | BP 1, observer 전 등록 | Java submission 경로 공유 |
| Close/late terminal | `_operations->shutdown`와 complete/fail registry 경쟁, `:1279–1287` | `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:185–214` TryTake/Post 및 FailAll | registry takeLocked 한 곳; close winner ShuttingDown, late ignored | 같은 Java terminal |
| Send cancellation/deadline | send deadline max, caller cancellation 없음(인계 표의 기존 구현) | PendingSend `:275–324`에는 request cancellation 없음 | Kind.SEND, deadline/cancel 제외 | `ZLinkOneWayCalls.kt:55–81` 등 cancelOperation=false, 기존 Kotlin cancellation 시험 |

C++의 기존 `tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:868–929`는 pending send의 route removal=Unavailable, shutdown=ShuttingDown을 확인한다. 이번에는 소스 기대치 대조만 했고 C++ 실행 결과로 세지 않았다. .NET의 immediate 선등록 제거와 Node 전체 send parity는 이번 Java 코더 범위를 넘어가므로 인계 상태 그대로 감독자에게 넘긴다. 새로운 Issue·Project 상태는 만들거나 바꾸지 않았다.

배포 binding은 `framework/languages/java/gradle/libs.versions.toml`의 **1.19.1**이다. Gradle cache의 실제 jar에 `javap`를 실행했다(`binding-public.txt`, `binding-completion-owner.txt`). OK는 snapshot, BP는 admitted stage를 제공하고 token 없는 실패는 ZlinkSubmitException으로 throw하는 배포 구현을 사용한다. Binding 소스 의존성이나 binding 수정은 추가하지 않았다.

## 여덟 기준 자체 점검

| 기준 | 점검 결과 |
|---|---|
| 스펙 gap | 승인 branch의 S1·§7·§11·§15 및 E3/E4/E5를 적용. checkout의 이전 E3 문구 대신 승인 branch 기준. 스펙 수정 없음 |
| 불필요한 규칙 | Kind.REQUEST/SEND는 계약의 입력 차이만 표현. 무한 timeout sentinel·send done flag·추가 map/queue 없음 |
| 제어 분산 | removal/unlink 4→takeLocked 1. Mesh/port의 같은 entry owner는 같은 registry 객체 |
| 동기화 | 기존 socket monitor/port lane 재사용. Registry gate 밖에서 observer 설치 및 post. Application callback을 기다리는 close 없음 |
| hot path | OK/초기 거절의 entry/node 등록 0. BP에만 기존 pooled entry/node 등록. 기존 dispatcher 외 hop·native 호출·로그 생성·retry 없음 |
| 리팩토링 잔여 | cancel/expire/close removal 사본을 같은 변경에서 takeLocked로 통합. 이번 범위의 추가 확정 발견 0 |
| 매직 값 | Kind enum 및 기존 SHUTDOWN 상수/오류 projection 재사용. Send 등록의 private timeout 인자 0은 Kind.SEND에서 읽지 않으며 deadline sentinel로 사용하지 않음 |
| 계층 소유 | Core/binding의 admission·route loss 결과만 관찰. 재제출/선택/재연결·두 번째 poller를 추가하지 않음 |

리팩토링 점검: 성능 1건(BP-only 등록 및 즉시 경로 유지)·POSDDD 1건(terminal removal 사본)·불필요 코드 1건(close의 중복 clear/reset) 처리. 신규 잔여 발견 0. 이전 인계의 .NET/Node parity와 Spring 공개 metadata 제약은 이 작업에서 해결했다고 주장하지 않는다.

## 검증·커밋

- 전체 명령: `cd framework/languages/java && env -u ZLINK_LIBRARY_PATH ./gradlew --no-daemon --max-workers=2 --continue --rerun-tasks -Pzlink.includeSamples=false test contractTest`.
- `full.log`: BUILD SUCCESSFUL, 65 actionable tasks: 65 executed. Unit/contract task의 UP-TO-DATE/FROM-CACHE 재사용 없음. XML 합계는 `full-summary.txt`: unit **3204/0/0/0**, contract **91/0/0/0**(tests/failures/errors/skipped). Core unit 2741, contract 25; Kotlin unit 82, contract 20; stream unit 202. 전체 새 실패 0.
- Focused 65/0/0/0. 최종 전체 실행에는 takeLocked 공통화 뒤의 소스와 새 concurrency 시험 전부가 포함됐다.
- `scripts/format/format.sh --check java`: exit 0, `format-check.log`. `git diff --check`: 통과.
- Java API snapshot: exit 0, `api-java.log`, sha256 `c0fef6f02d502620fd0b84559f2be876c361aef5d48d0e8250c537d2963e86f3`, 2968줄. Application public API 변화 없음.
- Kotlin API snapshot: exit 0, `api-kotlin.log`, sha256 `6b91bc6d4b4ebf2cccf24cd3243d81a046a4a5a9df941455a1647f363b98808c`, 3590줄. Application public API 변화 없음.
- Unit/contract에서 실제 배포 native binding을 포함한 기존 시험을 실행했다. Integration/fakeBackend 전용 label과 C++/.NET 실행 검증은 실행하지 않았다.
- Build가 다시 생성한 Unity `.jspre`는 시작 당시 clean HEAD 내용으로 되돌렸다(`generated-cleanup.txt`의 SHA-256). 생성된 Kotlin session marker는 build 종료 시 사라졌다. 다른 프로세스를 종료하지 않았다.
- 커밋: `a3def38cff` — `java: register backpressured sends with the operation owner` (8 files, +558/−31), 본문 마지막 `Refs #1673`. Push 없음. Project 보드 변경 없음.
- 남은 Java 시험 실패 0. 남은 검증 범위는 C++/.NET 실행 기반의 네 언어 공통 contract 대조(인계 6)다. .NET immediate 선등록과 Node 전체 send 감사는 기존 담당 범위로 넘긴다.

변경 파일(모두 `framework/languages/java/zlink-framework-core/src/` 아래):

- `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketSupport.java`
- `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java`
- `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java`
- `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java`
- `main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java`
- `main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java`
- `test/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubmissionTest.java`
- `test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistryTest.java`

