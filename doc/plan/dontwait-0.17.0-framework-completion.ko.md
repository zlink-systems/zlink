# 0.17.0 DONTWAIT framework 완결 체크리스트

작성: 2026-09-04 (machine A, 자율주행). 진실원천은 이 문서 + `doc/plan/c016-worklog/`.
기준선(전부 green): `seven-samples-green-v1` (4bad5ac979, 2026-08-29). 그 이후 0.16.0 pull-completion
캠페인이 회귀를 유발 → 0.17.0 DONTWAIT 재설계(B의 core `50d77800f2` + 바인딩)로 정정 중.

## 역할
- **B**: core = 성능 개선만. 전 언어 바인딩(cpp/node/dotnet/java 완료, 0.17.0 bump `70a9998998`).
- **A(나)**: framework 적응·검증, **core 버그는 직접 수정+커밋+푸시**(sub-agent 활용), 남은 회귀 전부 해소.

## 근본원인 (확정)
1. **DONTWAIT send 우회** (해소): framework가 DONTWAIT 전송에 sync submit을 써서 바인딩의 async
   backpressure/WRITABLE-resubmit를 우회 + 완료 poller가 POLLOUT 미구독. → **fix = 바인딩 async send await
   + poller가 POLLOUT|POLLCOMPLETION 구독 후 completion queue drain(스펙 05-polling·socket/README Part send)**.
2. **terminal/error 분류 회귀** (미해소, 별개 클러스터): request/transport timeout이 특정 terminal 대신
   generic으로 분류됨. 원인 미확정 (B core 변경? two-poller? request-reply?).

## 체크리스트

> **2026-09-04 세션 핸드오프:** WSL C→D 이전으로 워크스페이스 재-clone 예정.
> 진행상황·다음 단계·환경은 [`c016-worklog/handoff-2026-09-04/HANDOFF.ko.md`](c016-worklog/handoff-2026-09-04/HANDOFF.ko.md).
> origin/main = `8f8f75ff71` (DONTWAIT fix + docs 자연화 머지 + 가이드 재생성 + 앵커수정, **사이트 배포 완료**).

### A. DONTWAIT send/POLLOUT framework fix
- [x] cpp framework 적응 (raw_dealer_port·raw_route_port·channel_outbound_exchange·stream_host_service) — `channel_messaging` GREEN
- [x] dotnet framework 적응 (ZLinkBackendStreamSocketWrapper[D-084]·SubmitFailureMapper·ManagedMeshNode·SpotOutbound{Endpoint,Transport}) — binding WRITABLE 계약 22/22, backpressure-retry GREEN(epoch fallback)
- [x] node framework 적응 (node-socket-backend-adapter: sync 분기 제거) — 샘플 all PASS, 테스트 fail 0
- [x] java framework 적응 (ZLinkJavaRawServicePort·ZLinkJavaSocketReceivePoller: POLLOUT|POLLCOMPLETION 구독 + state-lane) — Java↔Node/.NET E2E 4/4
- [x] **커밋+푸시 완료** — `4d263e66b9` (origin/main)
- [x] 스펙 준수 확인: POLLOUT은 미읽은 WRITABLE 있을 때만 level, drain하면 해제 (busy-spin=스펙위반→Core/B)
- [x] 게이트 재검증(B 최신 바인딩 대비): node DONTWAIT 해소✓, java DONTWAIT+E2E 4/4✓ (cpp·dotnet은 §B 미해소로 미완)

### B. terminal/error 분류 회귀 (별개 클러스터 — 근본원인 규명 필요)
- [x] **cpp m6a** `records.size()==1` — 근본원인 = Core 0.17이 스펙대로 ROUTER 미등록 RID submit을 `NOT_FOUND+ENOENT`로 답하는데
      framework `transient_route_errno`가 ENOENT를 빠뜨려 영구 실패로 분류 → 재시도 없음. **수정+회귀 assert 커밋 `a22f8c880b`**.
      후속으로 드러난 같은 endpoint RID 교체 시 stale connect intent 문제(spec 04-network-listener-identity §replacement)도 **수정 커밋 `44b9b27efc`**.
