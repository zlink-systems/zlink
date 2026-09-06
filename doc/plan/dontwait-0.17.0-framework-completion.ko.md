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
- [x] 7 samples × 4언어 green — **최종 gate10(2026-09-05 23:25~23:42, Core `d8b65141a4`/785b647b…, 안정 시계): cpp unit+7 PASS, java 7 + kotlin 7 PASS(모든 host role Stopped/None), node 7 PASS + npm test 1604/0, dotnet 7 + aggregate + ZoneWorld×2 PASS, dotnet unit 1975/0 + join 16/0 + SampleRegression 157/0; cross-language E2E(rebuild10) cpp all-stage·node smoke 19 ok·java-cross 모두 passed** — 이전 기록: **cpp 7/7**(c-e2e-2), **node 7/7**(SupportChat budget `2e3b1b47e4`, ZoneWorld entry html `c67deb44e0`, runner PASS marker `2ceb137abe`), **dotnet 7/7 + aggregate**(2차, `ebff5b3e1b` 후), **java+kotlin 14/14 완주**(3차 aggregate; 이후 forbidden-pattern scan 2건 = DeliveryDispatch test sleep(`ed156f5983` 도입)·TicTacToe client polling → `cb22cf6ae5`로 제거, scan clean) → **최종 aggregate(`cb22cf6ae5`) 14/14 marker + "All Java/Kotlin samples passed" exit 0** (`zlink-work/c016/logs/gate-final-java-aggregate-final.log`). 잔여 간헐 관찰(미재현, 기록만): java Bingo TEARDOWN_FAILED 1회(2차 gate, 재시도 pass, 6회 재현 0 — `bucketB-java-bingo-teardown-2-summary.md`). **재실행(현재 패키지·최종 tree, `gate-final-cpp-node-samples-summary.md`)**: cpp 개별 7/7 + aggregate exit 0(203 s); node 개별 7/7(GameQuest 1회 Redis endpoint 대기 D 후 재시도 pass), aggregate는 1차 cpp 동시 실행 포트충돌(D) → 단독 2차에서 **SupportChat.Ts browser scenario 3개 wait timeout 재발**(`gate-final-samples-node-aggregate-2.log:270-300`, 단독 실행은 pass) → round-2 job 진행 중(예산 확대 금지, wait arming 순서/재개 세션 push 근본원인).
      java: TicTacToe·SupportChat·DeliveryDispatch(`ed156f5983`+teardown `6682ae0db1`)·GameQuest(heartbeat `dc9fe76100`)·ShoppingMall·**Bingo**(teardown/heartbeat 수정으로 해소, exit 0) = **6/7**; 최종 java gate(2026-09-05 05:22): 개별 6/7(TicTacToe는 gate job이 `./run_sample.sh`를 직접 실행한 탓(비실행 모드는 `SampleReleaseGateContractTest:140-143`가 의도적으로 고정, runner는 `bash` 호출) → 실행비트 커밋 `133d01c9b2`는 revert `dd23fce2ff`; ZoneWorld A3 `moveTo` timeout 1/4 간헐 → job), aggregate는 Java 7/7 완료 후 Kotlin GameQuest `Program.kt:179` `ensure(unavailable != null)` 실패(owner termination 뒤 요청이 성공) → job;
      ZoneWorld: A2 지연 = public mesh poller owner가 command 36 STREAM admission을 동기 대기 → 비동기 admission `424b15684c`; A5 = STREAM 수신 owner가 control frame(heartbeat pong·session-closing) 전송을 state lane에서 동기 대기 → 비동기 전송 `7b0590183d` → **FULL ZoneWorld 2/2 green** ⇒ **java 7/7**(최종 일괄 게이트 job 실행 중);
      dotnet 7 samples(1차, `ebff5b3e1b` 전): 6/7 — TicTacToe `JoinGameNotify` 미전달 = auto-connect가 이미 admitted된 inbound peer를 재사용하지 않고 reciprocal candidate를 열어 route/survivor 불일치(수정 `ebff5b3e1b`, 3/3) → **2차 dotnet 7/7 + aggregate exit 0(300 s)** (`gate-final-dotnet-samples-2-summary.md`). java ZoneWorld A3 = bound-session push가 호출 thread에서 STREAM state lane join(수정 `d8575e6fbb` 직전 커밋, prefix 5/5·FULL 2/2) → java 2차: 개별 6/7(+Bingo 1회 TEARDOWN_FAILED 후 재시도 pass, ZoneWorld는 상대경로 `bash "$0"` 자기호출 runner 결함 D) → Bingo teardown+ZoneWorld runner job 진행 중; core test 1216/2(M6A). cpp/node 리팩토링 pass 커밋 `01e5c4613d`.
