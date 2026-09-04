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

### A. DONTWAIT send/POLLOUT framework fix
- [x] cpp framework 적응 (raw_dealer_port·raw_route_port·channel_outbound_exchange·stream_host_service) — `channel_messaging` GREEN
- [x] dotnet framework 적응 (ZLinkBackendStreamSocketWrapper[D-084]·SubmitFailureMapper·ManagedMeshNode·SpotOutbound{Endpoint,Transport}) — binding WRITABLE 계약 22/22, backpressure-retry GREEN(epoch fallback)
- [x] node framework 적응 (node-socket-backend-adapter: sync 분기 제거) — 샘플 all PASS, 테스트 fail 0
- [x] java framework 적응 (ZLinkJavaRawServicePort·ZLinkJavaSocketReceivePoller: POLLOUT|POLLCOMPLETION 구독 + state-lane) — Java↔Node/.NET E2E 4/4
- [ ] **커밋+푸시** (13파일, +181/−46) — 이 커밋 진행 중
- [x] 스펙 준수 확인: POLLOUT은 미읽은 WRITABLE 있을 때만 level, drain하면 해제 (busy-spin=스펙위반→Core/B)

### B. terminal/error 분류 회귀 (별개 클러스터 — 근본원인 규명 필요)
- [ ] **cpp m6a**: `records.size()==1` (bound-session bind) 실패 — 근본원인
- [ ] **cpp m6b**: `transport_failure.error_kind()==deadline_exceeded` 실패 — 근본원인
- [ ] **dotnet stale-authority 3건**: `TimedOut(101)` vs stale terminal(107) — 근본원인
- [ ] 판정: framework 분류 버그 vs Core 버그. **Core 버그면 A가 직접 수정+커밋+푸시(sub-agent), Core는 B가 perf만**
- [ ] 수정 + 재검증 + 커밋

### C. 검증 인프라 (신뢰성 확보)
- [ ] cpp 샘플 신뢰성 있는 실행 (공유 build cache 동시경합으로 종료결과 미확보 → 격리 필요)
- [ ] cross-language E2E full: dotnet의 C++ host가 **Core 0.13.2 stale artifact에 고정**돼 0.17 수렴 실패 → C++ host 재빌드
- [ ] Java↔C++ E2E (선행 C++→.NET 실패로 미실행) 재시도

### D. 최종 게이트 (plan line 143: framework unit + cross-language E2E + 7 samples 전부 green)
- [ ] 4언어 framework unit: DONTWAIT 회귀 0, 잔여는 pre-existing만 (java M6A 2·doc-regression 1은 pre-existing)
- [ ] 7 samples × 4언어 green
- [ ] cross-language E2E green
- [ ] 최종 커밋+푸시

## 알려진 pre-existing (회귀 아님, 별도 추적)
- node lint `spot-timer.ts:137` (2026-08 이후 불변, DONTWAIT 무관)
- java M6A 2건 + JavaDocumentationRegression 1건 (0.16 전환 커밋에도 존재)
- cpp `common_e2e_inventory` 278 (feature-map 미충족 inventory gate)