- [x] **cpp m6b** `error_kind()==deadline_exceeded` — 동일 원인(ENOENT), 격리 3/3 green. 잔여: m6b 후반 `verify_raw_spot_and_actor_routing` line 4524 /
      `route_cache_stops_at_owner_admission_deadline` line 1909 불안정(별개 원인, job `bucketB-cpp-m6b-late` 진행 중)
- [x] **cpp m6a 잔여**: `verify_client_server_plain_hello_is_rejected` — 테스트가 DEALER의 completion poller를 구동하지 않아 거부 종결이 관찰되지 않던 하네스 결함, 수정 커밋 `cc9a4edcd7` (최종 cpp ctest에서 m6a green)
- [x] **dotnet stale-authority 3건**: 원인 = spec(README §4 HANDOVER)이 정한 패배 방향 request timeout. Framework가 Core handover 수렴 전에 `Admitted`를 공개한 것이 결함.
      수정 = settle-before-submit(패배 outbound intent를 endpoint로 disconnect, 패배 방향이 ready였다면 두 lane Disconnected 처리 뒤에만 Admitted 공개), 회귀 `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`.
      3건×3회 9/9, Stateful suite 50/50. 전체 unit gate는 감독관이 실행 중(커밋 대기). generic request 자동 재전송은 spec(02-spot-messaging:392-403, 02-session-actor-binding:626-634)이 금지하므로 미도입
- [x] **node 신규(B)**: `user-spot-native-two-process` — 첫 request가 deadline 전부 소진 → replay 여지 없음. 수정 커밋 `285cfa522a`
- [x] 판정: cpp 4건·dotnet 1건·node 1건 모두 framework(+테스트 하네스) 결함. Core·binding 버그 0건 (Core 수정 job 1건은 오판으로 중단).
- [x] 수정 + 재검증 + 커밋 — cpp 4/4·node 1/1 커밋, dotnet 4건 커밋(`b28eb24270`·`25952a76bc`·`6b77ba013f`·`ebff5b3e1b`, unit gate v7 1920/0+15/15), java 2건(`d8575e6fbb` STREAM push, ZoneWorld runner `83dfe33fb5`), kotlin runner `9e76d053e5`, node lint `8dd97bda2d`

### C. 검증 인프라 (신뢰성 확보)
- [x] 환경 재구축(2026-09-05, WSL 재설치): Core dev 빌드, **로컬 Core 0.17.0 프리픽스 수동 구성**(GitHub에 `core/v0.17.0` 릴리스 없음 — `fetch-release.sh` 캐시 단락 경로 이용), 4언어 로컬 패키지,
      cpp 의존성(apt boost/gtest/gmock/lz4/libuv + hiredis·redis++ 소스 빌드), node `http-client` tarball 재생성(`npm pack ./packages/http-client`), Docker Desktop WSL 통합, Playwright Chromium
- [x] cpp 샘플 신뢰성 있는 실행 — 별도 build dir `build/linux-ninja-c-e2e`(protobuf include 수정 커밋) 에서 **7/7 PASS**(`c-cross-language-e2e-2-summary.md`)
- [x] cross-language E2E: C++ host stale 문제는 caller가 `ZLINK_CPP_BUILD_DIR` 미지정이 원인(runner는 이미 우선 계약) — 0.17 host로 재실행. C++↔.NET·C++↔Node messaging/flow/stream PASS, C++→.NET spot-route PASS.
      `.NET→C++ spot-route` `Unavailable` = .NET RouteMesh admission 후보 선택 결함(unilateral 연결의 Hello를 새 inbound로 오인해 유일한 outbound 폐기; cpp `raw_mesh_node_owner.cpp:445-476` fallback과 parity) → 수정(미커밋, dotnet 전체 unit gate 후 커밋) 후 **cpp all-stage runner 32/32 PASS**(Java↔C++·relocation·User-Spot Join 포함).
      node smoke flow tag = node 하네스가 spec attribute 이름 대신 log 축약 key를 읽던 문제 → `4135d4edf6`; node smoke 12/12 stage PASS(마지막 Redis stage는 미복원 test project = 환경, `dotnet restore` 완료)
