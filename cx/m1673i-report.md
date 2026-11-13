# #1673 Java 메시지당 비용 2차 후속

대상: `/root/worktree/zlink-1673-java-2`, `framework-java/1673-per-message-cost-2`. 시작 HEAD `75d484921e`. Push·branch 전환·rebase·main 변경·다른 프로세스 kill 없음. 스펙 파일 변경 없음.

1번 lane 경량화는 `88e6dafa23`으로 커밋했다(본문 끝 `Refs #1673`). 2번 Spring은 공개 metadata만으로 일반 Spring context에서 안전하게 생략하는 조건을 확정할 수 없어 시안을 제거했다. 3번 S1은 아래 설계에서 멈췄고 코드는 바꾸지 않았다.

## 1. Lane primitive — B

변경: `ZLinkStateLane.java`, `ZLinkStateLaneTest.java`, `ZLinkChannelSubmitTurnTest.java`, `ZLinkNodeSubmitTurnTest.java`.

- `scheduled`·`admission`의 두 AtomicInteger를 volatile int + VarHandle CAS/RMW로 바꿨다. Admission의 sign bit와 publishing count, drain claim/release 순서는 그대로다.
- mailbox는 최초 queued turn에서 CAS로 한 번 설치하고, close future는 최초 close에서 CAS로 설치한다. Close future를 먼저 설치하고 admission을 seal하므로 기존 `completeCloseIfDrained` 경로가 future 설치를 놓치지 않는다.
- 기존 executor·FIFO·batch 크기·비재진입·lane 밖 publication·close 대기 규칙은 바꾸지 않았다. HandlerInstanceOwner 자체는 바꾸지 않았다.
- 새 시험은 barrier에서 시작한 16 producer의 첫 제출을 겹치고, turn 배타·모든 turn의 정확히 한 번 실행·close drain·close 후 admission 거부를 확인한다. Sleep 없음.
- 기존 submit-turn 시험 helper 두 곳은 `mailbox`가 생성 시점부터 non-null이라는 구현 가정을 갖고 있었다. 기존 전체 core 실행에서 세 시험이 NPE로 실패했다. Helper가 같은 시간 상한 안에서 mailbox의 생성과 enqueue를 함께 기다리게 했다. 기존 `assertFalse(mailbox.isEmpty())`와 submission/replacement 동작 assertion은 그대로다. Admission이 publish 중인 상황을 시험하는 기존 StateLaneTest의 mailbox 삽입은 그대로 통과 대상이다.

소유 계층: Framework state-lane primitive 하나가 component mutable state의 FIFO·turn 배타·admission seal을 소유한다.
스펙: `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:98` C2, `:163–170` turn 배타/FIFO/비재진입, `:209–223` scope 밖 completion, `:352–363` 공통 close 계약. Hot path `08-messaging-hot-path.ko.md:244–251` scope 저장소 수명·별도 실행 자원/대기 금지.
교차언어: .NET `Runtime/Execution/ZLinkStateLane.cs:39–51`도 mailbox·close completion 지연 생성과 primitive int를 쓴다. C++ `runtime/execution/state_lane.hpp:168,293–296,349`는 FIFO single-turn/close와 atomic primitive를 소유한다. Node `runtime/execution/state-lane.ts:29–33,90–99`는 event loop의 mailbox/draining/closed와 dispose가 같은 의미를 소유한다. Java의 저장 표현만 바꾸며 언어별 공개 동작 차이를 도입하지 않는다.
분류: B(감독자 2차 리뷰에서 승인한 primitive 비용 수정).
수정 전/후 규칙 수(판정 위치): turn claim CAS(`tryClaimInlineTurn`, `scheduleDrain`) 2→2, 제출 수락(`tryAdmit`) 1→1, close drained(`completeCloseIfDrained`) 1→1, 합계 4→4. 정책 소유자는 state lane 하나 그대로다. 새 lock·queue·정책 flag 없음. AtomicInteger 두 객체를 필드로 바꾸고 기존 mailbox/future의 생성 시점만 늦췄다.