- [x] cross-language E2E green — **최종(커밋 tree `ebff5b3e1b`+java `d8575e6fbb`): cpp all-stage 32/32, node smoke 12/12(Redis 양방향 stage 실행 확인), java-cross 4/4** (`gate-final-cross-language-summary.md`; 초기 1회 Java library-path env 실패는 D, unset 후 재검증)
- [x] 최종 커밋+푸시 — **2026-09-06 00:4x 기준 main 최신 `009accd83a`(origin/main 동일)**; 2026-09-05 하루 커밋: Core 8(D-090·NOT_FOUND·inproc preamble·D-092/same-RID pair·STREAM DISCONNECTED·D-094·close endpoint release + B의 D-B112/D-B11x), bindings java 1(pump close), framework dotnet 8·node 6·java 8·cpp 2, samples/runners 4, spec 3(§4 REJECT/§6 D-090, §14 D-097), decisions D-086~D-097. 이전 기록: 2026-09-05 07:5x 기준 main 최신(`cb22cf6ae5` + 계획서). 캠페인 framework 수정 커밋: cpp 8·dotnet 4·node 4·java 3·kotlin 1·samples 2·refactor 1, 전부 origin/main 푸시. Core/bindings 수정 0건(후속 D-086·D-087은 B). 미커밋 잔여 없음(`bindings/node/provenance/core-package-provenance.json` 로컬 빌드 산출물만 제외).

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

## 알려진 pre-existing (회귀 아님, 별도 추적) — 2026-09-06 재판정: node lint = npm test lint 0으로 해소, java M6A 2건·JavaDocumentationRegression = 해소(위 §F), cpp inventory 278만 잔존(feature-map inventory gate, 샘플·unit과 무관)
- node lint `spot-timer.ts:137` (2026-08 이후 불변, DONTWAIT 무관)
- java M6A 2건 + JavaDocumentationRegression 1건 (0.16 전환 커밋에도 존재)
- cpp `common_e2e_inventory` 278 (feature-map 미충족 inventory gate)