- [x] `java-cross` selector(Java↔Node/.NET) 4/4 통과. Java↔C++ 방향은 all-stage runner 후반이라 위 .NET→C++ 실패 해소 후 실행

### D. 최종 게이트 (plan line 143: framework unit + cross-language E2E + 7 samples 전부 green)
- [x] 4언어 framework unit — 최종(2026-09-05): **dotnet 1920/0 + Canonical 15/15(hang 0; 36분짜리 D-068 sibling만 제외)**, **node 표준 gate 1536/0 + lint 0**, **cpp ctest 68/69(inventory 278 = 기존 C, m6b 1909 flake 미발생)**, **java core 1216/2(M6A 2건 = 기존 C)**, contractTest는 JavaDocumentationRegression 1건(기존 C). A/B 실패 0.
      java 1213/2 = pre-existing M6A 2건만(F 1건 `EntrySpotActorDispatchTests…staleTerminal`은 하네스 drain barrier 수정 `2b1d0c794c`);
      node 1552/5 → C 4(ZoneWorld dist 미빌드 3·lint 1) + F 2 수정 완료(`2ceb137abe`);
      cpp gate-v2 63/69 → stream-connector fixture(RAW mode before bind, spec 08-stream §2) `dbfcf7d6fe`·lz4 packaging `20b94c3457`·package-test config `4573c09a2a` 수정 후 잔여 = inventory 278(C)·m6b 1909 flake(C); **최종 cpp ctest(리팩토링 후) 68/69 = inventory 278만, m6b flake 미발생, package consumer 2건 pass**; **node 최종 표준 `npm test`(build·typecheck·lint·runtime) 1536/0, lint 0 errors** — `spot-timer.ts:137` 1줄 수정 후(ZoneWorld dist 3건·two-process·Chromium 항목 모두 green);
      dotnet 전체 unit gate: v3 852/1/1hang → v4 → v5 1917/2 → **v6 1919/0 + Canonical 15/15** → **v7(TicTacToe 수정 포함) 1920/0 + Canonical 15/15, hang 0** (36분짜리 D-068 sibling만 제외). 커밋: 승인 rework `b28eb24270`, ClientServer 재연결 `25952a76bc`, fixture DEALER 누수 `6b77ba013f`, inbound peer 재사용 `ebff5b3e1b`.
      relay fail = 늦은 loser `ConnectionReady`가 survivor의 RID→pair 인덱스를 덮어 command 33이 `current_source=False`로 폐기(수정, 15/15);
      hang = fixed-RID handover 테스트 fixture 경합(`RouteAdmission_HandoverStartsFreshLivenessDeadline`, 이전 DEALER reconnect intent 유지) — fixture 수정(8/8).
      **Core 후보(B 보고) → 해소(2026-09-05 05:40, Core 결함 아님)**: dump 분석 결과 native ctx에 남은 socket은 test fixture(`CanonicalActorJoinIngressReplyTests.ConnectedRuntime.HandoverAsync/ReconnectAsync`)가 admission 실패 예외 경로에서 dispose하지 않은 fixed-RID DEALER 1개(managed handle GC root 0) — `zlink_close` 미호출 상태에서 `Context.DisposeAsync`가 spec(`core/doc/spec/core/socket/README.ko.md:486`, 모든 socket close 후 ctx term)대로 block. 공개 C API repro(모든 socket close 시 tcp/inproc/handover cycle 5/5 즉시 반환; 미close DEALER 1개면 늦은 close까지 block) = `doc/plan/c016-worklog/evidence/test_ctx_term_fixed_rid_handover.cpp`. fixture 수정 job 진행 중. 2차 발견(Core 후속, B 영역): tcp에서 이전 pipe가 살아있는 same-RID replacement DEALER admission이 0.1~2.9 s(간헐 >5 s, inproc 즉시) — decisions D-086.
      gate-v5(재admission 수정 포함, Canonical 별도 3분 blame): Canonical 11 pass + hang 1(`RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`, 단독 2/2 pass → teardown/ctx_term 계열, Claude sub-agent 조사 중; codex는 content filter로 2회 사망);
      나머지 suite **1917 pass / 2 fail / hang 0**(`InstanceSpotIdleInspection` hang 소멸): `MalformedPushedControl_ReconnectsAndReadmits`(round-1 수정 후 단독 5/5이나 full suite에서 재현, `ready=0` — round-2 job) + `ManagedNode_Tcp_SameEndpoint_Replacement_RemainsAdmitted_Across_Repeated_Lifecycles`(단독 3/3 fail, 결정적 회귀 — job 진행 중)