할당 계측은 시간 측정 없이 JFR `jdk.ObjectAllocationOutsideTLAB` 횟수만 셌다. `/root/jobs/fx/cx/m1673i/AllocationProbe.java`가 같은 JVM에서 10,000회 준비 뒤 5,000회를 기록한다. `-XX:-UseTLAB`, stackdepth=96. Baseline은 HEAD의 lane·Spring factory 소스만 별도 디렉터리에 컴파일해 classpath 앞에 두었다. Worktree 전환 없음. 집계는 인계 harness의 `m1673g/harness/jfrcount/JfrCount.java` 그대로 사용했다.

| 대상 | baseline | 변경 후 | 차이 |
|---|---:|---:|---:|
| lane 자체+생성자 귀속 객체 / 생성 | 6.000 | 1.000 | −5.000 |
| 생성자 귀속 AtomicInteger | 2.000 | 0 | −2.000 |
| ConcurrentLinkedQueue + 초기 node | 2.000 | 0 | −2.000 |
| close future | 1.000 | 0 | −1.000 |

원문: `m1673i/baseline-lane-counts.txt`, `changed-lane-counts.txt`, JFR 두 파일. 전체 probe 합계 6.058→1.058에는 JFR 종료 등의 약 0.058 객체가 섞여 있어 lane 귀속과 구분했다. Dispatch에서 남는 lane+instance/close lambda는 인계의 8→3 obj 계산과 일치한다. 이는 primitive 생성 계측이며 request-serial/send-saturation 전체 셀을 다시 잰 값은 아니다.

## 2. Spring — BLOCKER, 변경·커밋 없음

캐시는 있다: `ZLinkSpringHandlerFactory.handlerPlans`와 `HandlerPlan`(`:83`, `:452` 부근). 따라서 새 cache는 만들지 않았다.

설치된 Spring은 6.2.18이다. `javap`로 확인한 API 제약:

- `AutowiredAnnotationBeanPostProcessor`의 InjectionMetadata 조회/build 메서드는 public API에 없다. `InjectionMetadata` 자체의 public 접근자는 이미 얻은 metadata를 읽는 API다.
- `DisposableBeanAdapter.hasDestroyMethod` 메서드는 public이지만 **DisposableBeanAdapter 클래스가 package-private**이므로 Framework에서 public API로 호출할 수 없다.
- 대안으로 public `MergedBeanDefinitionPostProcessor.postProcessMergedBeanDefinition`이 채운 `RootBeanDefinition.getExternallyManagedConfigMembers()/getExternallyManagedDestroyMethods()`를 기존 HandlerPlan에 넣는 시안을 만들었다. Spring의 정확한 Autowired/CommonAnnotation processor 두 개에 대해서는 annotation을 수동 해석하지 않고 injection/lifecycle 존재를 알 수 있었다.
- 그러나 기본 AnnotationConfigApplicationContext에도 `ConfigurationClassPostProcessor$ImportAwareBeanPostProcessor`(InstantiationAware)와 `ApplicationListenerDetector`(DestructionAware)가 있다. 이들은 instance별 동작을 소유하고, 위 metadata에 클래스별 생략 가능 여부를 제공하지 않는다. 일반 custom post-processor 역시 instance별 주입·destroy를 수행할 수 있다. 공개 metadata가 비었다는 사실만으로 autowireBean/destroyBean 전체 호출을 생략하면 이 동작을 잃는다.
- 이 processor들을 만나면 기존 호출을 유지하는 안전한 시안으로 계측한 결과, plain handler의 autowire 귀속 14.000→14.000, destroy 귀속 3.000→3.000이었다. 따라서 요청한 −14/−3 절감을 달성하지 못했다. 비공개 클래스 이름 분기, 수동 annotation 스캔 또는 processor 실행을 직접 재구현하는 우회는 넣지 않았다.

