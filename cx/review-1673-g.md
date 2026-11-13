# #1673 Java 리뷰 (감독자, 2026-10-15) — m1673g 커밋 10개 + merge 7c240c9672 해결분 (a2e5bbce5f..0db1cc211f, main 커밋 09da8bbc52·54d52a24cf·c62f614da9 제외)
판정: 승인 조건부(J-F1·J-F2 수정 뒤). 스펙 gap 0.
- 6e7af1ebf1 J2 승인: SpringActivation의 synchronized 제거 — ZLinkHandlerInstanceOwner가 openActivation/create/destroy/close를 모두 같은 state lane(awaitTurn)에서 호출, 다른 openActivation 사용처 없음(AGENTS §3 gate 이중 보호 정리).
- ae55a812c2 J3 승인: failure(header)가 기존 kind/message/metadata와 같은 값. 기존 접근자는 시험에서만 쓰이나 public 클래스라 유지(기록).
- 093075ff65 J11: FIFO 보존(waiters 비었을 때만 예산 예약, 미사용분은 oldest waiter에 grant) 승인. 단 J-F1.
- 346242ca90 J1 승인: 요청당 mapRequestFailure stage 제거, terminal을 entry에 직접. 취소 전달은 completeCancellation↔forwardCancellationTo의 volatile 이중 확인으로 누락 없음. OK 경로의 requestAdmission 동기 complete는 이전과 같은 시점.
- 370b978d5d J4 승인: UTF-8 직접 검증이 round-trip 비교와 같은 입력 집합을 거부(overlong·surrogate·>U+10FFFF·C0/C1·NUL), 기존 FRAMEWORK_MULTIPART_PROFILE_FIELDS 재사용.
- b6c6968494 J13(binding) 승인: bindings/doc/spec/message-ownership.ko.md:336-341 표는 "구현 예이며 공개 계약이 아니다", java/README.ko.md:615 "반환될 수 있다". 수정 전 실패 시험 있음. ※ binding 재릴리스 필요(사용자 확인).
- 73613792b4 (c): J-F2.
- d02f8a976b (a) 승인: record당 0초 poll 제거, 첫 record는 claim 전 permit(I1), 예산 record는 예약 permit 안에서 claim(I2).
- d7469ecaf1 (b) 승인: liveness 수신 기록 회전·source당 1회.
- 0db1cc211f J7 승인: nextDueNanos→nextDueKnown 쓰기 순서로 lane 밖 읽기 일관.
- 7c240c9672 merge 승인: main #1805(54d52a24cf)가 multicast 채널 자격 검사를 의도적으로 발행 node로 옮김(02-spot-messaging.ko.md:526-544) — main 쪽 채택이 맞다.
로그 직접 확인: full/branch/summary.txt tests=3361 failed=0 (16:21, 마지막 커밋 16:12 뒤), gradle BUILD SUCCESSFUL, format-final "== java" 실패 없음, api-java/kotlin BUILD SUCCESSFUL.
J-F1 (AGENTS §3 판정 위치 하나): "지금 free이고 older waiter 없음"이 ZLinkApplicationJobQueue.acquire()·reserveAvailable()·reserveRotationBudget() 세 곳. lock 안 판정 하나로 모으고 세 경로가 그것을 쓴다.
J-F2 (동작 변경): classifyNodeSendTarget fast path — intent 없는 admitted peer가 automaticNotRequiredPeers에 있으면 이전엔 TARGET_NOT_FOUND, 지금은 sendable. fast path 조건에 !automaticNotRequiredPeers.containsKey(target) 추가 + 그 경우 시험.
주의: #1648 브랜치도 ZLinkServiceLivenessRegistry.tickCore를 바꾼다(next wait timeout) — 나중에 머지하는 쪽이 충돌을 푼다.
PR 전 할 일: 이 브랜치의 이전 머신 커밋(2dc7ad5723..4458abe381, 848ce30597 포함) 리뷰 기록이 없다 — supervisor-guide §1.1-2에 따라 PR 전에 감독자가 커밋별로 리뷰한다.