### F. 2026-09-05 전수 리뷰 이후 (사용자 지시: 완화 금지·근본원인·하위 계층 재구현 제거)
- [x] 리뷰 4건(`review-campaign-{dotnet,java,cpp-node}.md`, `review-layer-ownership-audit.md`) — dotnet Mesh/ClientServer의 물리 상태기계·pair fence(D/C), cpp ENOENT 병합(D), node half-budget(C), SupportChat 예산(C), java M6A 2건은 `e65abaf7ac` 회귀 확인
- [x] AGENTS.md §3/§4 필수 규칙(계층 소유권·금지 패턴·교차언어 대조·진단 먼저) `6ad197b14b`; spec 결정 2(b) `3a2ada8187`, 5 `3a2ada8187`+`662e191536`
- [x] 1단계(B 무의존): dotnet SUB 특례 제거 + Canonical fixture 경쟁 복원(3 red, D-086 귀속) `5883711e88`; cpp ENOENT 재분리(m6b red, terminal 종류 미분리) `6b18a3f445`; node gate false-green 제거(1560/1559/1) + SupportChat 값 복원 + replay 정책(원인 (b) route 미admit) `71202e4947`; node 소진 종류 `21c12eb02e`
- [x] B 확정: D-B94 D-086은 **Core 버그**(same-RID tcp replacement가 pair ID 재사용) 수정 `7ffb8e55d9`; D-B95 java monitor ABI 수정 `c9d294c44f`(inproc/CLOSED id는 Core 후속, Poller monitor 등록은 spec gap); D-B96 reciprocal HANDOVER spec대로(패배 lane standby 유지) → dotnet settlement 삭제 근거
- [x] cpp 2단계 terminal 종류(route retry 소진 → Unavailable, admitted reply timeout만 DeadlineExceeded) 커밋; node m6b InvalidState = fake STREAM에 `submit_sync` 없음(fixture 수정 커밋) → 남은 red는 runtime `deliver()` catch-all(backpressure를 세션 종료로 처리) → 진단 job; parity 후속: dotnet/java Actor Join·node STREAM bind의 pre-admission 소진이 DeadlineExceeded(진단 예정)
- [x] node: DeliveryDispatch NotFound = direct resolver가 owner-lease-unavailable을 NotFound로 축약(B, `927e3a0f68`); STREAM deliver catch-all = async terminal + typed 분류(B, m6b 112/112)
- [x] spec 결정 (a) Core §4: handover로 밀려난 REQUEST는 즉시 `REQUEST_NOT_CONNECTED`(`bb730c654f`) — Core 구현은 여기서 astra로 진행 중(worktree a); Core polling §3 disconnect progress 검증 job(worktree b)
- [x] java durable replay(A+B): `ZLinkJavaDurableRequest` 단일 소유, core test 1221/0 — **M6A 2건 해소**(재빌드된 java binding D-B95)
- [x] node STREAM bind deadline 단일 소유(B, `8e509bb143`); Core 작업 7 stranded REQUEST 즉시 NOT_CONNECTED(`8b82d51b75`); Core 작업 4 = B D-B104(`0c39ed2e52`) + 여기서 발견한 completion-poller/monitor-lease 결함(`7cbf12de41`, 24 case 120/120)
- [x] dotnet Mesh 걷어내기(D 제거, `3975cea255`, −640줄): admission 8/8·foundation 59/59·샘플 green; 잔여 red = 동시 reciprocal을 Core가 same-direction replacement로 분류(B1, Core 조사 job) + durable create 2 s 소진(B2, Core 7로 해소 예상) → rebuild4(10:55) 후 gate v8 재검증 중
- [x] Core: 동시 reciprocal 오분류 = inproc connect-before-bind에서 locally-initiated flag 유실(B, `0145f5a59a`, 800/800); REJECT 정책 spec 결정 D-088(중복 pipe 즉시 close, `fadfac1c4e`) → Core job 진행 중; monitor-lease 수정(`7cbf12de41`)의 close 회귀 D-089(completion 해제가 executor 재시작 → `zlink_close` 정지, cpp 샘플 137) → Core job 진행 중
- [x] cpp replay 3차(A, completion NOT_CONNECTED replay, `b76d8c1e00`); cpp m6b 미승인 request red = fixture가 hello DATA 미수신(B) → fixture job
- [x] dotnet durable sender `ZLinkDurableRequest`(A, `d6ec19765e`, 16/16); dotnet ClientServer second poller·수동 reconnect 삭제 1차(−280줄, fixture REJECT→production HANDOVER 정렬 2차 진행 중)
- [x] Core close 회귀 수정(`cb62cb89f8`, admission scope 단일화, 200/200, cpp 샘플 exit 0); cpp m6b fixture(`d50d474f63`); dotnet ClientServer 걷어내기(`05b53a8098`, reconnect 소유자 3→1); node ClientServer 걷어내기(`619a09043d`, 4→2); runner SIGKILL 보고 통일(4언어)
- [x] rebuild5(12:54, Core `0145f5a59a`·`7cbf12de41`·`cb62cb89f8`) → **dotnet gate v9: 나머지 1939/1939(Stateful reciprocal 5건 전부 해소), Canonical 14/16** — 잔여 2건(`HandoverLeavesReplyRouteToCore`, lost-reply fixture가 삭제된 5초 cut에 의존) astra 진단+수정 job
- [x] Core D-090(`40137f1bd0`, REQUEST 종결 규칙 3→1: submit 시점 pair 종료 → 원인 불문 즉시 NOT_CONNECTED; `pipe_peer_terminated()` 단일 호출자, REJECT 중복 pipe 즉시 close, 계약 테스트 `test_router_reject_duplicate`) — 검증 171/171 기능·REJECT/close 5/5; B의 D-B112(`349040d3e6`, ws/tls identity·inproc owner·REJECT close)와 병합 `d21e5308b6`, §4 "자기 timeout으로 종결" 문구는 D-090으로 대체(D-091, ko/en·`zlink_disconnect` 단락 포함)
- [x] dotnet Canonical 16/16(fixture `4b6dfe4a7e`); dotnet runners SIGTERM + cleanup 단일 소유(`8e76335988`)
- [x] 병합 HEAD Core 전체 gate 172/173(hotpath n/a); rebuild6(14:47, Core c5f62fb1…) → dotnet/cpp/java 패키지, node 재설치
- [x] node teardown 1·2차 커밋 `8159b15752`(host shutdown 소유 결함 5건 + ZoneWorld runner 중복 SIGKILL + SupportChat idle 시각, 규칙 16→7; 7/7 샘플·aggregate exit 0, SIGKILL 0) + 공유 ZoneWorld UI 키 listener `useLayoutEffect`(npm test 마지막 1 red)
- [x] dotnet 종료 outcome B1–B3 커밋 `d3ea1e4223`(shutdown 중 actor relocation 시작 제거, force-stop 순서, authority Delete CAS 경합 규칙 통합; 6/7 샘플 Stopped/None); dotnet SampleRegressionTests 7건 = 8월 말 공통 증거 계약 drift → 테스트 정렬 `a5a5df5f9b`(157/157)
- [x] Core: 명시적 endpoint/RID 제거 시 ROUTER logical-RID REQUEST가 NOT_FOUND 대신 (D-090 이후) NOT_CONNECTED/(이전) TIMED_OUT — pending matcher가 DEALER만 선택 → correlation pipe 단일 규칙 `ccb418b6ee`(12 case 계약 테스트, 173/174)
- [x] java contractTest 2 red = 문서 경로 drift + boundary whitelist(B의 receive-owner 리팩터가 쓰는 `ReceiveFlowState`) `594821570d`
- [x] Core 회귀 2건 수정: java suite abort `pipe.cpp:210`(inproc pending-connect가 bind 쪽 preamble을 admission 뒤에 write; 순서 2→1) `b63f79a3ce`(Claude agent — codex는 content filter로 중단); D-092 결정(REJECT된 pipe의 DISCONNECTED를 app recv 없이 관찰) → Core job 진행 중
- [x] dotnet ShoppingMall B4(canonical root에 로컬 CAS 혼입 → stage 분리, source가 target 자체 publication 관찰, Preserve eligibility, Admit 물리 방향 재필터 제거; 규칙 6→2) `3810baab93`; unit v10 1948/1948 + 16/16, SampleRegression 157/157, 6/7 샘플 Stopped/None
- [x] rebuild7(15:57, Core 1c7887c3…: D-090·D-B112·NOT_FOUND·inproc preamble + B node binding D-B113) → 직렬 gate7: node 7샘플 aggregate exit 0, npm test 1591 중 2 fail(ClientServer 5 s cap 8.5 s·sample-regression 내부 runner — 동시 java 샘플/gradle 부하; 단독 재실행 36/36 통과), java 7샘플 runner exit 0이지만 Bingo session-b/play-b `FORCE_STOPPED/TEARDOWN_FAILED`(runner가 실패로 판정 안 함), java core test 1234/1 (`descriptorFenceReplacesEndpointOnlyIntent` class run only), cpp unit `mesh_node_vertical` permits_in_use assert(샘플 미실행)
- [x] Core D-092 + tcp same-RID HANDOVER pair `599b4a75ef`: pipe 종료 시점에 DISCONNECTED·inproc 재연결 게시(관찰 규칙 2→1), READY pair와 같은 RID의 새 handshake는 새 pair id를 받아 ROUTER admission이 HANDOVER/REJECT 결정(2→1), snapshot의 숨은 command drain 제거; 계약 테스트 100 case 10/10, 전체 175/176(hotpath n/a)
- [x] dotnet D-093 구현 `0097062421`(lifecycle-terminal 0→1, admission deadline 소유자 3→1): ZoneWorld G4 ×2 + aggregate에서 `Unavailable`; 잔여 = E5 재시작 admission 미완료(same-RID 재연결 → Core pair 수정 대상), replacement create 104 1회, DirectReplyCompletionRegistry wall-clock 단언 1건
- [x] java `ad993ce9b3`: durable sender D-093 rule 1, ZoneWorld sample 오류 kind 보존(DEADLINE_EXCEEDED→"Unavailable" 통합 제거로 G4 은폐 해소), runner 종료 판정 단일화(PID 배열 밖 verifier, G4 표식은 client 표식 필요); java binding `61f227a4b1`: shutdown→close 시 completion pump control send TERMINATED(Bingo session/play·ZoneWorld gateway TeardownFailed 원인) — 규칙 2→1
- [x] cpp `402884255f`: mesh_node_vertical permits 단언은 테스트 결함(공유 queue 예약 관측) → spec 계상식 검증으로 교정, unit 41/41·샘플 7/7
- [x] rebuild8(17:14, Core f20f5cdb…: `599b4a75ef` D-092/HANDOVER pair + java binding pump close + B D-B113~115; java runner Core 경로 override 제거로 version-sync 정렬 `329541bff1`)
- [x] (기록) **gate8 결과(17:37)**: cpp unit+7샘플 PASS; java Bingo·TicTacToe PASS(binding 수정 확인), DeliveryDispatch courier-session TeardownFailed(runner가 정상 판정); node TicTacToe·Bingo PASS, DeliveryDispatch 브라우저 `DeliveryStatusNotify` out-of-sequence; dotnet Bingo client `BingoNumberDrawnNotify` 미수신(exit 134), ZoneWorld ZW-B6 relay=0·B8 ProtocolError, SampleRegression 157/157, unit main 1956/1957(`DurableSenderPreservesExhaustionCause…` withheld 1건, 간헐 재발); java core test 1176/2(`observedInprocCloseDoesNotFenceDescriptorReplacement` 신규 + `descriptorFence…`); npm test 2 fail(5 s cap 8.5 s 재현, sample-regression=DD 영향) — **세 언어 STREAM 샘플이 같은 rebuild에서 동시 회귀 → Core `599b4a75ef` 회귀로 판단, astra xhigh job(worktree a)**
- [x] **환경 원인 확정(17:50)**: WSL2 `systemd-timesyncd`가 호스트 시간 동기화와 충돌해 11:43부터 wall clock이 ±5 s 점프(817회) → 마스킹 후 drift 0. Core STREAM job은 `599b4a75ef` 회귀를 재현하지 못했고(TCP/WS 다중 client 전달 10/10) 대신 STREAM `disconnect_rid` DISCONNECTED 누락(기존 결함)을 발견 → Core job. dotnet Bingo client 로그에 시계 역전(17:21:17→22→17) 확인 — gate8의 시간 조건 red는 클록 점프 영향 가능성, gate9에서 재판정
- [x] 17:50~18:35 커밋: java DD courier-session teardown(binding 정리 규칙 2→1 + 종료 원인 event 한 줄) `ad993…`→`4d38c4f9e7`; java mesh transport identity(귀속 2→1) `ea0d47e1b8`; Core STREAM `disconnect_rid` DISCONNECTED(관찰 claim 2→1, 176/176) `47a8f4329b`; dotnet monotonic durations(183 site/53 file, unit half 1966/0) `29f990286f`; node monotonic durations(114 site) `9981c9fd6e`; node DD out-of-sequence = 클록 점프에 의한 offer deadline 소진(코드 변경 없음)
- [x] Core D-094 `08e5b9c183`(admission 특례 제거 3→2, RID 해제를 논리 종료 edge로, tcp alias term_endpoint 귀속; 176/176) + D-096(같은 RID 동시 count-2 attempt는 §4.1대로 충돌·재시도, 새 wire 식별자 없음 → 인위적 tcp 동시-intent 테스트 case 제거); node ZoneWorld Ops 상태 보고 소유자 2→1 `175f44f586`
- [x] rebuild9(18:44 Core 2055a581… + 19:30 node 재설치) → **gate9a(안정 시계)**: cpp unit+7샘플 PASS; java 7샘플 PASS(kotlin DD courier-session만 TeardownFailed → job); dotnet aggregate 7/7 PASS(Bingo·ZoneWorld 포함 — 클록 점프 red 해소 확인); dotnet ZoneWorld 2회차는 E5 재시작에서 sealed node가 drain 중 Hello 재발행 → admission 대기 초과(간헐) → job
- [x] gate9a 완료(19:55): cpp 전부 PASS; java 7/7·kotlin(DD 수정 후 7/7) `53cafe6b87`; dotnet aggregate 7/7, SampleRegression 157/157, unit main 1969/1969 + join 16/16; java core 1245/1(`descriptorFence…` class run) ; dotnet ZoneWorld 2회차 E5 = 교차 Hello/Admit의 Idempotent 경로가 완료 로그를 생략 + seal 뒤 Hello 재발행 → D-097(§14 보강, spec 커밋 `0ef12195eb`)
- [x] **codex 사용량 한도 도달(19:47, Sep 11 19:46까지 불가)**: 진행 중이던 3 job이 중단 → Claude general-purpose agent로 대체(node F4 timer nominal tick 중복 = Framework spot-timer 결함 B 승인; dotnet D-097 구현; java descriptorFence class-run)
- [x] 3 agent 결과 커밋(20:30~21:10): node spot-timer nominal index 중복(2→1) + ZoneWorld client F3/F4 규칙(3→1) `bc089b7a6c`(ZoneWorld 3/3, aggregate 7/7, npm test 1604/1604); java mesh intent close 규칙 2→1 + inproc 예외 제거 `ea71eab005`(M6A 28/28 ×3, core 1274/0, contract 96/96); dotnet D-097 `d3d5ac66e9`(seal 뒤 Hello 0, idempotent 완료 진단 통합; ZoneWorld 3/3 E5 admission 관찰, aggregate 7/7, unit half 1975/0)
- [x] **cross-language E2E(rebuild9 Core) green**: cpp all-stage passed, node smoke 19 ok, java-cross passed(21:04)
- [x] D-097 parity: cpp `64c2fc7e7c`(seal은 app_state draining atomic 재사용; Draining descriptor·Update 게시; duplicate admission tail 통합; unit 42/42, 샘플 7/7); java `4a76f8b489`(seal predicate = ZLinkMeshDrainCoordinator; publish loop 3→1; + raw mesh admission 관찰 순서 결함 4건 2→1 — M6A 28/28 ×10, core 1256/1256, contract 96/96; java+kotlin aggregate exit 0 ×2)
- [x] Core `d8b65141a4`: zlink_close가 bound endpoint 등록을 반환 전에 해제(reaper 지연 → 즉시 rebind EADDRINUSE 2991/3000 → 0/3000; 규칙 2→1; ctest 179/179). codex 한도 2차 소진(21:5x) 뒤 Core/java 잔여는 Claude agent
- [x] **rebuild10(Core 785b647b… = `d8b65141a4`) → gate10 전부 green**(위 §D 항목 갱신) + cross-language E2E green(23:52). 유일한 잔여: java core:test 1256 중 `ZLinkActorClientMessageFollowRuntimePortTest…RefreshesTheNextCall` 1회 간헐(단독 3/3 통과) → Claude agent가 경합 원인 분석 중
- [x] java unit 간헐 3건 근본 수정(Claude agent): Update가 pending 물리 candidate를 소비해 admission 마커를 지움(candidate 바인딩 규칙 3→1) `f03e6aae99`; 연결 종료 게시 1회화 + idempotent Hello/Admit 마커 미삭제 + registry 테스트 stage 대기 `3b5274a809`; manual fanout open 소유권(요청자가 open을 소유, tick은 빈자리만) `009accd83a` — full core gate 1259/1259 ×2, contract 27/27; java+kotlin aggregate exit 0 재확인(2026-09-06 00:3x)
- [x] **§F 마감(2026-09-06)**: 최종 HEAD `009accd83a`(origin/main 동일). 잔여 red 없음. 후속(별도 추적): zlink_unbind(inproc, 2-lane) ~200 ms 지연; tcp SO_REUSEPORT가 fd 비동기 close를 가림(spec-gap 후보); dotnet WaitForDescriptorPropagationAsync 시간 대기; java raw mesh terminal retention wall-clock(D-095 잔여); sealed node의 inbound Hello Admit 응답 여부(§14 해석); M6A/TransportIdentity fixture의 불필요해진 retry 대기 정리(리팩터 체크포인트)
- [x] (기록, 전부 해소) job 5: Core STREAM 회귀(worktree a), node DD out-of-sequence, java DD courier-session teardown(+종료 원인 로그 단일화), node ClientServer 5 s cap, java M6A class-run 2건. 대기: dotnet `DurableSenderPreservesExhaustionCause` 간헐(Core 수정 뒤 재판정)
- [x] (기록) Core 회귀 수정 → rebuild9 → gate9 → cross-language E2E → §F 마감
- [x] (전부 완료: rebuild4~10, dotnet Mesh 2단계 `3975cea255`, Canonical 16/16, ClientServer poller/reconnect 제거 `05b53a8098`, java 합성 connection id·errno 표 제거(B D-B95/D-B97 뒤 framework에는 진단 문자열 2곳만 잔존), cpp replay `b76d8c1e00`, 최종 gate10) 로컬 패키지 재빌드(`7ffb8e55d9`·`c9d294c44f` 포함) → dotnet Mesh 걷어내기 2단계(brief `stage2-dotnet-mesh-unwind.prompt`) → Canonical 3 red 재검증 → dotnet ClientServer second poller/수동 reconnect 제거(B 작업 4 결과 대기) → java 합성 connection id·errno 표 제거(B 작업 5·6) → cpp replay 범위 진단 → 4언어 최종 게이트 재실행