근거: `m1673i/spring-public-api.txt`(javap public API), `m1673i/spring-processors.txt`(실제 기본 processor 목록), `baseline-spring-counts.txt`, `changed-spring-counts.txt`, `spring-attempt.diff`(제거한 시안과 lifecycle 시험). 시안은 전부 원래 내용으로 되돌렸고 build.gradle에도 잔여 변경이 없다. Lifecycle 동작 동등성 시험은 채택된 변경에 대해 완료하지 않았으므로 이 항목을 완료로 보고하지 않는다.

감독자 결정 필요: (a) Spring public API가 제공하는 더 좁은 주입/종료 metadata 접근 경로를 지정하거나, (b) Framework가 지원하는 processor 계약을 먼저 정해야 한다. 임의 custom processor를 무시하는 최적화는 제안하지 않는다. Common API `00-foundation/06-framework-api.ko.md:485–489,500–534`의 scope/종료 책임은 유지해야 한다.

## 3. S1 — 코드 변경 전 설계 보고

기준은 **origin/spec/1639-hot-path-cost**의 `01-submit-and-completion.ko.md` §10(추가 문단), §7·§11·§15다. 현재 checkout §10에는 S1 문단이 아직 없다. 스펙은 고치지 않았다.

### 결정 소유 표

| 결정 | 스펙 소유 | Java 현재 | C++ 현재 | .NET 현재 | Node 현재 | 하나로 둘 소유자 |
|---|---|---|---|---|---|---|
| 즉시 admission은 pending entry 없이 완료, BACKPRESSURED만 등록 | §10 S1, §15 | `runtime/binding/ZLinkJavaSocketSupport.java:69–71,124–129`: BACKPRESSURED는 binding stage를 바로 반환 | `runtime/mesh/raw_mesh_node_owner.cpp:1231–1251`: immediate 분기 뒤 send 등록 | `Runtime/Service/ZLinkManagedMeshNode.cs:9975–9991`: submit 전 PendingSend 등록(즉시 경로도 등록하는 옛 규칙) | `runtime/backend/node/node-raw-mesh-backend.ts:2022`: STREAM send는 admitted 직접 await; 전체 send parity 감사는 후속 필요 | Binding 결과 판정은 SocketSupport, pending 수명은 해당 runtime/adapter의 기존 operation registry |
| 완료 권한을 atomic take로 획득 | §10 | `internal/service/ZLinkServiceOperationRegistry.java:149–169,227–239`: request는 take, send는 우회 | 위 C++ `:1279–1287`, `operation_registry.cpp`의 complete/fail | `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:185–214`: CompleteSend→TryTake→Post | `runtime/foundation/operation-registry.ts:77–85` take 기반 registry; 위 STREAM 경로는 우회 | 같은 registry의 take 한 곳 |
| send에 timeout·caller cancellation 없음 | §7 | registry `:285–309,318–335,425–437,469–482`는 모든 entry에 deadline과 cancel 정책 적용 | send 등록 deadline은 time_point::max, shutdown/transport terminal | `PendingSend` `:275–324`에는 request cancellation 경로 없음 | 위 registry는 deadline 소유 선택 가능하지만 send는 별도 직접 await | registry의 typed send entry 정책 한 곳; binding 대기를 취소/재제출하지 않음 |
| shutdown/close와 binding terminal 경쟁 | §7·§10 | RawMeshNode `:4503–4539`에서 operations.close 후 port.close; 미등록 send는 이 경쟁에 없음 | RawMeshNode close `:612–627`에서 operations.shutdown 후 port 정리 | MeshCompletionTable `:193–207` FailAll로 detach→Post; MeshDispatchPump `:803–806`에서 FailAll→CompletionDrained | registry close `:103`는 등록 항목만 종료 | registry close와 terminal take의 동일 gate |
| caller continuation은 새 execution turn | §11 | service dispatcher `:68–107`는 별도 process worker; SocketSupport pending stage 직접 반환은 이 경로를 안 씀 | registry terminal dispatch 경로 | CompleteSend/FailAll→ZLinkCompletionDispatcher.Post | 기존 Promise 경로와 §11의 lane-current 경계는 후속 설계 시 함께 확인 | 기존 process completion dispatcher |