- [ ] 7 samples × 4언어 green — **cpp 7/7**(c-e2e-2), **node 7/7**(SupportChat budget `2e3b1b47e4`, ZoneWorld entry html `c67deb44e0`, runner PASS marker `2ceb137abe`), **dotnet 7/7 + aggregate**(2차, `ebff5b3e1b` 후), **java+kotlin 14/14 완주**(3차 aggregate; 이후 forbidden-pattern scan 2건 = DeliveryDispatch test sleep(`ed156f5983` 도입)·TicTacToe client polling → `cb22cf6ae5`로 제거, scan clean) → **최종 aggregate(`cb22cf6ae5`) 14/14 marker + "All Java/Kotlin samples passed" exit 0** (`zlink-work/c016/logs/gate-final-java-aggregate-final.log`). 잔여 간헐 관찰(미재현, 기록만): java Bingo TEARDOWN_FAILED 1회(2차 gate, 재시도 pass, 6회 재현 0 — `bucketB-java-bingo-teardown-2-summary.md`). **재실행(현재 패키지·최종 tree, `gate-final-cpp-node-samples-summary.md`)**: cpp 개별 7/7 + aggregate exit 0(203 s); node 개별 7/7(GameQuest 1회 Redis endpoint 대기 D 후 재시도 pass), aggregate는 1차 cpp 동시 실행 포트충돌(D) → 단독 2차에서 **SupportChat.Ts browser scenario 3개 wait timeout 재발**(`gate-final-samples-node-aggregate-2.log:270-300`, 단독 실행은 pass) → round-2 job 진행 중(예산 확대 금지, wait arming 순서/재개 세션 push 근본원인).
      java: TicTacToe·SupportChat·DeliveryDispatch(`ed156f5983`+teardown `6682ae0db1`)·GameQuest(heartbeat `dc9fe76100`)·ShoppingMall·**Bingo**(teardown/heartbeat 수정으로 해소, exit 0) = **6/7**; 최종 java gate(2026-09-05 05:22): 개별 6/7(TicTacToe는 gate job이 `./run_sample.sh`를 직접 실행한 탓(비실행 모드는 `SampleReleaseGateContractTest:140-143`가 의도적으로 고정, runner는 `bash` 호출) → 실행비트 커밋 `133d01c9b2`는 revert `dd23fce2ff`; ZoneWorld A3 `moveTo` timeout 1/4 간헐 → job), aggregate는 Java 7/7 완료 후 Kotlin GameQuest `Program.kt:179` `ensure(unavailable != null)` 실패(owner termination 뒤 요청이 성공) → job;
      ZoneWorld: A2 지연 = public mesh poller owner가 command 36 STREAM admission을 동기 대기 → 비동기 admission `424b15684c`; A5 = STREAM 수신 owner가 control frame(heartbeat pong·session-closing) 전송을 state lane에서 동기 대기 → 비동기 전송 `7b0590183d` → **FULL ZoneWorld 2/2 green** ⇒ **java 7/7**(최종 일괄 게이트 job 실행 중);
      dotnet 7 samples(1차, `ebff5b3e1b` 전): 6/7 — TicTacToe `JoinGameNotify` 미전달 = auto-connect가 이미 admitted된 inbound peer를 재사용하지 않고 reciprocal candidate를 열어 route/survivor 불일치(수정 `ebff5b3e1b`, 3/3) → **2차 dotnet 7/7 + aggregate exit 0(300 s)** (`gate-final-dotnet-samples-2-summary.md`). java ZoneWorld A3 = bound-session push가 호출 thread에서 STREAM state lane join(수정 `d8575e6fbb` 직전 커밋, prefix 5/5·FULL 2/2) → java 2차: 개별 6/7(+Bingo 1회 TEARDOWN_FAILED 후 재시도 pass, ZoneWorld는 상대경로 `bash "$0"` 자기호출 runner 결함 D) → Bingo teardown+ZoneWorld runner job 진행 중; core test 1216/2(M6A). cpp/node 리팩토링 pass 커밋 `01e5c4613d`.