### G. 잔여 이슈 일괄 수정 (2026-09-06, 사용자 지시 "남은 이슈도 모두 수정") — D-098
- [x] Core `1899e82f1a`: tcp/ipc/ws/tls/wss listener fd 해제를 close/unbind 반환 전 완료(endpoint-release command completion; SO_REUSEPORT 제거) + inproc 2-lane unbind peer progress(191 ms → <300 µs); 규칙 4→2, ctest 180/180
- [x] dotnet `147b2b07ae`: sealed inbound Hello 무응답 + propagation 시간 대기 제거(2→1); ZoneWorld ×2 seal 뒤 Hello/Admit 0, unit half 1978/0, aggregate 7/7; 테스트 fixture 시간원 32곳 Stopwatch `ebab877246`
- [x] java `81dad6324d`: sealed inbound Hello 무응답 + retention nanoTime + fixture retry 제거 + (드러난 결함) closed intent terminal(늦은 READY 재활성화 금지, D-098 item 8) — M6A/Seal/Identity ×10, full core gate ×2 0 fail, java 7/7 + kotlin ZoneWorld ×2(이전 B7 1회 실패는 미재현·원인 미확정으로 추적)
- [x] cpp `b890e2e718`: sealed inbound Hello 무응답; unit 42/42, 샘플 7/7 재확인
- [x] (범위 외로 확정, 2026-09-06 사용자: e2e 요구는 cross-language E2E만) cpp `common_e2e_inventory` 278 open(cpp 공통 e2e 시나리오 122개 미구현 등) — job 중단·편집 폐기, 별도 계획으로 남김
- [x] rebuild11(Core 8c547d87… = `1899e82f1a`) → gate11(2026-09-06 04:21~05:07): java 7+kotlin 7 PASS, node 7 + npm test 1604/0, dotnet 7 + aggregate + ZoneWorld×2 PASS, dotnet unit 1978/0 + join 16/0 + SampleRegression 157/0, **java core/contract 0 fail(간헐 3건 근본 수정 뒤 첫 전체 green)**, cross-language E2E(cpp all-stage·node smoke 19·java-cross) passed
- [x] cpp ZoneWorld EADDRINUSE = cpp framework 결함(discovery publish 뒤 manual publisher 경로까지 실행 → 같은 주소에 두 번째 XPUB bind; SO_REUSEPORT가 가림) `bd0d499189`(ZoneWorld 3/3); cpp DeliveryDispatch readiness marker 누락 = sample의 stdout 행 경합(reporter 3→공유 streambuf 1) `6cd7b172ce`(샘플 7/7)
- [x] cpp M6A abort 원인 = **Core 메모리 안전성 결함(B)**: PUB/XPUB에서 `attach_pipe`가 이미 종료 완료된 pipe에 `dist_t::attach` 없이 조기 반환(_array_index=-1)하는데 `pipe_terminated`는 `dist_t::pipe_terminated`를 호출해 `array_t::erase(-1)`로 버퍼 앞 8바이트 OOB write(ASan 스택 확보, 공개 C-API 재현 `pub_churn.cpp` 55/200 process 재현)
- [x] Core `f57c73b040`: dist_t/lb_t::pipe_terminated가 fq_t처럼 membership을 확인(attach/terminate 규칙 2→1); 공개 API 회귀 test_pubsub_churn_dist(ASan 0 error, 부하 10/10), 전체 ctest 181/181
- [x] **§G 마감(2026-09-06 07:24)**: rebuild12(Core 23673ae4… = `f57c73b040`) → cpp unit ×3 43/43(M6A abort 미재발) → gate12 전부 green: cpp unit+7샘플, java 7+kotlin 7(host role 전부 Stopped/None), node 7 + npm test 1604/0, dotnet 7 + aggregate + ZoneWorld×2, dotnet unit 1978/0 + join 16/0 + SampleRegression 157/0, java core/contract 0 fail → cross-language E2E(cpp all-stage·node smoke 19·java-cross) passed. 최종 HEAD = 이 커밋(origin/main 동일). 미커밋 잔여 없음(provenance 산출물 제외). 범위 외로 남긴 것: cpp `common_e2e_inventory` 278(공통 e2e 시나리오 구현, 별도 계획), kotlin ZoneWorld B7 1회 실패(미재현·원인 미확정, 예외 상세 손실)