분류: Java pending binding send의 registry 미등록은 B(S1 계약 반영 결함). 즉시 send의 미등록은 새 S1에서 올바른 동작이다. .NET의 즉시 send 등록 제거는 별도 언어 변경 담당과 함께 맞춰야 한다. Node는 위에 확인한 STREAM 경로만 확정 근거이며 전체 Node send에 일반화하지 않는다.

**지목된 Java RawMeshNode:3073은 구분해야 한다.** 이 메서드는 `dispatchLocalInstanceSpot`이다. `request`일 때만 registry를 만들고, send는 `onInstanceSpotMessage`의 동기 acceptance가 true일 때 `:3152`에서 이미 완료된 stage를 돌려준다. 이 경로에는 binding BACKPRESSURED가 없다. 따라서 :3073을 모든 send 등록으로 바꾸면 즉시 send를 등록하지 말라는 S1에 오히려 어긋난다. 실제 BP 누락은 SocketSupport→RawServicePort(`:130–154`)→RawMeshNode의 `port.send/sendMessages` 경로다.

### 안 1 — 기존 registry에 typed send entry 추가 (권고, 아직 미승인)

- 기존 `ZLinkServiceOperationRegistry`의 bucket/slot·active ownership·atomic take·intrusive dispatcher node를 재사용한다. 병렬 send map, done flag, 두 번째 completion queue는 만들지 않는다.
- 요청 entry와 구분되는 **send terminal entry (`Void`)**를 둔다. Request만 deadline/cancel 입력을 받는다. Send entry는 deadline maintenance에서 제외하고, caller cancellation을 registry 또는 binding에 전달하지 않는다. `Long.MAX_VALUE` timeout을 요청 API에 넣어 무한 대기를 흉내 내지 않는다. Entry 정책의 구현 형태(typed subtype 또는 닫힌 kind)는 구현 브리프에서 확정한다.
- SocketSupport는 binding `SubmitResult.OK`를 기존 immediate marker로 반환한다. BACKPRESSURED에서만 그 runtime의 registry에 entry+dispatcher node를 등록하고, 등록이 끝난 뒤 binding의 admitted stage에 terminal observer를 단다. Stage가 이미 끝났어도 entry 등록이 먼저이므로 결과를 잃지 않는다. 초기 동기 거절(대기 토큰 없는 BACKPRESSURED 포함)은 pending entry가 없다. Java binding의 `contracts/messaging/SendSubmission.java:9–14`는 반환 snapshot을 OK/BACKPRESSURED로 제한하고, `runtime/sockets/CompletionOwner.java:122–125,153–154`는 writable wait가 아닌 실패를 throw하므로 snapshot BP와 초기 throw를 구분할 수 있다. 배포 binding 버전에서도 이 계약을 먼저 확인한다.
- **submit→pending 등록 사이에 close가 끼지 않게** 기존 socket/lifecycle 소유 turn의 범위 안에서 두 작업을 마친다. Close가 먼저 이겼으면 새 entry를 받지 않고 ShuttingDown을 확정한다. Binding terminal은 오직 registry take를 시도하고, close가 가져간 항목에 뒤늦게 온 terminal은 아무것도 하지 않는다.
- Mesh의 기존 operations를 재사용하고 RawServicePort가 pending 등록을 그 소유자에게 위임하도록 internal 배선을 확정해야 한다. Dealer/Router 같은 독립 backend socket은 자기 lifecycle의 registry를 쓴다. 같은 send를 port registry와 mesh registry에 이중 등록하지 않는다. Public backend API를 새로 만들지 않고 SocketSupport의 internal 배선으로 한다.
- close는 현재 seal→pump stop→registry detach/종료 게시→port close 순서를 기본으로 유지한다. Send entry도 같은 detach에 포함하며 ShuttingDown으로 끝낸다. Route loss는 binding terminal의 Unavailable을 관찰한다. Framework retry·추가 poller·timer는 없다.
- caller future는 registry dispatcher가 완료한다. 등록/terminal 판정의 gate 및 state-lane CURRENT를 caller에게 넘기지 않는다. 다른 실행 자원에서 독립 turn이면 source stack의 전역 반환 순서는 요구하지 않는다(§11). 즉시 admission에도 public continuation을 게시하는 기존 경계가 §11을 만족하는지 함께 시험한다. `whenComplete` 안에서 caller future를 직접 complete하는 경로를 남기지 않는다.
- 종료 시 application callback의 반환을 infrastructure turn 안에서 기다리지 않는다. Dispatcher에 terminal을 게시하는 것과 application 작업의 drain을 구분한다. .NET의 CompletionDrained와 Java close의 공개 반환 조건은 해당 lifecycle 스펙과 대조한 뒤 구현 브리프에 명시해야 한다.