- [x] cross-language E2E green — **최종(커밋 tree `ebff5b3e1b`+java `d8575e6fbb`): cpp all-stage 32/32, node smoke 12/12(Redis 양방향 stage 실행 확인), java-cross 4/4** (`gate-final-cross-language-summary.md`; 초기 1회 Java library-path env 실패는 D, unset 후 재검증)
- [x] 최종 커밋+푸시 — 2026-09-05 07:5x 기준 main 최신(`cb22cf6ae5` + 계획서). 캠페인 framework 수정 커밋: cpp 8·dotnet 4·node 4·java 3·kotlin 1·samples 2·refactor 1, 전부 origin/main 푸시. Core/bindings 수정 0건(후속 D-086·D-087은 B). 미커밋 잔여 없음(`bindings/node/provenance/core-package-provenance.json` 로컬 빌드 산출물만 제외).

### E. 문서/사이트 (2026-09-04 추가)
- [x] docs PR #2 (영문 자연화 + archive 삭제) 머지 `3f0f02d478`, 가이드 재생성 `baa4b4e4f6`, 앵커수정 `8f8f75ff71`
- [x] **docs 사이트 퍼블리시 완료** (GitHub Pages 배포 성공)
- [x] "cpp async-only submit projection" 계약 실패는 로컬 false-failure로 판정(CI 통과) — framework API 추가 불필요
- [x] **[사용자 요청] framework 가이드에 메시징 API 종결자 의미·사용법 추가** — 05-channel-messaging에 개념 절 신설(4언어 ko/en, 04 §3.1 중복 없음) 커밋 `15ba13b217`, MANDATORY 문구 수정 `5e9d8f258e`


## §B 근본원인·해결방안 정리 (2026-09-05 새벽, A 감독관)

### B-1. dotnet stale-authority 3건 (`TimedOut(101)`) — **Core 버그 아님, spec이 정한 handover timeout**
- **현상**: caller/owner가 서로 `ConnectPeer`(manual 양방향), caller 먼저 시작. owner는 stale terminal을 정상 submit(OK)하지만
  caller는 completion을 못 받고 3초 뒤 `ExpireOperationAsync`가 101을 만든다. 순수 C 재현 5/5(`c016-worklog/tools/repro_router_reply_pair_mismatch.cpp`).