### H. pull 뒤 전체 재검증 (2026-09-06 07:4x~, 사용자 지시)
- [x] pull: B의 Core `bf28780d51`(STREAM fragment drain) 1건 유입 → rebuild13(Core 083588b4…)
- [x] green: cpp unit 43/43 + 샘플 7/7, java 7 + kotlin 7, dotnet 7 + aggregate, dotnet SampleRegression 157/0 + unit main 1978/0, java core/contract 0 fail
- [x] red 3건 전부 근본 수정: node gateway crash = node framework가 이미 없는 세션의 `disconnect_rid` NOT_FOUND를 치명 예외로 전파 → 완료된 close로 처리(핸들러 2→1) `62ee78e3f5`; Core `bf28780d51`은 긴 partial packet drain이 poller 진행을 독점 → bounded step + 재wake `7738b8fd41`(B 의도 유지, 182/182); dotnet ZoneWorld G3→A1 = outbound intent 제거 시 native endpoint 등록 잔존(판정 2→1, D-100) `28c6c567ae`; node replay 테스트는 fixture 시간원 결함 `d9f6fc24b6`
- [ ] 진행 중: rebuild14(Core `7738b8fd41` + B `0add1dd621`) → cpp unit → gate14(4언어) → cross-language E2E → §H 마감
