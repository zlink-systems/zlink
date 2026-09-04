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
- [ ] **dotnet stale-authority 3건**: 분류 오류가 아니라 **stall** — owner가 terminal reply submit OK(트레이스), caller가 completion을 못 받아 3초 뒤 `ExpireOperationAsync`가 TimedOut(101).
      Core 최소 재현(ROUTER↔ROUTER 양방향 + POLLCOMPLETION poller)은 정상 → framework/binding 경계. job `bucketB-dotnet-stale`(sol/xhigh) 진행 중
- [ ] **node 신규(B)**: `user-spot-native-two-process` `RequestError: request failed` — job `bucketB-node-two-process` 진행 중
- [x] 판정(현재까지): cpp 2건 = framework 매핑/연결 소유자 버그(Core·binding 아님). dotnet·node 미판정.
- [ ] 수정 + 재검증 + 커밋 — cpp 2/4 완료

### C. 검증 인프라 (신뢰성 확보)
- [x] 환경 재구축(2026-09-05, WSL 재설치): Core dev 빌드, **로컬 Core 0.17.0 프리픽스 수동 구성**(GitHub에 `core/v0.17.0` 릴리스 없음 — `fetch-release.sh` 캐시 단락 경로 이용), 4언어 로컬 패키지,
      cpp 의존성(apt boost/gtest/gmock/lz4/libuv + hiredis·redis++ 소스 빌드), node `http-client` tarball 재생성(`npm pack ./packages/http-client`), Docker Desktop WSL 통합, Playwright Chromium
- [ ] cpp 샘플 신뢰성 있는 실행 — 별도 build dir `build/linux-ninja-c-e2e` 구성 완료, cpp job 종료 후 `c-cross-language-e2e` job으로 실행
- [ ] cross-language E2E full: dotnet의 C++ host 0.13.2 stale artifact → 같은 job에서 해소
- [x] Java↔C++ E2E: `java-cross` stage 4/4 통과 (`gate-node-bootstrap-summary.md`)

### D. 최종 게이트 (plan line 143: framework unit + cross-language E2E + 7 samples 전부 green)
- [ ] 4언어 framework unit: DONTWAIT 회귀 0, 잔여는 pre-existing만 — java unit 1207/2·contract 26/1 = pre-existing 3건 동일 assertion 확인(`gate-node-java-summary.md`); node 1532/6(신규 B 1건 + 환경 D 5건, lint C); cpp·dotnet §B 진행 중
- [ ] 7 samples × 4언어 green
- [ ] cross-language E2E green
- [ ] 최종 커밋+푸시

### E. 문서/사이트 (2026-09-04 추가)
- [x] docs PR #2 (영문 자연화 + archive 삭제) 머지 `3f0f02d478`, 가이드 재생성 `baa4b4e4f6`, 앵커수정 `8f8f75ff71`
- [x] **docs 사이트 퍼블리시 완료** (GitHub Pages 배포 성공)
- [x] "cpp async-only submit projection" 계약 실패는 로컬 false-failure로 판정(CI 통과) — framework API 추가 불필요
- [ ] **[사용자 요청] framework 가이드에 메시징 API 종결자 의미·사용법 추가** — 05-channel-messaging에 개념 절 신설(04 §3.1 중복 금지). 진단·제안 위치는 HANDOFF §4.2

## 알려진 pre-existing (회귀 아님, 별도 추적)
- node lint `spot-timer.ts:137` (2026-08 이후 불변, DONTWAIT 무관)
- java M6A 2건 + JavaDocumentationRegression 1건 (0.16 전환 커밋에도 존재)
- cpp `common_e2e_inventory` 278 (feature-map 미충족 inventory gate)