- **spec 판정** (`core/doc/spec/core/socket/README.ko.md` §4 rid 중복 정책, HANDOVER): 반대 방향 pipe 충돌 시 두 peer가 RID 비교로
  **같은 방향 하나**를 선택하고, 물러나는 방향에 이미 admit된 request는 **"submit 시점 transport pair로만 reply 전달"**되어 자기
  timeout으로 정확히 한 번 종결되며 **"Caller는 handover 뒤 다시 보낸다"**. framework spec `01-channel-topology` "Peer 연결":
  automatic은 RID가 작은 쪽만 connect, manual 양방향은 duplicate-pipe admission으로 **ready 연결 하나만** 남긴다.
  ROUTER-ROUTER 링크 = lane set 하나(Application lane + Completion lane, READY `Zlink-Lane` 0/1로 확정).
- **Core 구현**: `router_admission.cpp` identify_peer의 `reciprocal_duplicate` — 승자에만 logical RID를 묶고 패자 pipe는 끊지 않고
  익명 standby RID(`_standby_pipes`)로 유지. 수신 측 pair 고정(`socket_request_reply_pending_api.cpp:105`)은 spec 문면 그대로.
  → 세션 중 "수신 측 pair 고정이 spec에 없다"는 판정과 그에 따른 Core 수정 job은 **오판으로 중단·폐기**했다.
- **실제 결함 위치 = .NET framework 순서/재전송**: (a) Core handover가 수렴하기 전에 peer를 `Admitted`로 선언하고 물러날 pair로
  application request를 제출(`mesh_peer_duplicate_retire_skip_transport`가 패자 transport를 standby로 유지), 그리고/또는
  (b) spec이 caller에 요구하는 "handover 뒤 재전송"이 actor request 경로(`CompleteNativeApplicationRequest`)에 없다.
- **해결방안**: cpp(`verify_duplicate_connection_survivor_is_symmetric`, `…bilateral…keeps_survivor`)·node(terminal replay 루프)와
  parity로 .NET에 (a) survivor 확정 전 route 미선택 또는 (b) deadline 내 같은 operation 재전송(exactly-once 유지)을 구현.
  브리프 `briefs/bucketB-dotnet-handover-resend.prompt`(sol/xhigh). Core·spec 변경 없음. standby pipe 유지가 spec "물러난다"와
  일치하는지는 사용자 판단 사항으로 보고만 한다.

### B-2. node `user-spot-native-two-process` `RequestError(101)` — 같은 현상의 node판, framework 수정 완료(미커밋)
- 첫 raw request가 end-to-end deadline 전부를 소진해 reply 유실 뒤 terminal replay 기회가 없었다. 수정: attempt별 남은 deadline의
  절반만 배정(`service-stateful-runtime.ts:4008`), 회귀 테스트 `test/m6b/m6b-user-spot-terminal-replay.contract.ts`. 3/3 green.
  → 새 Core 재빌드 후 감독 게이트 재실행하고 커밋.

### B-3. cpp — 4건 모두 커밋 완료
- `a22f8c880b` ENOENT 재시도 분류, `44b9b27efc` 같은 endpoint RID 교체 시 stale connect intent 종료,
  `cc9a4edcd7` plain-hello 테스트 하네스(poller 구동), `cee95ff462` bound-session 테스트를 실제 transport 경로로.
- 잔여 후보: m6b line 1909 route-cache owner-admission 타이밍 flake(2/15) — in-memory store(system_clock) vs runtime(steady_clock)
  cross-clock, `bucketB-cpp-m6b-late-summary.md` 참조. 별도 job.

### B-4. **새 의존성 — Core REQUEST 계약 B (D-B85, `7d8205a028`, 2026-09-05 00:24)**
- DONTWAIT request가 SEND처럼 `BACKPRESSURED`+wait token → WRITABLE 후 재제출. ROUTER no-route는 SEND와 같은 `NOT_CONNECTED` 규칙.
- **B의 bindings 8개 REQUEST 포팅 완료·푸시됨**(2026-09-05 01:4x: java `a06260f507`, dotnet `6b4c60eb33`, cpp `e0860723bc`, node `b145f86501` 등). 바인딩 async request
  종결자가 BACKPRESSURED→WRITABLE 재제출을 내부 처리하므로 §A에서 바인딩 async 경로+POLLCOMPLETION으로 전환한 framework는 추가 적응이 최소일 것으로
  예상 — Core(`29add0ac81` 포함)+로컬 패키지 재빌드(01:53) 후 **gate-v2**(cpp/java/node 즉시, dotnet은 handover job 뒤)로 실측한다.
  01:53 이전 게이트(node 게이트·샘플 등)는 새 Core+옛 바인딩 조합이었음을 감안한다.