## PR 전 브랜치 전체 리뷰 (감독자, 2026-10-15, origin/main...1ac52f3108, 55파일 +2464/-749)
이전 머신 커밋(2dc7ad5723..4458abe381, 848ce30597 포함)을 브랜치 전체 diff로 줄 단위 리뷰 — main 소스 전부, 시험 20개 전부.
판정: 승인 조건부(J-O1 수정 뒤). 스펙 gap: J-O1 하나.
- 승인: B1/B2(owned codec 출력 이전, 복사 없음), completion dispatcher lock-free(ZLinkServiceCompletionBudgetTest가 MONITORENTER 0을 bytecode로 고정), operation registry slot pool(재사용 slot을 이전 caller가 취소 못 함 시험), shared transport lane, FromOwner 변형, state lane 무할당 idle turn·한 번 실행에 drain, handler activation 지연 open.
- ZLinkChannelSubmitTurnTest 이름·assertion 변경 승인: 08-messaging-hot-path.ko.md:95,105,125가 selector 소유자(E2)와 operation 소유자(E3·E4) turn을 분리 — 등록 교체는 selector lane에서 진행되어 operation lane이 바빠도 끝나는 것이 스펙.
- J-O1 (스펙 gap, 반려 대상): ZLinkStateLane의 static publishOutsideLane이 whenCompleteAsync(publication)로 ForkJoinPool.commonPool을 쓴다(ZLinkJavaDurableRequest:115 사용). main #1804가 스펙 06:209-225에 따라 common pool 의존을 없앤 것과 충돌. 기존 DEFAULT_EXECUTOR(virtual thread)로 발행한다.
- J-T1 (시험 시간 한도): ZLinkServiceOperationRegistryTest.concurrentTerminalProducersKeepTheirPublicationOrder의 submission.get(1s)·callbacks.await(1s)는 부하 시 간헐 실패 위험 — 5s로.
- 작업 트리에 범위 밖 변경 framework/languages/unity/.../zlink-stream-connector.jspre(M) — 빌드 산출물로 보임, 커밋에 넣지 않는다.
- 2026-10-15 감독자 리뷰 승인: b71c6d3280 J-O1(publishOutsideLane → DEFAULT_EXECUTOR, 다른 executor 없는 *Async 추가 없음), b01df5ea00 J-T1, merge 1d8712b619 충돌 없음 — 양쪽 수정 파일 Message.java(main과 동일해짐, bindings diff 0), RawMeshNode(main #1810 pump close·Actor admission NotFound 그대로), RawSpotNode(branch inSourceTurn 유지). 로그 원문 m1673g/full/merged/summary.txt tests=3376 failed=0 gradle exit=0. PR #1820 승인, 머지는 0.30.1 태그 뒤.
- zlink 조건부 승인(#1820 코멘트): #1815 먼저 머지 → registry 충돌 해소 hunk 재리뷰, #1761 머지 뒤 Java 전체 재실행.

## 2차 리뷰 (감독자, 2026-10-15) — framework-java/1673-per-message-cost-2: da5e6928e9(merge), cc3b751ac4, 41aee094ed, 75d484921e
- cc3b751ac4 승인: flow 판정 ZLinkMessageFlowTracer.captureEnabled() 한 곳(Normal 이상, 04-flow-correlation.ko.md:91), ActorSubsystem의 별도 판정 삭제·tracer 하나 공유. dispatch error 기록은 dispatchErrorsEnabled(Off 제외)로 분리 — 03-message-flow-tracing.ko.md:201(Errors에서 오류·backpressure·drop 기록). ZLinkMessageFlowTracerTest 단언 변경은 S2가 규칙을 바꾼 결과로 정당.
- 41aee094ed 승인: handedOff synchronized → volatile + VarHandle CAS(C3, 06 §4), 빈 context 복원 객체 공유. 의미 동일.
- 75d484921e 승인: DERR-009를 Normal로(단언 불변).
- 시험: 전체 run이 다른 job의 hangrep.sh(150초 넘은 Gradle Test Executor kill)로 오염 — 감독자가 watcher 종료, 깨끗한 재실행 필요. 이중 close 실패(ChannelBlockingSubmissionTest, SpotCloseConformanceTest) 미판정.
결정: item 2 = B(lane primitive 경량화, 스펙 안). A는 06 §4에 'dispatch 한정' 분류가 없어 스펙 개정 필요 → 보류. Spring autowire/destroy 생략(주입·destroy callback 없는 handler만, 동작 동일) 승인. binding poller 14 obj/msg는 binding 릴리스 필요 → 보류. S1 gap: Java는 BACKPRESSURED send를 등록하지 않음(ZLinkJavaSocketSupport.java:125-126, ZLinkJavaRawMeshNode.java:3073) — 01-submit-and-completion §10(PR #1821) 위반, 다음 작업에서 send entry·종료 경로 설계.
- 2026-10-15 main merge a0e07aeb90(#1828) 감독자 리뷰 승인(codex luna): ZLinkJavaRawMeshNodeM6ATest 충돌 1 hunk — branch nodeRequestKeepsMultipartViewUntilDispatchRelease(:68)와 main localActorJournalHasNoRecordWithoutBindingIdentityOrAuthority(:115) 둘 다 유지, 단언 불변. 자동 merge된 RawMeshNode·RawSpotNode·InternalSpotNode는 서로 다른 경로(job 확인). unit 2740·contract 25 실패 0, format·API snapshot 통과. 주의: job이 에픽 #1639 보드를 Review로 바꿔 감독자가 InProgress로 되돌림.
- 2026-10-15 88e6dafa23 감독자 승인(lane 경량화 B): AtomicInteger 2개 → volatile+VarHandle, mailbox·close future 지연 생성(CAS). close 완료 누락 없음 확인 — close는 completed 쓰기→CLOSED→scheduled 읽기, drain은 scheduled=0→completed 읽기(volatile SC로 한쪽은 상대 쓰기를 봄); 거부 경로 admission<0이면 completed 가시. 판정 위치 4→4. 시험: 반사 helper 두 곳만 지연 mailbox에 맞춤(단언 불변) + 16 producer 동시 첫 제출 시험. lane 생성당 객체 6→1. unit 3206·contract 91 실패 0.
- Spring: 보류 승인(공개 API로 생략 조건 확정 불가 — DisposableBeanAdapter package-private, 기본 context의 ImportAware/ApplicationListenerDetector가 instance별 동작). Framework가 지원할 processor 계약을 정하기 전에는 하지 않음.
- S1: 안 1 승인(기존 ZLinkServiceOperationRegistry에 send terminal entry, BACKPRESSURED만 등록, 등록 뒤 admitted 관찰, close와 terminal은 registry take 한 곳, send는 deadline·cancel 없음, caller 완료는 registry dispatcher). RawMeshNode:3073(local instance spot)은 BP 경로 아님 — 건드리지 않음.
- 2026-10-16 a3def38cff(S1 안 1) 감독자 리뷰: 등록 경로는 승인 — BACKPRESSURED만 submitSend로 등록(gate 아래, closed면 ShuttingDown), 등록 뒤 admitted 관찰, close·terminal은 takeLocked 한 곳, send는 deadline·caller 취소 없음(minimalCompletionStage), Dealer/Router synchronized·service port state lane으로 submit↔close 직렬화. **반려 J-S1-1:** ZLinkServiceOperationRegistry 생성자가 scheduleAtFixedRate(expireMaintenance, 1ms)(:75-76) — 이제 Dealer·Router socket마다·독립 service port마다 registry를 만들어 socket당 1kHz 주기 작업이 생긴다(ClientServer 연결 수만큼). send entry는 deadline이 없으므로 send만 담는 registry는 주기 작업이 없어야 한다. #1815(timer wheel)와의 정합도 확인.
- 2026-10-16 57fc2ecc45 J-S1-1 감독자 승인: maintenance를 첫 REQUEST 등록 때 gate 아래에서 시작(registerLocked는 closed면 예외), close는 같은 gate에서 closed 설정 후 취소 → close 뒤 주기 작업 생성 없음. send만 있는 registry 예약 0 시험(수정 전 3 red). unit 3207·contract 91 실패 0. #1815(timer wheel)는 아직 lazy 아님 — 머지 때 확인 항목(m1673k-report).
- zlink #1846 승인(15:44). 참고(후속): RawMeshNode와 port가 공유 operation registry를 각각 close — 소유자 한 곳(registry를 만든 RawMeshNode)에서만 닫게 정리할 후속 항목. close는 멱등이라 결함은 아님.
