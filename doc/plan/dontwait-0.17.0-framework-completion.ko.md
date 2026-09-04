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
- [ ] **cpp m6a 잔여**: `verify_client_server_plain_hello_is_rejected` 행(단독 실행도 timeout) — job `bucketB-cpp-m6a-plain-hello` 진행 중
- [x] **dotnet stale-authority 3건**: 원인 = spec(README §4 HANDOVER)이 정한 패배 방향 request timeout. Framework가 Core handover 수렴 전에 `Admitted`를 공개한 것이 결함.
      수정 = settle-before-submit(패배 outbound intent를 endpoint로 disconnect, 패배 방향이 ready였다면 두 lane Disconnected 처리 뒤에만 Admitted 공개), 회귀 `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`.
      3건×3회 9/9, Stateful suite 50/50. 전체 unit gate는 감독관이 실행 중(커밋 대기). generic request 자동 재전송은 spec(02-spot-messaging:392-403, 02-session-actor-binding:626-634)이 금지하므로 미도입
- [x] **node 신규(B)**: `user-spot-native-two-process` — 첫 request가 deadline 전부 소진 → replay 여지 없음. 수정 커밋 `285cfa522a`
- [x] 판정: cpp 4건·dotnet 1건·node 1건 모두 framework(+테스트 하네스) 결함. Core·binding 버그 0건 (Core 수정 job 1건은 오판으로 중단).
- [ ] 수정 + 재검증 + 커밋 — cpp 4/4·node 1/1 커밋, dotnet 커밋 대기(전체 unit gate)

### C. 검증 인프라 (신뢰성 확보)
- [x] 환경 재구축(2026-09-05, WSL 재설치): Core dev 빌드, **로컬 Core 0.17.0 프리픽스 수동 구성**(GitHub에 `core/v0.17.0` 릴리스 없음 — `fetch-release.sh` 캐시 단락 경로 이용), 4언어 로컬 패키지,
      cpp 의존성(apt boost/gtest/gmock/lz4/libuv + hiredis·redis++ 소스 빌드), node `http-client` tarball 재생성(`npm pack ./packages/http-client`), Docker Desktop WSL 통합, Playwright Chromium
- [x] cpp 샘플 신뢰성 있는 실행 — 별도 build dir `build/linux-ninja-c-e2e`(protobuf include 수정 커밋) 에서 **7/7 PASS**(`c-cross-language-e2e-2-summary.md`)
- [x] cross-language E2E: C++ host stale 문제는 caller가 `ZLINK_CPP_BUILD_DIR` 미지정이 원인(runner는 이미 우선 계약) — 0.17 host로 재실행. C++↔.NET·C++↔Node messaging/flow/stream PASS, C++→.NET spot-route PASS.
      **잔여**: `.NET→C++ spot-route` `Unavailable`(B, dotnet client측) 및 node smoke의 .NET TestHost flow Activity tag 불일치(D) — dotnet handover job 종료 후 순차 job
- [x] `java-cross` selector(Java↔Node/.NET) 4/4 통과. Java↔C++ 방향은 all-stage runner 후반이라 위 .NET→C++ 실패 해소 후 실행

### D. 최종 게이트 (plan line 143: framework unit + cross-language E2E + 7 samples 전부 green)
- [ ] 4언어 framework unit (gate-v2, D-B85 바인딩 반영 패키지): **A/B 실패 0**.
      java 1206/3 = pre-existing C 3 + 신규 F 1(`EntrySpotActorDispatchTests…staleTerminal` relocation seal 부재, job 진행);
      node 1552/5 = C 4(ZoneWorld dist 미빌드 3·lint 1) + F 2(SupportChat lifecycle 테스트가 옛 3000/1000ms 하드코딩 — 내 budget 커밋 `2e3b1b47e4`의 후속, TicTacToe.Ts PASS 마커 — job 진행);
      cpp gate-v2 63/69 → stream-connector fixture(RAW mode before bind, spec 08-stream §2) `dbfcf7d6fe`·lz4 packaging `20b94c3457`·package-test config `4573c09a2a` 수정 후 잔여 = inventory 278(C)·m6b 1909 flake(C);
      dotnet 전체 unit gate 감독관 실행 중(handover 수정 커밋 대기)
- [ ] 7 samples × 4언어 green — **cpp 7/7**; node 5/7(ZoneWorld `vite` 환경 D, SupportChat 브라우저 stream timeout B — job); java 1/7(TicTacToe·SupportChat TEARDOWN_FAILED·Bingo session-disconnect = pre-existing C, DeliveryDispatch·GameQuest·ZoneWorld timeout = B, D-B85 binding-port 의존 여부 판정 job); dotnet 미실행(handover job 종료 후)
- [ ] cross-language E2E green
- [ ] 최종 커밋+푸시

### E. 문서/사이트 (2026-09-04 추가)
- [x] docs PR #2 (영문 자연화 + archive 삭제) 머지 `3f0f02d478`, 가이드 재생성 `baa4b4e4f6`, 앵커수정 `8f8f75ff71`
- [x] **docs 사이트 퍼블리시 완료** (GitHub Pages 배포 성공)
- [x] "cpp async-only submit projection" 계약 실패는 로컬 false-failure로 판정(CI 통과) — framework API 추가 불필요
- [ ] **[사용자 요청] framework 가이드에 메시징 API 종결자 의미·사용법 추가** — 05-channel-messaging에 개념 절 신설(04 §3.1 중복 금지). 진단·제안 위치는 HANDOFF §4.2


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