안 1의 수정 전/후 판정 위치 목표: pending terminal 소유가 binding future와 runtime registry로 나뉜 상태에서 registry take 1곳으로 통합. Binding은 물리 admission 결과만 계속 소유한다. Send/request별 입력 차이는 §7이 정한 계약이며 runtime별 별도 정책 사본을 만들지 않는다.

### 안 2 — backend 공통 pending-send registry를 수명 소유자로 두기

SocketSupport를 쓰는 RawServicePort·Dealer·Router의 소켓 lifecycle에서 pending send를 전부 등록하고, Mesh는 그 terminal을 관찰만 한다. 모든 send entry와 request entry는 동일 registry primitive/dispatcher를 쓰되 각 소켓의 수명으로 분리한다. Register/terminal/close는 안 1과 같은 take 규칙을 쓴다.

장점: SocketSupport 한 경계에서 모든 binding BP send를 다룬다. 단점: Mesh의 기존 request registry와 socket registry의 종료 순서·소유 경계를 더 많이 설명해야 한다. STREAM reply·local acceptance 등 socket send 외 표면도 조사해야 한다. 기존 Mesh registry를 직접 재사용하는 안 1보다 수명 배선 변경이 크므로 권고하지 않는다. 두 안을 동시에 넣거나 같은 send를 두 registry에 등록하지 않는다.

### S1 구현 승인 뒤 필요한 재현/검증

1. Binding을 BP에 두고 runtime close를 실행하면 send caller가 정확히 한 번 ShuttingDown을 받는다. 현재 구현에서 registry pendingCount가 증가하지 않는 재현을 먼저 고정한다.
2. BP 결과 직후 admitted stage가 이미 완료된 경우, register와 close가 교차하는 경우, close 이후 늦은 admission/failure를 barrier로 각각 재현한다.
3. Request의 timeout·cancel은 기존대로 동작하고 send는 같은 가상 clock/취소 입력에서 끝나지 않는다. Binding admission·route loss·runtime shutdown으로만 끝난다.
4. Immediate OK와 동기 거절은 entry/dispatcher node 등록 0회다. BP는 1회이며 slot reuse 뒤 늦은 이전 terminal이 새 entry를 완료하지 못한다.
5. Completion callback에서 source runtime 재호출이 가능하고, CURRENT가 source lane이 아니며 완료 gate를 상속하지 않는다. Callback이 dispatcher turn에서 실행되는지 확인하되 특정 native completion thread와 같다는 assertion은 만들지 않는다.
6. C++/.NET/Java/Kotlin 공통 contract로 immediate/BP/close/late terminal을 대조한다. Node 발견도 같은 #1673 범위에서 담당자에게 넘긴다. Sleep·시간 성능 계측 없음.

S1은 승인 전이므로 위 설계를 구현하지 않았다.

## 검증 / 커밋