- 즉시 조치: 새 Core로 `core/build-dev`·프리픽스·로컬 패키지 재생성 후 cpp m6a/m6b·monitor 재현 재실행(진행 중).

## 알려진 pre-existing (회귀 아님, 별도 추적)
- node lint `spot-timer.ts:137` (2026-08 이후 불변, DONTWAIT 무관)
- java M6A 2건 + JavaDocumentationRegression 1건 (0.16 전환 커밋에도 존재)
- cpp `common_e2e_inventory` 278 (feature-map 미충족 inventory gate)

### F. 2026-09-05 전수 리뷰 이후 (사용자 지시: 완화 금지·근본원인·하위 계층 재구현 제거)
- [x] 리뷰 4건(`review-campaign-{dotnet,java,cpp-node}.md`, `review-layer-ownership-audit.md`) — dotnet Mesh/ClientServer의 물리 상태기계·pair fence(D/C), cpp ENOENT 병합(D), node half-budget(C), SupportChat 예산(C), java M6A 2건은 `e65abaf7ac` 회귀 확인
- [x] AGENTS.md §3/§4 필수 규칙(계층 소유권·금지 패턴·교차언어 대조·진단 먼저) `6ad197b14b`; spec 결정 2(b) `3a2ada8187`, 5 `3a2ada8187`+`662e191536`
- [x] 1단계(B 무의존): dotnet SUB 특례 제거 + Canonical fixture 경쟁 복원(3 red, D-086 귀속) `5883711e88`; cpp ENOENT 재분리(m6b red, terminal 종류 미분리) `6b18a3f445`; node gate false-green 제거(1560/1559/1) + SupportChat 값 복원 + replay 정책(원인 (b) route 미admit) `71202e4947`; node 소진 종류 `21c12eb02e`
- [x] B 확정: D-B94 D-086은 **Core 버그**(same-RID tcp replacement가 pair ID 재사용) 수정 `7ffb8e55d9`; D-B95 java monitor ABI 수정 `c9d294c44f`(inproc/CLOSED id는 Core 후속, Poller monitor 등록은 spec gap); D-B96 reciprocal HANDOVER spec대로(패배 lane standby 유지) → dotnet settlement 삭제 근거
- [x] cpp 2단계 terminal 종류(route retry 소진 → Unavailable, admitted reply timeout만 DeadlineExceeded) 커밋; node m6b InvalidState = fake STREAM에 `submit_sync` 없음(fixture 수정 커밋) → 남은 red는 runtime `deliver()` catch-all(backpressure를 세션 종료로 처리) → 진단 job; parity 후속: dotnet/java Actor Join·node STREAM bind의 pre-admission 소진이 DeadlineExceeded(진단 예정)
- [ ] node 진단: DeliveryDispatch courier NotFound 진행 중; STREAM deliver catch-all 진행 중
- [ ] 로컬 패키지 재빌드(`7ffb8e55d9`·`c9d294c44f` 포함) → dotnet Mesh 걷어내기 2단계(brief `stage2-dotnet-mesh-unwind.prompt`) → Canonical 3 red 재검증 → dotnet ClientServer second poller/수동 reconnect 제거(B 작업 4 결과 대기) → java 합성 connection id·errno 표 제거(B 작업 5·6) → cpp replay 범위 진단 → 4언어 최종 게이트 재실행