- 관련 시험: StateLane 28·HandlerInstanceOwner 5·ChannelSubmitTurn 6·NodeSubmitTurn 14 = 53, 실패 0. `m1673i/lane-final-focused.log`, `focused-summary.txt`.
- 수정 전 비교/실패 재현: 첫 core 전체 실행 2743개 중 3개 실패(두 reflection helper의 eager mailbox 가정). `first-core-summary.txt`. Helper 수정 후 위 53개로 같은 세 실패가 모두 사라짐을 확인했다.
- 첫 focused 시도는 같은 worktree에서 두 Gradle을 겹쳐 제가 class output 경합을 만들었다. Discovery의 NoClassDefFoundError로 실행되지 않았으며 검증 통과로 세지 않았다(`lane-focused.log`). 이후 Gradle은 하나씩 실행했다.
- Spring 시안의 새 annotation 시험 첫 컴파일은 jakarta.annotation test 의존성 누락으로 실패했다(`spring-focused.log`). 시안과 test 의존성을 모두 제거했으며 Spring 신규 동작 동등성 검증을 완료했다고 주장하지 않는다.
- 최종 전체 Java/Kotlin unit 3206·contract 91, 실패 0·skip 0. 그중 core unit 2743·contract 25다. `m1673i/final-summary.txt`에 module별 XML 집계가 있다.
- 전체 명령: `env -u ZLINK_LIBRARY_PATH ./gradlew --no-daemon --max-workers=2 --continue -Pzlink.includeSamples=false cleanTest cleanContractTest test contractTest`. `full.log`의 이 실행은 Node TypeScript 의존성 부재로 stream 준비 단계에서 실패했으나 다른 unit·contract는 끝까지 실행했다(3004+91개 실패 0). Test/contractTest 결과를 UP-TO-DATE로 재사용한 task는 없다.
- 누락된 stream 의존성을 준비했다. 같은 버전의 기존 HTTP-client npm archive를 `/root/worktree/zlink-1838-node/.artifacts/node-install/npm/`에서 이 worktree의 동일 artifact 위치로 복사하고 `npm ci --no-audit --no-fund`를 실행했다(159 packages, `node-dependencies-final.log`). Package manifest/lock 수정 없음. `:zlink-stream-connector:cleanTest :zlink-stream-connector:test`의 남은 202개를 실행해 실패 0, BUILD SUCCESSFUL(`stream-final.log`). 이미 통과한 전체 test를 반복하지 않았다.
- Node build가 갱신한 Unity `.jspre`는 시작 당시 clean HEAD 내용으로 되돌렸다. 사용자가 가진 기존 변경은 없었다. `generated-unity-cleanup.txt`에 두 SHA-256을 기록했다. 최종 변경은 Java 네 파일뿐이다.
- `scripts/format/format.sh --check java`: 종료 0, `format-final.log`. `git diff --check` 통과.
- Integration/fakeBackend 전용 label은 실행하지 않았다. 변경은 lane의 저장 표현이며 기존 전체 unit·contract와 concurrency 시험으로 검증했다.
- Java API snapshot 통과(종료 0, SHA-256 `c0fef6f02d502620fd0b84559f2be876c361aef5d48d0e8250c537d2963e86f3`, 2968줄), Kotlin API snapshot 통과(종료 0, `6b91bc6d4b4ebf2cccf24cd3243d81a046a4a5a9df941455a1647f363b98808c`, 3590줄). `api-java.log`·`api-kotlin.log` 및 `.exit` 원문.
- 커밋: `88e6dafa23` `java: allocate state lane queue and close completion lazily`. 네 파일, +126/−40. 본문에 근거 스펙 네 조항과 마지막 `Refs #1673`을 넣었다. Push 없음.
- 남은 시험 실패 0. 최종 worktree clean. 작업상 BLOCKER는 Spring의 공개 metadata 제약이고, S1은 설계 승인 대기다. 두 항목을 구현 완료로 세지 않는다.

리팩토링 점검: 성능 1건(lane eager allocation) 수정, POSDDD 0건·불필요 코드 0건 추가 발견. Spring 공개 metadata 제약과 S1 pending-send 수명 결함은 이 보고서로 넘겼으며 임시 구현을 runtime에 남기지 않았다.
