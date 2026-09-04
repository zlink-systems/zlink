# 0.16.0 캠페인 감독 판정 기록 (2026-09-02 17:30 재개 — 이전 D-001~D-020은 /tmp 소실)
요약 복원: D-008 flow-state stale=epoch만 public; D-010 Java Socket 공통 표면=options/monitorOpen/TLS/close;
D-011 Node publishAsync 삭제; D-013~D-016 Phase 2~4 PASS(105/105); D-017 perf baseline=worktree 빌드
(`--core-version` 금지); D-018 PAIR 회귀=blocking path per-send 비용(커밋 04ecca54d1); D-019 lane A안
승인(DEALER-DEALER count 1, VERSION 0x01); D-020 lane 스펙 리뷰 6건 반영(커밋 157aaf837e).

## D-021 (2026-09-02 17:30)
lane 구현 resume job 재투입(sol ultra, systemd-run scope). 잔여 위험 2건(monitor owner TOCTOU,
control-slot flush 분기 전 socket 적용) 필수 처리 지시. 사용자 framework/languages/cpp 변경(DI 작업)
은 어떤 job도 건드리지 않음.

## D-022 (2026-09-02 17:42)
lane 구현 job 대기 중 유휴 자원 활용: Phase 8 stale-API 인벤토리 조사 job(terra high, 읽기 전용,
보고서 c016/phase8-inventory.md)을 병렬 투입. framework/** 범위 밖, core/build 미사용. 문서 수정은
lane Core 커밋·구현 확정 뒤 별도 job으로(plan Phase 8 "구현과 test가 확정된 뒤에만" 준수).
sweep 판정: lane 이전 candidate perf 결과(PAIR 6 transport·PUBSUB tcp)는 stale — lane 커밋 후
첫 cell부터 baseline→candidate 짝비교 재실행.

## D-023 (2026-09-02 17:58) Phase 8 인벤토리 검수 PASS
보고서 c016/phase8-inventory.md 표본 3/3 일치(lifecycle:122 SEND_PENDING·zlink_enum.h:243 stale 주석·
node README:33 RAW/PACKET 현행), 트리 무수정. BLOCKERS 판정:
1. zlink_enum.h:243 "Paired DEALER/ROUTER completion-lane" 모니터 이벤트 주석 → lane 구현 job 범위
   (브리프의 public header 주석 갱신 항목). lane diff 리뷰 체크리스트에 추가: 243 포함 enum.h·
   socket/api.h의 completion-lane 문구 전수 확인.
2. public poller wait() loop 규칙(draft :923-936) 언어별 실제 동작 → Phase 8 문서 job이 각 binding
   소스 확인 후 서술(추정 금지). Node README는 부분만 현행.
3. bindings/doc/reference/** 는 plan Phase 8 명시 대상 → 범위 포함, 생략 없음.
Phase 8 착수 조건: lane Core 커밋 + Phase 7 완료 후(plan "구현과 test 확정 뒤에만").

## D-024 (2026-09-02 18:20) lane resume r1 OOM 사망 → r2 재투입
journal/dmesg: 17:54 test_zmp_metadata anon-rss 55GB → 전역 OOM kill, scope OOMPolicy=stop으로 codex까지
종료(로그 EXIT 마커 없음, r1 로그 lane-impl.r1-oom.log 보존). 부분 구현 트리(core 62파일 +5,290/−839)
보존. r2 브리프에 OOM 근본 원인 우선 수정 + ulimit -v 아래 테스트 실행을 추가. 기동은
systemd-run scope에 MemoryMax=24G·OOMPolicy=continue(테스트 바이너리만 죽고 codex는 실패를 관찰).
감독 교정: Monitor 생존 판정을 pgrep 자기매칭 대신 `systemctl --user is-active <unit>`로 변경.

## D-025 (2026-09-02 20:30, 사용자 지시) Phase 5 sweep 실행 파라미터
5-run 대신 **cell(pattern+transport) 단위 1-run**으로 baseline→candidate 즉시 짝비교. 5% gate 벗어난
cell만 같은 조건으로 재측정해 환경 변동/회귀 구분. 목표 2시간 이내. baseline=worktree 자체 빌드 lib
(`--core-version` 금지) 유지. plan 본문의 `--runs 5` 예시는 사용자 지시로 대체(문서 갱신은 Phase 8
문서 커밋에 포함 여부 사용자 확인).

## D-026 (2026-09-02 20:40, 사용자 지시) Phase 7 = smoke 전용
8언어 perf runner를 plan 명령(--duration 1 --runs 1, multi --clients 1, go/rust/python --smoke)으로 돌려
unexpected skip/fail/hang 없음만 확인. 수치 판정 없음. 이월 항목(cpp perf callback→pull 재설계 3파일,
C bench JVM 컴파일)은 smoke를 실제로 막을 때만 수정. sweep(수치 측정)과 CPU 동시 사용 금지.

## D-027 (2026-09-02 20:45, 사용자 지시) Phase 8 = 30분 목표
인벤토리(phase8-inventory.md) 확정 상태이므로 문서 job 1개가 Core+바인딩 전 대상을 일괄 수정(코드 예제는
sample/contract fixture에서 인용, ko/en 동시). Claude 검수 = 표본 대조 + 링크·렌더·diff --check 게이트.
sweep 진행 중 병렬 투입 가능(빌드·테스트 없음).

## D-028 (2026-09-02 20:50, 사용자 지시) Phase 11 = 1시간 미만
framework 4언어 Core 0.16.0 패키지 전환 + 컴파일·API 맞춤 위주. 착수 직전 사용자 병행 수정 범위
(framework/languages/cpp DI, framework/doc/framework/**)만 확인. Phase 8 문서 job은 lane 완료 대기 없이
지금 투입(텍스트 전용, 파일·CPU 충돌 없음).

## D-029 (2026-09-02 20:55) Phase 8 문서 job r1(terra) 반려
98파일 +546/−19,967: 챕터를 11줄 보일러플레이트로 통째 교체(12-router.en 267→11줄 등). 인벤토리 대상 98파일
전부 `git checkout`으로 HEAD 복원(반려 diff 보존 c016/phase8-docs-r1-rejected.diff). r2는 sol high로,
"섹션 삭제 금지·파일별 numstat 상한·heading 보존" 강제 + 인벤토리 외 stale 2건(rust README callback-only,
05-errors 5언어) 범위 추가.

## D-030 (2026-09-02 21:52) lane r2 요약·diff 표본 리뷰 (EXIT 전 read-only)
- OOM 근본 원인: pending predicate가 monotonic sequence allocator를 slot 존재 표식으로 오용 → 실제 slot
  (weight unset / flow_state_valid) 기준으로 교정. 코드 확인 PASS.
- control-slot guard(peer_control_slots_enabled): pair id/gen + peer D/R + count별 lane. 설계 §4.4 정합 PASS.
- count-1 FIFO head 분류(reclassify_transport_pair_application_head): DATA/REQUEST는 public receive 유지,
  REPLY/control만 completion owner. 설계 §4.3 정합 PASS.
- responder reply route(retain_reply_transport_pipe): source peer type별 lane 선택 + current route 검증. PASS.
- test_router_handover +12줄(선행 요청으로 승자 방향 확정): assertion 삭제 없음, 승인 설계 exact submit-pair
  fence와 정합 → 완화 아님 PASS. **spec gap 기록**: RID 반대 방향 충돌 수렴 중 패자 방향 in-flight request의
  결과(timeout)가 README §RID duplicate·07-router에 미규정 → 후속 스펙 상세화 후보.
- B1: READY protocol error에 HANDSHAKE_FAILED_PROTOCOL event 없음(HELLO 경로 429행만 발생). lane 이전과 동일
  → 회귀 아님, 스펙 공백(enum 어휘). 사용자 판정 대기(enum 추가 vs 이월).
- B2: bindings/cpp Contracts 헤더 4곳 Completion-only 주석 → Phase 7 bindings 커밋에 포함(주석만).
- B3: 4개 버전 파일은 lane job 부작용 판정(사용자 미실행 확인) → EXIT 후 HEAD 복원, 복원 상태로 smoke 재실행.

## D-031 (2026-09-03 00:05) B1 스펙 변경(감독관 직접) + Phase 8 문서 r2 PASS
- B1: 사용자 판정 "지금 enum 추가". 스펙 오류(닫힌 어휘가 01-zmp의 READY protocol error 관측을 표현 불가)로
  06-monitoring ko/en에 `ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY = 0x10000016`(libzmq ZMTP 번호
  체계 정합) + Event 내용 조항 추가. 구현 job(b1-ready-enum, sol high)은 lane 커밋 후 투입.
- Phase 8 문서 r2(sol): 116파일 +2,442/−2,467, 20%+ 축소 0, stale 토큰 0, 링크 깨짐 0, diff --check OK,
  표본 3건 제자리 교체 확인 → PASS. 보류: Kotlin·plain JS PACKET 샘플 callback 잔존(Phase 7에서 확인).
  커밋은 lane Core 커밋 뒤 "docs: update core and binding pull API guides"로 분리.
- 4개 버전 파일 HEAD 복원 완료. 이후 모든 job 브리프에 scripts/local-package/** 실행 금지 명시.

## D-032 (2026-09-03 00:25) lane Core 커밋·push PASS
gate 직접 재실행: build OK, ctest 133/134(유일 실패 contract_public_surface = 제 스펙 READY enum 선반영 기인,
enum job이 해소), test_single_lane_* 29/29 ×3, mirror 12/12, diff --check OK, cpp 7/7·python 144+7/7
(복원된 0.16.0 버전 파일 상태). 커밋 `8b40b3feb2` core lane(85파일), `53b8282e3b` docs Phase 8(116파일), push 완료.
후속: B1 enum job(sol high, c016-b1-enum) 투입 — 완료 후 스펙 2파일과 함께 커밋. Phase 7 smoke 드라이버 terra 작성 병렬.
sweep은 enum 커밋 뒤(candidate 고정) 시작.

## D-033 (2026-09-03 00:40) Phase 7 smoke 드라이버 검수 PASS
c016/phase7-smoke.sh: plan 명령 그대로(8언어 single 42·multi 28 cell, duration 1·runs 1·clients 1·--smoke),
timeout 1200, runtime identity 파싱, 언어별 재개. Go/Rust --smoke는 영구 report 파일을 안 남김 →
D-026(smoke 전용)에 따라 캡처 stdout으로 판정, runner 계약 변경 안 함. Node help preflight 1회(빌드 유발 허용).

## D-034 (2026-09-03 00:55) B1 enum 커밋 `c2d5f33438` push, Phase 5 sweep 시작
B1 gate 직접 재실행 134/134·mirror 12/12·diff-check OK. 스펙 2 + 코드 9 파일 한 커밋. count-2 lane set의
HANDSHAKE_IVL 미완성을 READY protocol error로 재분류(01-zmp 190-193 근거) 수용. 이후 sweep.sh 기동
(cell 1-run 짝비교, D-025), Monitor bxidptcm6.

## D-035 (2026-09-03 03:10) Phase 5 sweep — 실제 회귀 확인, release 중단 상태
single 30+ cell: DEALER_DEALER·DEALER_ROUTER·DEALER_ROUTER_REQREP bandwidth 0.55~0.69(tcp/inproc/ipc), REQREP wss 0.19,
PUBSUB ipc 0.87. 감독관 오류: PAIR inproc latency 42배를 HWM 적재 artifact로 단정해 "문제 없음"으로 표현 → 사용자
반박. 정정: artifact 여부는 조사 job의 실측 항목으로 넘기고 판정 근거로 쓰지 않음. 조치: R/R negative control
12 cell까지만 sweep 후 중단, sol ultra 조사 job(perf-regression.prompt): bisect(prelane 39e7467be5)→hot path
실증→수정→gate. B2 주석 job 완료(29파일 +108/−88, 리뷰 대기).
## D-035 보강 (03:15) single 42 cell 완료 후 sweep 중단. R/R(negative control)도 회귀: one-way tcp 0.94·ipc 0.89,
REQREP tcp 0.78·ws 0.73·ipc 0.77·wss p99 2.7×·inproc latency 1.7×. ROUTER_ROUTER inproc candidate = runner
non_zero_exit_1(기능 결함 의심, 조사 1순위). 조사 job c016-perf-regression(sol ultra) 기동.
감독 실수 3회째: pkill/pgrep -f 자기매칭으로 대기 셸 2회 자멸 → 이후 프로세스 종료는 `ps -eo comm` 정확 매칭 + sid.

## D-036 (2026-09-03 ~05:10) 감독관 직접 수정(사용자 지시 "직접 해결") — perf 회귀 1차 수정
진단(c016/perf-diagnosis.md): 회귀 두 겹 — 주범=pull-completion DEALER 송신 경로(select→endpoint 문자열→
find_pipe_by_endpoint 재해석, vector/string 할당, transport_pairs mutex+map ×2), lane 증분=hot path lock·map·hold.
수정(모두 계약 유지, 새 옵션·제어점 없음):
 1. pipe_t: `_transport_pair_application_ready` atomic 캐시(pair admission에서 set, 첫 detach에서 clear) →
    transport_pair_application_ready()가 lock/map 없이 판정. `_state_active` atomic 미러 → is_lifecycle_active lock 제거.
 2. end_public_part_receive_delivery_hold: hold 없으면 lock 전에 조기 return(플래그 atomic화).
 3. DEALER blocking send/request fast path: 한 scope에서 lb 선택(xselect_routed_submit_pipe) + 선택 pipe로 직접
    admit(try_admit의 selected_pipe 분기, xsend_selected_pipe). 재시도 가능 거절 때만 endpoint 문자열 경로로 후퇴.
    lb::select_connected_pipe 임시 vector → 멤버 scratch 재사용. request 경로는 route history 기억 유지.
 4. poll-edge 결함(R/R inproc ERROR·D/R+monitor 재현): process_async_mailbox가 idle-stop 경로에서
    rearm_primary_signaler 없이 detach → 공개 poller가 timeout까지 잠듦. stop_unowned_..._at_idle에 rearm 추가.
측정(1-run, baseline=0.15.1): D/D inproc 588k→921k(0.93), tcp 658k→1,102k(1.11); D/R inproc 930k(0.98),
tcp 1,040k(1.03); D/R REQREP inproc 342k→397k(0.64), tcp 322k→359k(0.77); R/R REQREP inproc 372k(0.64), tcp 370k(0.86).
gate(3번까지 상태): ctest 134/134, snapshot_accounting 단독 3/3(병렬 시 1회 flake 관측).
잔여: REQREP(요청당 명령어 +35%: submit_pull_blocking_request·public_router_reply_submit·zlink_completion_recv
self 3.7K/req), one-way latency 지표(HWM 적재) 판정은 사용자 몫.

## D-037 (2026-09-03 ~06:30) perf 수정 커밋 + 핫패스 gate 승인
- 사용자 승인("진행해"): 핫패스 규칙 문서(doc/principal/dev/hotpath.ko.md, 작성 완료·미커밋) + callgrind
  명령어/메시지 gate(core/tests/perf/hotpath_gate, terra job 예정) + 인벤토리. 모든 Core job 필수 green.
- reply 대기: sleep_for(1ms) → wait_submit_progress(1ms 슬라이스, 명령 도착 시 즉시). ws/wss REQREP는 1-run
  편차가 커(89k~236k) 결론 보류 — ws flush wait EAGAIN 원인 별도 조사 항목.
- flake: test_single_lane_flow_snapshot_accounting 전체 suite 병렬(-j3/-j4)에서 2회 즉시 실패, 단독 10/10·lane
  세트 병렬 5/5 통과 → load-flake 후보, 재실행 1회 규칙 적용. 출력 미확보.
- single sweep(수정 후) 42 cell: PASS 18 / FAIL 24 — FAIL 대부분 latency 지표(HWM 적재) 또는 0.90~0.95 경계,
  REQREP 0.63~0.86(tcp/inproc). multi sweep 진행 중.

## D-038 (2026-09-03 ~06:50, 사용자 지시) 배정 변경 + 타협 금지
- "이제 codex sol 에이전트로, 고난도는 ultra": 이후 구현은 sol, 큰 이슈는 ultra. 감독관 직접 구현 종료(1344022a3e까지).
- "뭘 자꾸 성능을 타협하라고 하는거야": gate 완화·이월 선택지 제시 금지. 기준 = 전 cell 5% 이내. 벤치 측정 결함
  (one-way latency=queue 깊이)은 gate 완화가 아니라 벤치 수정으로 해결.
- 투입 순서: hotpath gate 도구(sol) → Core 2차(ultra, part_helper·REQREP·ws) + C perf latency 벤치 수정(sol) 병렬.

## D-039 (2026-09-03 04:35) multi sweep — 초 단위 정지 발견, 배정 전환
multi 28 cell: SENDSEND latency p95 1,400~5,000배(candidate 0.5~1.6초), R/R REQREP multi latency 350배·throughput
0.06, D/D multi bandwidth 0.61~0.69. wake/activation 유실 계열(1344022a3e의 rearm 수정 외 경로 잔존 추정).
hotpath gate 도구 job은 8분 만에 중단(최종 트리 기준값을 위해 Core 2차 뒤 재투입). ultra job
c016-hotpath-phase2(D 최우선→A·B·C) + sol job c016-latency-bench(C perf one-way latency 비포화 측정) 병렬 기동.

## D-040 (2026-09-03 04:50, 사용자 판정) 성능 합격 기준 확정
1. cell(pattern, transport, size, metric) 단위 5%는 측정 오차 허용치. (throughput·bandwidth ≥0.95, latency ≤1.05)
2. **pattern×transport의 전 size 평균 성능은 baseline(0.15.1)보다 내려가면 안 된다**(candidate/baseline 평균 ≥ 1.0).
   size 축 = 64·256·1024·65536(plan 5.1 STREAM matrix와 동일). 평균은 size별 ratio의 기하평균(또는 산술평균 —
   gate 도구에서 하나로 고정, 문서화).
3. 따라서 sweep은 1024 단일 size가 아니라 4 size × pattern × transport로 돌리고, gate는 cell 판정 + 집계 판정
   둘 다 PASS여야 한다. latency는 벤치 수정(비포화 측정) 후 같은 규칙 적용.
적용: perf_regression_gate.py에 집계 판정 추가(다음 sol job), sweep.sh --msg-sizes 확장, plan §5.2 문구 보정(감독관),
진행 중 ultra job 결과는 완료 후 이 기준으로 재판정(미달 시 같은 브리프+기준으로 resume).

## D-041 (2026-09-03 05:20) latency 벤치 수정 PASS·커밋, gate 집계 job 투입
- c016-latency-bench(sol): one-way 5 pattern에 in-flight 1 latency 구간 추가, throughput 구간 유지, D/R·R/R를 공통 runner로
  통합. 3파일 양 worktree byte-identical, gate unit 6/6, 검증 mean 0.0% 차. 커밋 push(main); baseline worktree는 미커밋 로컬
  적용 상태 유지(재생성 시 summary의 cp 절차).
- c016-gate-aggregate(sol): perf_regression_gate.py에 (pattern, transport, metric) size 기하평균 집계 판정(≥1.0/≤1.0) +
  sweep2.sh(4 size) 작성. sweep.sh는 ultra job 사용 중이라 미수정.

## D-042 (2026-09-03 05:40, 사용자 지시) 핫패스 규칙을 정식 스펙에 고정
core/doc/spec/core/systems/10-hot-path.{ko,en}.md 신설(범위 표·금지 7항·캐시/후퇴·gate 5.1/5.2·변경 절차), 목차·nav 갱신,
doc/principal/dev/hotpath.ko.md는 포인터로 축소. 커밋 push. 이후 모든 Core job 브리프는 이 스펙 §3·§5를 필수 인용.

## D-043 (2026-09-03 05:55) gate 집계 판정 커밋
perf_regression_gate.py: 반복 report 병합 + (suite,pattern,transport,metric) 4-size 기하평균(≥1.0/≤1.0) + 누락 FAIL,
unit 12/12. sweep2.sh(4 size 드라이버, sweep2-results.md) 준비. 공식 판정은 ultra 종료 후 sweep2.sh로 감독관이 실행.

## D-044 (2026-09-03 07:30, 사용자 지시) posddd 리팩토링 병행
성능 수정 커밋 뒤 ultra job(c016/posddd-refactor.prompt): 손댄 핫패스 모듈의 불필요 코드 제거·중복/pass-through 정리·
책임 분리·명명 정합. 동작 보존, 성능 이득 없어도 구조 개선이면 채택, 커밋 분리(refactoring-request-meaning 메모리 규칙).
gate = ctest + hotpath_gate + sweep2 비회귀. 순서: ultra 2차 완료 → 감독관 gate·sweep2·커밋 → hotpath gate 도구 job →
posddd 리팩토링 job.

## D-045 (2026-09-03 10:10) 감독 실수 기록 — 대형 job 직렬 배정
ultra 2차를 D·A·B·C 4항목 단일 job으로 배정 → 실제 원인 7개, 5.5h+. worktree 분리(엔진/lifecycle vs api/reqrep)로
병렬 배정했으면 2~3h 단축 가능했음. 이후 대형 작업은 모듈 경계로 worktree를 나눠 병렬 투입(posddd 리팩토링부터 적용).

## D-046 (2026-09-03 10:30, 사용자 승인) Core 성능 회귀 재발 방지 5조치
1. hotpath_gate(callgrind, ctest) 모든 Core job 필수 green + 야간 4-size release sweep. 2. 메시지 경로 캐시화(posddd 리팩토링).
3. Core 설계 문서에 "메시지 경로 영향" 절 의무(없으면 리뷰 반려). 4. Core runtime 커밋은 서브시스템 단위로 분할·각각 gate.
5. wake/activation 불변식 결정적 테스트 suite(전이마다 poller가 기한 내 깨는지).
spec gap 처리: gap1(handover 패자 방향 in-flight request)·gap2(05-polling wake 보장 명시)는 문안 반영, gap3(completion
poller owner의 blocking request)은 ultra 구현 의미 확정 후 문안. 수정 건별 spec-gap 분류를 ultra 요약 후 수행.

## D-047 (2026-09-03 10:45, 사용자 지시) 스펙 전수 감사(spec-writing-guide) — sonnet 5분할, 리뷰는 감독관 직접
core/doc/spec/core/** 68파일을 A(root 00-04+README/glossary) B(05-08) C(socket README+pubsub) D(dealer/router/stream)
E(protocol+systems)로 나눠 sonnet 병렬. 규칙: 계약 의미 불변, 앵커 보존, ko/en parity, 문서 외 수정 금지, 이상한 계약은
CONTRACT-QUESTIONS로 보고. 리뷰·판정은 감독관 직접(계약 diff 정독 + 가이드 대조 + 링크 gate), 에이전트 자기리뷰 불인정.
ultra job과 파일 충돌 없음(doc 전용).
- (D-047 리뷰) A(root 00-04): 10파일 +54/−28 편집적. 새 주석의 계약 서술 전건 원문 대조 일치(profile 표·기본값·errno 표) → PASS.
  CONTRACT-QUESTION 03-errors REQUEST_CONFLICT "generation" → 감독관이 "transport pair generation 불일치(ESTALE)"로 명확화(ko/en).
- (D-047 리뷰) B(05~08): 6파일 +40/−6 — 하단 nav 3쌍, backpressure·Auto HWM budget 첫 사용 정의(glossary 정의와 일치 확인) → PASS.
- (D-047 리뷰) D(dealer/router/stream): 6파일 +17/−17 — 깨진 앵커 1, 명사구 풀이, STREAM 표 "언제 쓰는가" 열 → PASS.
  CONTRACT-QUESTION 06-dealer:159(count-1 Application connection 문장) → 계약 명확(ready peer마다 Application connection
  control 경로) — 감독관이 ko/en 문장 재작성. 보류 항목(§4.3 중복·선언 순서)은 계약 위험으로 이번 패스 제외 판정 유지.
- (D-047 리뷰) E(protocol+systems): 10파일 +58/−45 — exact-pipe 명사구 풀이, 첫 링크, 표 셀 동사화, 문장 분할 → PASS.
  감독관 정정 1: 01-zmp "Completion connection = reply만 나름" → "reply와 receive-flow control" (계약 정확성).
  CONTRACT-QUESTIONS: ① glossary generation을 transport pair 재생성까지 포괄하도록 명확화(ko/en) ② 10-hot-path의
  "plan Phase 5.2" 참조 제거(스펙이 판정 기준 소유, plan은 인용) — 둘 다 감독관 반영.
- (D-047 리뷰) C(socket README+pubsub): 10파일 +79/−50 — 01-pair 산문→§5 링크(규칙 §5 잔존 확인), parity 정정(마지막 이전 모든
  part MORE = 올바름), handover 문단 동치 재서술, 링크·용어 정합 → PASS. CONTRACT-QUESTIONS: ① 01-pair 소유권 "모순"은
  README 소유 규칙의 §5 검증층 재진술(가이드 §4.3 허용) → 유지. ② zlink_subscription_at 원자성: 구현(호출마다 snapshot·정렬,
  TOPICS_COUNT 별도 getter) 확인 → "호출 간 비원자, caller 직렬화" 명시(ko/en).
- (D-047 종합) 5분할 전부 리뷰 완료: 46파일 변경, 링크 0 깨짐·diff-check·ko/en heading parity OK. 감독관 정정·명확화 6건
  (REQUEST_CONFLICT generation, 06-dealer flow-state 문장, 01-zmp Completion connection 정의, glossary generation, 10-hot-path
  plan 참조 제거, 03-sub subscription_at 원자성). 이월(에이전트 보고): 디렉터리 전반 하단 nav 누락 관례, 01-pair/06/07의 §4.3
  산문-검증 중복, 선언-동작 순서 편차, PUB option enum 기본값 주석 — 계약 위험 없는 편집이라 다음 문서 패스에서 처리.

## D-048 (2026-09-03 10:55, 사용자 지시) 이 머신에서 계속
"아니다. 여기서 일단 진행해야겠다" → 중단했던 Core 2차 job을 resume 브리프(hotpath-phase2-resume.prompt)로 재기동
(c016-hotpath-phase2-r2). 트리는 중단 시점 그대로(43파일, 스냅샷과 동일). 완료 후 §0.3 절차(gate → sweep2 → 커밋·push).

## D-049 (2026-09-03 11:05, 사용자 결정) 두 머신 분담
A(이 머신): Core 2차 완료·기능 gate·커밋 → Phase 7 smoke → Phase 9 준비 → (B의 sweep2 PASS 후) 9 태그 → 10 → 11 → 12 → 13.
B(다른 머신, branch): sweep2 4-size 판정 → hotpath gate 도구 → posddd 리팩토링 + wake 테스트 → 비회귀 sweep2 → PR.
합류점 Phase 11 전. 태그는 sweep2 PASS 뒤에만. 리팩토링의 0.16.0 포함 여부는 사용자 결정 대기(권고: 태그 전 merge).

## D-050 (2026-09-03 11:10, 사용자 결정) release는 리팩토링 이후
Phase 9 태그 = posddd 리팩토링 merge + sweep2 PASS 뒤. 그 전 Phase 10~12는 로컬 빌드로 진행, 태그 후 release Core로
패키지·consumer smoke 재확인.

## D-051 (2026-09-03 12:25) Core 2차 트리 기능 gate 실패 2건 → 집중 수정 job
r2 job(resume, 1h12m 무보고 상태로 34파일 +2,468/−452 추가 편집)을 중단하고 감독관 gate 실행: ctest 133/134
(test_reconnect_options: blocking_directed_send_retries_multipart_final_frame FAIL), cpp smoke test_cpp_contract_socket 120s
Timeout. 판별: reconnect_options는 r2 델타(router_send_path 등 34파일)가 원인(스냅샷 78a718에선 PASS), cpp_contract_socket
timeout은 r1 상태(43파일)에서도 재현 → 2차 본체 결함. 트리 백업 6323fbf(→ wip 브랜치 갱신). sol ultra 집중 job
c016-phase2-fixup(perf 측정 없음, 기능 gate 전부 green 요구). B 시작 신호(커밋·push)는 이 job 뒤로 밀림.
- (Phase 9 준비, 12:35) 공개 표면 stale 심볼 0건(rg, subscription_event 오탐 제외). 0.15.1 잔존 = VERSION·core/CMakeLists.txt만
  (바인딩 매니페스트는 Phase 6에서 0.16.0). Phase 9는 `scripts/local-package/build-wsl.sh --sync-versions` → verify → 태그.

## D-052 (2026-09-03 13:20) fixup 결과 판정
회귀 1(ROUTER directed multipart FINAL 재시도): 근본 수정 1파일(+17) PASS(5회 반복). 회귀 2(cpp test_cpp_contract_socket
hang): 감독관이 테스트 원문 정독 → 8×2,000×3-part(3.4MB)를 receiver drain 전에 보내는 테스트 결함(HWM 1MiB 포화, 이전엔
EINVAL 거절 13k/16k로 우연히 통과). Core 정상. 바인딩 테스트 수정 sol job(c016-cpp-test-fix: 수동 HWM + 거절 assert 결정화).
Core 2차 커밋은 cpp 15/15 확인 후 테스트 수정과 함께 push.

## D-053 (2026-09-03 13:55) Core 2차 커밋·push — B 시작 신호
f3be895b3f core 2차(51파일, wake 3종·common send·REQREP·ws·ROUTER FINAL 재시도 fixup) + 3154ff90dc cpp 테스트 결정화.
감독관 최종 gate: ctest 134/134, cpp 15/15+7/7, mirror 12/12, diff-check. perf 4-size 판정은 B(plan-b §2). A는 Phase 7 smoke 착수.

## D-054 (2026-09-03 18:15, 이 머신 A 재개) 프로세스 정리 + Phase 7 완료
세션 3회 크래시(orca+WSL)로 detach job·/tmp 소실. 되살리던 원천=다른 Claude 세션 → --resume 세션(A)만
남기고 전부 종료(사용자 지시). dirty 26파일(중단 job WIP) 버리고 재시작(사용자 지시). main HEAD 1ac16a22b2
gate 재확인 134/134. 분담 확인(사용자): B는 별도 머신 계속 → A는 A레인만(sweep2/posddd/wake 미개입).
Phase 7 smoke 재실행 job(sol): 8언어 560 cell 실행 pass 553/unsupported 7(node inproc worker-context)/unexpected 0.
Rust hang(submit_sync 후 옛 POLLCOMPLETION 대기 잔재) 이식 수정. Rust single 독립 재검증 PASS. 커밋 cc81390c9b(perf 28파일).
다음: Phase 9 준비(D-051 절차: build-wsl.sh --sync-versions → verify → 태그는 B sweep2 PASS+리팩토링 merge 뒤).

## D-055 (2026-09-03 18:30, 사용자 지적) 버전 bump·framework 커밋 push 무방
B는 Core 성능(sweep2·posddd·wake)만 → Phase 11(framework 코드)과 파일 범위 무충돌, 동시 진행 OK.
버전 0.16.0 bump가 libzlink.so 이름을 바꿔도 perf 수치는 .so 이름과 무관(B gate=비율)하고 B는
branch 작업이라 영향 없음. 따라서 **버전 bump + framework 전환 커밋은 push 무방, 태그(core/v0.16.0)만
D-050대로 B 완료 뒤로 보류.** Phase 11 job 완료 시 검증 후 그대로 커밋·push(태그 제외).

## D-056 (2026-09-03 19:xx) Phase 11 node 결과 — BLOCKER 2건, 커밋 보류
node PARTIAL/BLOCKED. 통과: STREAM packet pull·중복 assembler(343줄) 제거·ReplyToken·stream-session 51/51·
contract 40/40·ClientServer reply. BLOCKER: (1) 공개 0.16.0 Node binding Poller가 MonitorSocket을 admit 안 함
→ D-028 monitor poller readiness 정확구현 불가, 기존 recv/pump/liveness 루프서 recv(DontWait) drain으로 우회
(추가 timer/lock 없음). (2) native mesh 통합에서 반복 Core assertion(backend-contract·RouteMesh 2/2 재현).
판정 보류: (2)는 Core assertion→A 범위 밖·언어 공통 가능성 → cpp/dotnet/java 완료 후 홀리스틱 조사.
node 커밋은 mesh 판정까지 보류. 병렬 job 교차 오염 없음(각자 자기 언어 dir만).

## D-057 (2026-09-03 19:xx) Core 버그 특정 — physical queue 회계 underflow
node mesh가 재현한 Core assertion = ctx_physical_queue_registry.cpp:64 subtract_exact `current >= amount_`.
release_committed_frame(590)·rollback_provisional(571)·completion_pending(595) 경로. RouteMesh(R/R) relocation
churn에서만, Core 134/134는 통과. 원인 후보: classify_pipepair_queues(395)가 첫 분류 시 provisional/committed=0
가정(28-33행 assert) — generation advance/reconnect 또는 lane 재분류로 잔여 committed가 남거나, 단일 lane
D/R 전환으로 REPLY byte lane 귀속이 commit/release 간 어긋남. 사용자 승인: **Core 버그면 A가 수정·push→B merge**
(core 수정 금지 해제, 이 건 한정). 계획: framework 빌드로 gdb backtrace 확보 → sol ultra Core-fix job(repro+스택+
회계 균형 분석) → 134/134 + mesh repro green → push. 그 후 framework 4언어 mesh-blocked 재실행.

## D-058 (2026-09-03 19:xx) Phase 11 4언어 결과 종합 — 공통 2이슈 판정
node·dotnet·java 완료(cpp 진행). 통과: 각 언어 framework 빌드+focused test(node 51+40+..., dotnet 57/57,
java 122/22). STREAM packet pull·중복 assembler 제거·ReplyToken·callback 제거 전 언어 완료.
공통 이슈 A(Core 버그): ctx_physical_queue_registry.cpp:64 subtract_exact exit 134, node·java mesh 재현
확정. → Core 수정(사용자 승인, A push→B merge). 최우선. cpp backtrace 후 sol ultra.
공통 이슈 B(바인딩 monitor-poller 갭): node·java Poller가 SocketMonitor 등록 미지원. 4 job 일관되게
callback 제거+recv() drain 루프 우회(no timer/lock). **판정: 우회 수용**(pull·no-callback 목표 달성),
바인딩 API 재개방 과함 → 후속 항목 기록, Phase 11 차단 안 함. spec은 poller+recv이나 recv-drain도 정신 부합.
dotnet 잔여: BLOCKER1(ConfigureCoreHwm E2E 26)=known-broken 사전존재; BLOCKER2(ContractTests 4)=사전 drift
재확인; BLOCKER3(ClientServer liveness errno91=EPROTOTYPE ROUTER→DEALER)=framework가 그 방향 typed request
쓰면 안 됨(전환 후속); BLOCKER4(handover reply-route timeout)=D-030/D-046 gap1, Core fix 후 재확인.
framework 4언어 커밋 보류 → Core A 수정·mesh 재실행 green 뒤. 병렬 job 교차오염 없음.

## D-059 (2026-09-03 20:xx) Core mesh 회계 버그 수정·push (a339149dbb)
근본원인 확정: R/R completion lane의 FLOWSTATE/WEIGHT peer-control frame이 append_pending_peer_controls_
unlocked에서 _out_pipe->write로 직접 나가 commit_message 적립을 건너뛰는데, session dequeue는 모든
non-delimiter frame을 release_committed_frame으로 차감 → committed underflow(subtract_exact:64). 단일-lane
D/R Application은 registry accounting 없어 미재현(Core 134/134·cpp mesh vertical 통과 이유).
수정: _registry_accounting일 때 control frame도 data와 동일 commit_message+publish 경로로(HWM은 completion
planned_hwm=0이라 무영향). 계약·API·제어점 불변, 새 플래그 없음. 검증: Core 134/134, node backend-contract 54/54.
**중대 함정(메모리 기록)**: 초기 "수정 안 먹음" 오판 3회 = node_modules prebuilds의 libzlink.so.0이
심링크 아닌 옛 실파일이라 .so.0.16.0만 갱신하면 옛 .so.0 로드. .so.0/.so.0.16.0 둘 다 덮거나 정본
sync 도구 사용. codex는 이 분석을 보안필터로 거부(EXIT:1) → Claude 직접 수정.
후속: java/dotnet mesh-blocked 재검증(고친 lib) → framework 4언어 커밋 → dotnet liveness EPROTOTYPE·handover 판정.

## D-060 (2026-09-03 20:5x) Phase 11 재검증 결과·판정 + version bump 커밋/push (e8045f4a02)
고친 Core(a339149dbb)로 로컬 패키지 4종(nuget·maven·npm·cpp install) 재빌드 — 모두 동일 lib
sha256 `43ddbc2f...`(6519848B), native version [0,16,0] 확인. **a=0 전 언어**: ctx_physical_queue_registry
assertion 소멸(node backend-contract 54/54, cpp mesh vertical, dotnet focused 57/57 모두 green). lib-copy
트랩 실측 해소.
언어별 잔여:
- **java**: :zlink-framework-core:test 1205/1207. 실패 2 = M6A monitor-edge gap(binding 0.16.0에 Poller
  monitor overload/공개 monitor fd 부재 → recv-drain workaround). clean HEAD에도 존재. 알려진 gap, 회귀 아님.
- **dotnet**: Phase11 focused 57/57. ClientServer 34/35 — liveness EPROTOTYPE(errno91). ContractTests 73/77
  = 기존 Actors source-owner drift(1)+ZlinkStreamDiagnosticsLevel snapshot drift(3). ConfigureCoreHwm E2E=
  known-broken(c).
- **cpp**: framework-unit 37/40(M6A known-broken, M6B old-route-admission line390, M6C production relocation),
  framework-contract 8/10(target CPP-CONTRACT-STREAM-001=D-004 stale 검사 갱신 대상, common E2E inventory 278 open).
- **node**: full 1513/1556(43 fail). backend-contract 54/54. 실패 분포: fake monitor drain(7)+socket submit(2)+
  monitor callback 기대(1)=삭제된 binding 표면 대상 test double, assertion drift(25), runtime timeout(4)+timer
  miss(1)+sample-regression(5)=거동, browser env(2). **node 전환은 test 측 미완**(Phase11이 손댄 test 4개뿐,
  ClientServer/object-routing/fanout/spot-manager 등 미전환 test가 옛 callback/monitor/submit API 기대).

**판정(D)**:
- **dotnet liveness EPROTOTYPE = framework 버그, Core 아님**. 근거 `03-errors.ko.md:552`: "ROUTER가 DEALER RID로
  typed request → ZLINK_SUBMIT_NOT_ADMITTED+EPROTOTYPE"(스펙 금지). liveness probe가 typed Request(clientRid)
  대신 plain send+correlate 써야. → framework 후속 수정.
- **dotnet handover reply-route(err101=TIMED_OUT) = spec gap**. reply token=opaque capability인데 same-RID
  handover(peer 재접속, 같은 logical RID) 후 captured token 생존 여부 미명시(physical connection 바인딩 vs
  logical RID 바인딩). Claude 전용 스펙 확정 사항, [[fix-then-spec-gap-review]]로 분류. D-046 gap1 계열.
- **cpp M6B/M6C = DIAGNOSE-ONLY**(canonical-multiattempt-trap: relocation/route-admission verify-only 실버그면
  STOP·에스컬레이션). Phase11이 relocation production(raw_route_port·raw_mesh_node_owner) + M6B test 둘 다 수정 →
  회귀 vs 기대변경 판별을 근본원인부터. blind fix 금지.

**커밋**: version bump 단독 `e8045f4a02` push(0.15.1/0.15.2→0.16.0, 검증됨·독립·머신드롭 복구비용 큼). framework
4언어 전환은 언어별 잔여 해소 후 커밋. reverify job 자체는 framework 소스 무수정(mtime 대조: node/package-lock.json
1개 npm install 부산물만).
**다음**: 병렬 codex — node test 전환+거동 root-cause, cpp M6B/M6C DIAGNOSE-ONLY. dotnet liveness fix + handover
spec gap은 Claude. java monitor-gap는 accepted followup 문서화.

## D-061 (2026-09-03 21:0x) handover reply-route 판정 정정 (D-060 item2 대체)
D-060은 handover reply-route(err101)를 "spec gap(physical vs logical binding 미명시)"로 분류했으나 **정정**.
근거 `07-router.ko.md:273-278`: reply FINAL은 "같은 logical source RID의 reply route가 local admission될
때까지" SNDTIMEO 대기하고 "현재 ready pipe"(ROUTER면 completion progress lane의 Completion pipe)를 고른다.
token 검증은 RID 일치·미소비 기준(line285: 제거·미보유·이미소비·RID불일치→NOT_FOUND). 즉 **reply token은
logical-RID 바인딩**이고, same-RID handover 후 captured (RID,token) reply route는 **스펙상 동작해야 정상**.
err101=TIMED_OUT(NOT_FOUND 아님)은 token은 생존했으나 handover 후 새 connection의 reply route가 SNDTIMEO 내
local admission 안 됨 → **defect**(pure spec gap도, invalid framework pattern도 아님).
판정: 진단 필요 — (a)Core: same-RID handover 후 pending reply FINAL이 새 connection reply route 재admission을
못 기다림/못 찾음, (b)framework: HELLO 재admission 후 reply re-drive 누락 or 테스트 타이밍. Core 결함이면
감독관 권한으로 수정·push(B merge). dotnet liveness(D-060: framework typed-request 오용)와 함께 dotnet 진단
job으로 처리 예정. [[canonical-actor-join-app-reply-contract]]·actor-authority OPEN RULING과 연관 가능.

## D-062 (2026-09-03 21:2x) cpp M6B/M6C 진단 결과 → Core CONNECT_ROUTING_ID next-connect alias 결함
cpp DIAGNOSE-ONLY job(sol ultra) 결과(c016/cpp-m6bc-diagnose.md, primary source 재검증 완료): M6B(line390 old-route
admission)·M6C(production relocation terminal blocked/restore_failed) 둘 다 **(a) production 결함**이나 원인은
Phase 11 framework reply-token diff가 **아니라 Core**. 확인: `socket_base_routing.cpp:34-51`이 alias를 socket-global
단일 `_connect_routing_id`(socket_base.hpp:1517)에 저장, `extract_connect_routing_id()`(67-72)가 move+clear로 소비,
소비 시점이 connect가 아니라 pipe-identify(`router_admission.cpp:335-336`). intent는 connect 시점 생성
(`socket_base_endpoint.cpp:~473`). back-to-back different-RID connect에서 첫 endpoint pipe가 두 번째 setter 뒤
identify되면 첫 endpoint가 둘째 RID 취득 → directed route 소실. 스펙 위반 `07-router.ko.md:118-121`(alias=다음
connect pipe 바인딩). **주의: "0.16.0 회귀" 아님** — 0.15.1 baseline 실측 없음(그 기록은 stale CMake cache 노트).
구조적 결함이 0.16.0 admission 타이밍에 노출.
판정: Core 수정(감독관 권한, B merge). 제약 5파일(socket_base_routing/socket_base.hpp/socket_base_endpoint/
router_admission/session_base)+신규 contract test 2개(우선=set-alias→connect→disconnect→reconnect→same RID route;
2번째=back-to-back). **blast radius=session_base.cpp:121 preserve_connect_routing_id(reconnect)** 반드시 커버.
리팩토링 금지(B posddd 43파일 브랜치 merge 마찰 최소화). Core fix job(sol ultra) 투입.
**후속 시퀀싱 주의(advisor)**: (1) M6B는 race 안 나면 line1343 다른 assertion → Core fix로 cpp 완료 아님, "fix→cpp
commit" 금지. (2) Core fix 후 4언어 패키지 재빌드(sha256 gate) 필요. (3) handover err101도 같은 alias 오귀속 가능성
→ Core fix 후 ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute **먼저 재실행**, dotnet job은 liveness만.

## D-063 (2026-09-03 21:5x) codex 사용한도 아웃 → Claude 서브에이전트 폴백 + node reverify 결과
**codex 사용한도 초과(Sep 7 13:45까지 불능)**: core-connect-alias-fix·dotnet-liveness-fix job이 조사/재현 단계에서
EXIT1("You've hit your usage limit"). node-phase11-finish는 한도 전 정상 완료. → [[agent-model-assignment-policy]]
폴백 발동(codex 불능 시 Claude 서브에이전트 sonnet/opus). 재투입: **Core alias fix=opus, dotnet liveness=sonnet,
node B-defect=sonnet**(Agent tool, scope 분리 core/**·dotnet/**·node/**, 커밋 금지·working tree 유지).
**node 결과(성공)**: 1513→**1552/1556 pass**. T 39건 fixed(test/contract만, src 무수정 mtime 검증), B 2건·E 2건 reported.
  - B-DETAILED: 공통 스펙(03-stream-connector:265-278) enum에 Detailed 있으나 node validator(ZlinkStreamConnectorOptions.ts:88-95)
    Off/Errors/Normal만 → node에 Detailed 추가(additive). 
  - B-SURFACE: 공통 스펙(69-101)은 setDiagnosticsLevel만인데 node public(IZlinkStreamConnector.ts:39-40)에 setDiagnosticsLevelAsync
    여분 → 조사 후 제거 or 위험시 보고.
  - E-CHROMIUM(2): chrome-headless-shell launch 직후 SIGTRAP(환경 결함, 코드 아님) — 비차단, known-env 기록.
node production(src 30파일)은 기존 Phase11 전환분(job 무수정). node 커밋은 B-defect fix 후 production+test 묶어.
**주의**: dotnet/node fix는 alias 버그와 독립이라 현 패키지로 유효. cpp M6B/M6C·handover는 Core alias fix 후 4언어
패키지 재빌드(sha256 gate)+재실행 필요.

## D-064 (2026-09-03 22:1x) Core alias fix 커밋·push (e3d5c5b79f) + dotnet liveness 실제 버그 정정
**Core alias fix(opus 서브에이전트, 감독관 전건 diff 재검증)** 커밋·push `e3d5c5b79f`(B merge). Design B: alias를
connect 시점에 transport_pair_id 키로 snapshot→locally-initiated Application pipe에 admission 전 apply→identify_peer는
pipe에서 route id 읽기, socket-global lazy 분기 제거. 2차 결함(alias≠peer-identity시 reconnect 미생존) 동시 수정.
6파일(session_base/socket_base.hpp/socket_base_endpoint/socket_base_routing/router_admission/CMakeLists)+신규 test
test_connect_rid_alias_binding(2 case, pre-fix FAIL→post-fix PASS: reconnect alias생존, back-to-back distinct route
EAGAIN 교차검증). 전체 ctest 135/135. 모든 경로 route_lifecycle_mutex 보호, scope 준수, 스펙 무수정.
**dotnet liveness 정정**: 내가 지목한 typed-request→plain-send는 **이미 기존 Phase11 전환분**(TickLivenessAsync·
ReceiveLoop 완료). dotnet 에이전트(sonnet)가 찾은 실제 버그 = Connection.Start()가 admission Hello를 두 트리거
(monitor ConnectionReady + 100ms fallback ScheduleAdmissionRetry)로 쏘는데 TryStartAdmission이 "in flight"만 가드,
"already succeeded" 미가드 → 중복 Hello가 liveness probe/ack 손상(~1/3 간헐). 수정: `|| _currentAdmission is not null`
가드(_admissionCompleted는 native-RID-mismatch 거부 경로도 set돼 부작용, _currentAdmission이 정답). 검증 liveness 20/20,
ClientServer 35/35. 1파일(ZLinkClientServerClientRuntime.cs).
**다음**: 4언어 패키지 재빌드(새 lib sha256 gate, 이전 43ddbc2f→변경) → cpp M6B/M6C(M6B는 line1343 이월 예상)·dotnet
handover(alias fix로 해소 기대)·node 재검증. node B-defect fix(sonnet): Detailed 추가+setDiagnosticsLevelAsync interface 제거.

## D-065 (2026-09-03 23:0x) 패키지 재빌드+재검증 결과 (alias fix e3d5c5b79f 효과 확인)
rebuild+reverify(sonnet). **sha256 gate PASS**: 새 lib `63336fb50769e8ad693c511413d91aba66eae7a1b8fbd5beaa6cfc0f9e2080fa`
(이전 43ddbc2f)가 9개 소비처(core/build·install prefix·nuget/maven/npm 패키지·bindings native·node prebuilds·
node_modules·NUGET 캐시) 전부 동일, lib-copy 트랩 없음. core ctest 135/135.
**alias fix 효과**:
- cpp: **M6C now PASS**(이전 FAIL). **M6B는 line390→line1343 전진**(verify_remote_bound_session_bind_classifies_
  retryable_outcomes, deadline_exceeded — 예측된 이월 signature). M6A(line176 complete_bound_session_bind) 여전히 FAIL.
  framework-contract 8/10(STREAM-001·E2E inventory 278, pre-existing).
- dotnet: **liveness now PASS**(guard fix 효과), ClientServer 35/35, focused 57/57. **handover(err101) 여전히 FAIL**
  → alias fix로 미해소, 별개 버그 확정(D-061 재판정 대상).
- java: 1207, 2 fail=known M6A monitor-gap(회귀 없음, baseline 유지).
- node: full suite background 실행 중(B-defect fix 반영, 결과 대기).
**autostash 발견(무관)**: stash@{0}=autostash **2026-09-01 15:47 생성**(세션 2일 전 pre-existing orphan, 사용자 09-01
작업). 41파일(Bingo 샘플·dist-tools·contract test), 일부 이미 커밋(stash==HEAD) 일부 2일치 old dirty. **내 세션 무관·
건드리지 않음**(drop/apply 금지). 내 세션 작업(dotnet guard·node fix·core alias·185 framework)은 전건 무결 확인.
**잔여 Phase11 blocker 3**: (1)M6A line176 — known-broken vs 회귀 판정 필요(메모리 대조), (2)M6B line1343 deadline_exceeded
진단, (3)dotnet handover err101 별개 버그 진단. codex 한도아웃 → Claude 서브에이전트/직접 판정.

## D-066 (2026-09-03 23:4x) M6B line1343·M6A 판정 + 잔여 진단 상태
**M6A(line176 complete_bound_session_bind)**: 메모리 [[zlink-env-test-quirks]] 확인 — baseline(fe77930f5d)서도 stash로
재현되는 deterministic pre-existing 실패(core 0.11.1 재설치 기인), "프레임워크 변경 탓 오판 금지" 명시. **Phase11 회귀
아님·blocker 아님**. 서브에이전트 "HEAD passed" 주장은 오판.
**M6B(line1343 verify_remote_bound_session_bind_classifies_retryable_outcomes, deadline_exceeded)**: 진단(sonnet)+감독관
primary source 확인. raw_route_port.cpp:157-174 기존 주석(46ef4b0f03)이 이 이슈 명시 — EHOSTUNREACH(미등록RID) vs
ECONNREFUSED 구분 불가로 m6a/m6b 단일 분류 공유, m6b assertion을 m6a에 맞춰 갱신. 현재 관측=not_found+errno=0(EHOSTUNREACH
아님). **alias fix diff의 xsubmit_retry_allowed는 미등록 RID를 old·new 모두 false 반환→분류 미변경**. M6B는 이전 line390서
막혀 line1343 미실행 → not_found 거동은 **pre-existing latent, alias fix가 노출만(제 커밋 e3d5c5b79f 회귀 아님)**. 판정:
**pre-existing 분류 gap, documented followup**(M6A와 동급, Phase11 blocker 아님). 잠재 fix(raw_route_port.cpp:181-190
submit_error catch에 not_found→route_unavailable 추가)는 blast radius(다른 not_found 경로)로 신중 — 별도 판단.
**잔여 진단 미완(서브에이전트 background 핸드오프로 중단)**: (2)dotnet handover err101(Core vs framework 미판별),
(3)STREAM-001(D-004 stale 여부 미확인). codex 한도아웃 지속.

## D-067 (2026-09-03 23:5x) STREAM-001 판정 + Phase11 잔여 정리
**STREAM-001**(test_cpp_framework_target_contract.cpp:2330-2353): source-scanning contract(코드 문자열 패턴 grep —
stream_send_call_t timeout·_submit(header,payload,_timeout)·pending.emplace(...timeout.async())·co_await·async_submit_runtime
부재 등). Phase11 pull 전환으로 stream submit/await 패턴이 바뀌어 grep 기대 어긋남 → **D-004 계열 in-scope Phase11 test-side
갱신**(production 버그 아님). 계약 패턴을 pull-model 코드에 맞춰 갱신 필요(node test 전환과 동류, 소량).

### Phase 11 잔여 최종 분류 (커밋 가부)
| 항목 | 판정 | blocker? |
|---|---|---|
| cpp M6A line176 | pre-existing known-broken(baseline 재현, core 0.11.1) | NO |
| cpp M6B line1343 | pre-existing latent 분류gap(alias fix가 노출), followup | NO |
| cpp M6C | **alias fix로 PASS** | 해소 |
| cpp STREAM-001 | in-scope test-side 계약 패턴 갱신(D-004) | 소량 작업 |
| cpp E2E inventory 278 | known-broken 목록 | NO |
| dotnet liveness | **guard fix로 PASS** | 해소 |
| dotnet ClientServer/focused | 35/35·57/57 | 해소 |
| dotnet handover err101 | 별개 버그, Core vs framework 미판별(canonical actor-join OPEN RULING 연관) | 진단 필요 |
| dotnet ContractTests drift(4) | pre-existing(Actors owner·StreamDiagnostics) | NO |
| dotnet ConfigureCoreHwm E2E | known-broken | NO |
| node full | 1552/1556(B-defect fix), 2 chromium 환경 | NO(환경) |
| java | 1205/1207(2 known monitor-gap) | NO |
**남은 실작업**: STREAM-001 계약 갱신(소량), handover 진단(Core/framework 판별). 나머지는 documented followup/known-broken.
이후 언어별 framework 전환 커밋(185파일)→Phase12·13. codex Sep7까지 아웃.

## D-068 (2026-09-04 00:1x) handover err101 최종 진단·스펙 판정 (canonical actor-join OPEN RULING)
diagnosis(sonnet)+감독관 스펙 분석 완결. 재현: ConnectedRuntime 테스트가 (1)원본 DEALER 소켓으로 `.Request().Timeout(2s)`,
(2)HandoverAsync가 **같은 SourceRid로 새 DEALER 소켓** 생성·연결·HELLO 재admission·`Source` 교체(원본은 PriorSource 보관),
(3)서버가 captured Core ReplyOperation으로 FINAL 제출(`nativeTerminalReplySubmitOverride`로 framework 재drive 큐 **완전 우회**,
`reply.Submit()` 단발). 관찰: 표면 예외가 ZlinkSubmitException(target측)이 아니라 **ZlinkRequestException err101(source측 2s 만료)**
— target `Submit()`은 예외 없이 성공. 즉 **Core는 reply 전달 성공**했으나 원본 DEALER가 청취 중인 물리 pipe가 아닌 다른
목적지(handover 후 "현재 ready pipe"=새 connection)로 감.
**스펙 판정**: `07-router.ko.md:273-278`은 reply FINAL이 "같은 logical source RID의 reply route admission 대기 후 **현재**
ready pipe 선택"이라 명시 → handover 후 현재 ready pipe=새 pipe. 그러나 DEALER request/reply correlation은 소켓-인스턴스
로컬이라 원본 pending request는 자기 pipe로만 수신 가능. **gap**: reply token(line92 "opaque capability")이 **물리
connection에 고정**되는지(원본 요청 pipe로 배달=test 기대) **logical RID에 고정**되는지("현재 ready pipe" 재해석=현 구현
거동)가 스펙에 미명시. token="opaque capability" 문면 vs "현재 ready pipe" 문면 **충돌**.
**결론**: **깨끗한 Core 버그 아님 = spec-design gap**(D-046/D-061 승계). alias fix와 무관(별개 메커니즘, 재현으로 재확인).
두 해소 방향 모두 비용: (A)물리-connection 결속=원본 pipe 배달 → 원본 소켓 살아있어야, 죽으면 fragile. (B)logical 유지 +
in-flight request를 handover 시 새 connection으로 마이그레이션 → **새 메커니즘·복잡도 증가**([[spec-change-policy]] 가드레일
저촉: 제어 분산·복잡화 금지). **감독관 판정: 현 스펙(logical/현재-ready-pipe)이 최단순·일관 → 유지. handover err101은 Core
결함 아니라 canonical actor-join의 request-across-handover 설계 미결(OPEN RULING)**. 이 설계 방향(captured reply route가
물리 handover를 건너 원본 요청자에게 배달돼야 하는가)은 사용자 스펙-권한 영역 → **documented followup, 사용자 설계 판정
대기**. [[canonical-actor-join-app-reply-contract]]·activationRecoveryState OPEN RULING과 동일 계열. Phase11 blocker 아님.

## D-069 (2026-09-04 00:3x) Phase 11 완료·push (framework 4언어 전환)
STREAM-001 gate 갱신(sonnet, target_contract 통과) 후 4언어 framework 전환 커밋·push:
- cpp `b32d4cae64`(42파일): backend/runtime pull 전환, e2e/CMake/STREAM-001 gate. mesh vertical·M6C·contract green.
- dotnet `7e655e3703`(30파일): runtime pull 전환 + liveness admission-Hello 중복 가드(_currentAdmission). liveness 20/20·
  ClientServer 35/35·focused 57/57.
- java `e65abaf7ac`(68파일): runtime pull 전환 + e2e/gradle. 1205/1207(2 known monitor-gap).
- node `360181172f`(46파일): runtime pull 전환 + test double 전환 + stream-connector 스펙 정렬(Detailed 추가·async 제거) +
  0.16.0 소비. 1552/1556(2 chromium 환경).
제외(Phase11 아님): bindings/ 21(세션 전 pre-existing + native sync 산출물), scripts/ 3, node_modules/.artifacts.
**Phase 11 documented followups**(baseline/known, 회귀 아님): M6A(pre-existing SIGABRT), M6B line1343(pre-existing 분류gap,
alias fix가 노출), dotnet handover err101(canonical actor-join OPEN RULING=spec-design gap, D-068), chromium SIGTRAP(환경),
E2E inventory 278(known-broken), java monitor-edge gap(binding followup). autostash(09-01)는 무관·보존.
**남은 phase(A머신)**: Phase 12(framework unit/E2E/cross-language — unit은 reverify서 검증됨), Phase 13(7 samples pull),
Phase 9 tag(B의 sweep2 PASS+posddd merge 대기), Phase 10 package. codex Sep7까지 아웃.

## D-070 (2026-09-04 00:5x) plan 정독 정정 — Phase 12/13 게이트 미완, framework 커밋은 plan 순서보다 이름
Stop hook 지적으로 plan(진실원천) 정독. 확인: plan line 1327-1331은 framework 커밋(`framework: consume zlink 0.16.0
pull APIs`)을 **Phase 12(unit+E2E+cross-language) 전부 green 후**에 하도록 규정. 나는 unit만 검증하고 언어별로 이르게
커밋(b32d4cae64 등) → 코드는 맞으나 **Phase 12.2 E2E·12.3 cross-language·Phase 13 samples 검증이 아직 안 됨**. 최종
체크리스트(line 1456-1458) 미충족: 언어별 E2E, C++·Node cross-language + Java host rebuild, 7 공통 sample.
plan line 84-85: Phase 10·11·12는 로컬 빌드 Core+로컬 패키지로 OK(태그 전), 태그(Phase 9, B 의존) 후 release Core로
package·consumer smoke. 즉 로컬 검증은 지금 진행 가능.
**남은 A 작업(정확)**: Phase 12.0 clean configure(preset linux-ninja-vcpkg-debug, tests/samples/e2e ON, ZLINK_CPP_BUILD_DIR)
→ 12.1 unit 재확인 → 12.2 언어별 E2E(run_e2e_all.sh ×4) → 12.3 cross-language(cpp/node smoke + java Host installDist) →
Phase 13 7 samples 실행. Phase 13 문서(framework/doc/framework/**)는 **사용자 영역, 건드리지 않음**(sample 실행만).
Phase 9 tag·10 release package는 B(sweep2 PASS+posddd merge) 의존. E2E/cross-language/sample에서 회귀 나오면 follow-up 커밋.

## D-071 (2026-09-04 01:1x) plan 오류 정정 — 언어별 E2E는 plan에서 제거(사용자 지시)
사용자 지시: "언어별 E2E는 별도로 진행, 진행해야 하는건 Language-Cross E2E만" + "정확히는 계획에도 있으면 안되는거야".
→ per-language E2E(구 Phase 12.2 run_e2e_all.sh ×5)는 이 캠페인 게이트가 아니라 **별도 파이프라인에서 독립 실행**되는
표준 회귀 검증. 이 pull-completion 캠페인의 E2E 게이트는 **cross-language E2E 하나**(binding/framework 계약이 언어 경계
너머 wire로 동작하는지가 캠페인 핵심). [[spec-change-policy]] 오류 정정으로 판정(구현 편의 완화 아님, 범위 오류 수정),
spec-first: 감독관(Claude) 직접 정정.
**plan 정정 4곳**: (1)line143 완료요약 항목15에서 "언어별 E2E," 삭제, (2)line1256 "12.1~12.3"→"12.1~12.2",
(3)구 12.2 "언어별 E2E" 섹션(run_e2e_all ×5) 삭제 + 구 12.3 Cross-language E2E→12.2로 renumber, (4)최종 체크리스트
line1456 "unit와 언어별 E2E"→"unit". 정정 후 Phase 12 = 12.0 configure·12.1 unit·12.2 cross-language E2E.
**감독관 착오 정정**: 앞서 Phase 12.2로 per-language E2E(cpp SpotService 등)를 서브에이전트로 돌리던 것은 범위 오류 →
중단. **남은 검증 = cross-language E2E(12.2) + Phase 13 samples**. (Phase 13 문서는 사용자 영역.)

## D-072 (2026-09-04 01:4x) handover reply-route 사용자 판정 — reply=RID 라우팅, handover 특별취급 없음 → Core 변경 불필요
사용자 판정: "reply도 다를건 없지 않어? rid handover가 다를 이유가 있어?" → **reply는 일반 send와 동일하게 logical RID로
라우팅되는 메시지이고, RID handover가 reply를 특별 취급할 이유가 없다.** handover는 RID를 새 pipe로 옮기는 것뿐 →
reply는 "그 RID의 현재 ready pipe"(=새 connection)로 가면 된다.
**Core는 이미 그렇게 동작**(D-068 진단: reply.Submit() 예외 없이 성공, 07-router:273 "현재 ready pipe"대로 새 connection
전달) → **Core 변경 불필요, handover err101은 Core 버그 아님, spec 변경(물리 binding)도 아님**. D-061/D-068의 "spec gap"
판정은 **logical-RID binding으로 확정**(reply=send와 동일 규칙)하여 종결 — 스펙 문면(현재 "현재 ready pipe")이 곧 정답.
**err101 원인 = test/framework 측**: 테스트가 (1)handover 전 원본 소켓에 `.Request()`를 매어두고 거기서 대기(reply는
올바르게 새 소켓으로 감), (2)`nativeTerminalReplySubmitOverride`로 framework의 reply 재drive 큐(SubmitOrQueueNativeReply/
PendingNativeTerminalReply)를 우회. 실제 framework엔 재drive 메커니즘 존재. → **framework/test followup**(Core 아님).
**귀결**: **이번 캠페인 Core 버그 2건(mesh a339149dbb·alias e3d5c5b79f) 모두 수정·push 완료로 확정**. handover는 Core 무관.
[[canonical-actor-join-app-reply-contract]] OPEN RULING(reply-token binding)= **logical RID로 종결**.

## D-073 (2026-09-04 02:4x) cross-language E2E가 dotnet PUB/SUB subscribe 수신 회귀 검출 (R)
Phase 12.2 cross-language E2E 실행(-v 없이, driver 직접) 결과: **.NET subscriber가 PUB/SUB 이벤트 미수신(deterministic R)**.
- cpp smoke: C++ publisher→.NET channel-subscriber, .NET READY하나 미수신(runDir dotnet-subscriber.events 빈파일, cpp-publisher는 발행). "timed out waiting for 'profile.changed:cpp-publish'".
- node smoke: Node publisher→.NET fanout subscriber, publishUntilFileText 반복발행에도 미수신 "expected event text did not appear".
공통=**.NET SUBSCRIBER(pub/sub·fanout) 수신만 실패**. dotnet channel(req/reply)·CoreCLR 정상, cross-manifest dotnet-framework=source-tree(현재 전환 framework). → **dotnet 전환(7e655e3703)의 SUB pull-receive drain 회귀**. unit(57/57)·ClientServer(35/35)은 cross-language SUB 미커버 → cross-language 게이트가 검출(게이트 가치 실증).
**주의(감독관 착오 정정)**: 1차 실행 실패는 내 driver의 ulimit -v(dotnet CoreCLR OOM 0x8007000E/137) 아티팩트였음 → -v 제거 후 재실행해서 진짜 회귀 분리. cross-language는 dotnet CoreCLR 띄우므로 **-v 절대 금지**([[zlink-env-test-quirks]] 재확인).
**대응**: dotnet SUB-receive 진단+수정 subagent(sonnet, framework/languages/dotnet만, Core면 보고). 수정 후 cross-language 재실행 확인→커밋(7e655e3703 후속). Phase 11 dotnet 커밋은 이 회귀 포함 상태였으므로 follow-up 수정 필요.

## D-074 (2026-09-04 03:5x) dotnet PUB/SUB 수신 회귀 수정(codex sol ultra) + binding 근본원인 follow-up
codex sol ultra(재로그인 후 codex 복구, hep7@naver.com). 근본원인 확정: **dotnet binding 버그** — `bindings/dotnet/src/
Zlink/Runtime/Eventing/Poller.cs:39-42`가 PollCompletion 요청 여부와 무관하게 `SocketKernel.Completion` 조회, `SocketKernel.cs:
28-38`은 SUB에 NotSupportedException(PAIR/DEALER/ROUTER/STREAM만 completion 지원) → `poller.Add(SUB,PollIn)`가 등록 전 예외 →
subscriber 루프가 Wait/Subscribe/dispatch 미도달(READY이나 silent). Phase11 `7e655e3703`은 SUB코드 무변경, binding 0.15.2→
0.16.0 bump만 했고 새 binding의 이 거동이 회귀 유발. category (a) 확정, (b)(c)(d) 배제.
**수정(framework-scoped, 커밋 후속)**: `ZLinkBackendSocketPoller.Create`가 ISubSocket을 분기해 completion 미조회 `ZlinkPoll.Poll`
read-only adapter 사용, Router/Dealer/Stream IPoller 유지. 1파일, 임시로그 제거(rg 0). 검증: cpp publisher→.NET·node publisher→
.NET 수신, **C++ 전체 32-stage cross-language smoke 통과**, fanout+ClientServer 37/37. 감독관 diff 재검증 완료.
**FOLLOW-UP(정식 근본 수정)**: `bindings/dotnet Poller.Add`가 PollCompletion 요청 시에만 Kernel.Completion 조회하도록 binding
수정(그러면 framework 워크어라운드 불필요, 타 언어 binding도 동일 잠재버그 점검). binding 레이어(Phase 6)라 별도 pass/B 조율.
**주의**: cpp/java 샘플 authenticate 회귀는 DEALER/ROUTER(completion 지원)라 이 SUB-poller 이슈와 **별개** — 다음 진단 대상.

## D-075 (2026-09-04 04:1x) cpp 샘플 red 근본원인 = Phase 11 아님, 다른 커밋들(사용자 영역) — 조율 필요
codex sol ultra 진단(cpp-sample-auth-fix-summary.md). **cpp 샘플 authenticate red는 pull-completion 전환(b32d4cae64)과
무관** — packet pull decode 성공 확인, Core/binding 결함 없음. 3개 레이어드 근본원인:
1. **nested auth service 등록 누락** ← `e6ae5d8fd6`(feat: constructor-based DI auto-deduction, drop dependency_types;
   ulalax 09-02, 이미 main). outer session은 자동등록되나 authenticate_(play_)session_handler_t는 미등록 →
   get_or_create_core_session "service not registered". **수정(검증됨, 미커밋)**: TicTacToe/Bingo host factory에
   `add_scoped<authenticate_*_handler_t, channel_client_t>()` 2줄. authenticate boundary PASS 재현. 다른 샘플
   (DeliveryDispatch/SupportChat/GameQuest)은 정적 분석상 동일 누락 없음(명시 등록돼 있음) → 복사 금지.
2. **protobuf payload에 JSON wire metadata 불일치**(미수정) ← `b1053aceda`(strict typed validation)+`1cf31e1a79`
   (marker 기반 generated-protobuf). typed serializer는 protobuf(application/x-protobuf) 생성하나 wire content_type을
   type_index로 별도 조회 시 serializer.cpp erased lookup이 application/json fallback → 수신측 "inbound content type
   does not match the typed handler codec" 거부(deterministic). **framework/languages/cpp/framework/include 공개 헤더 +
   test 변경 필요**(typed serializer/payload-encoder가 content_type을 함께 반환하도록). codex는 unsound framework/src
   워크어라운드 거부(1024 캐시 한계·reentrant·marker 불일치로 오작동), 작업 범위 밖으로 STOP. **사용자 영역·조율 필요**.
3. **TicTacToe bound-Session delivery stall**(별개, 비결정적) ← actor_gateway_runtime FIFO, b32 무변경, 재실행마다 다른
   post-auth 단계서 정지. 별도 lifecycle/delivery 조사.
**java 샘플**은 cpp-specific 커밋들과 무관 = 별개 root(readiness/route/handler-dispatch 독립 조사 필요).
**조율 사항**: (a)DI 수정 2줄 커밋 여부, (b)codec 이슈(framework 공개헤더=사용자 DI/framework 영역)를 이 캠페인서 고칠지
사용자가 직접 할지, (c)나머지(TTT stall·java·node stream·ZoneWorld) 처리 방침. **cpp 샘플 red는 pull-completion 캠페인
책임이 아닌 기존 framework 부채**임을 확인.

# 머신 B 판정 (D-B54~D-B66; 머신 A의 D-054~D-075와 번호 충돌을 피해 B 접두)
## D-B54 (2026-09-03 14:10, 머신 B, 사용자 지시) 성능 판정은 cell 단위로 끊어서
사용자: "패턴별 + transport별로 끊어서 비교, 전체를 한 번에 돌리지 말 것". 전체 sweep2(single 33/42 진행)를 중단하고
percell.sh(cell 하나 → FAIL이면 그 자리에서 1회 재측정 → 최종 verdict 기록)로 전환. 환경: valgrind 3.23.0 소스 빌드
(~/.local/bin, sudo 불가), baseline worktree core/v0.15.1(ba78905c3d) 자체 빌드, 벤치 3파일 동일 확인.
1차 관찰: (1) 대부분 cell의 FAIL은 latency_p95/p99 단일 size(1µs 해상도) — 재측정 대상. (2) REQREP 6 transport 전부
latency 10~70× — 벤치가 포화 구간 queue 깊이를 latency로 보고(candidate가 Byte HWM까지 ≈9,500 in-flight vs baseline ≈160).
스펙 §5.2 "벤치를 고친다" 적용: briefs/reqrep-latency-bench.b.prompt(one-way와 같은 in-flight 1 latency 구간). (3) REQREP
inproc·wss는 throughput도 0.81~0.88 → 실제 회귀 후보, 벤치 정정 후 cell 재측정으로 확정.

## D-B55 (2026-09-03 15:05, 머신 B) 벤치 정정 채택 + cell 측정을 runs=3(median)으로
c016-reqrep-bench(sol high) 산출 채택: single/multi REQREP two-phase(포화 throughput + in-flight 1 latency 1초), RESULT latency
3종 소수 6자리, baseline worktree 동일 파일 복사(callback API 경로는 compile-check로 선택), gate·policy unit 59/59. 검증 단발:
DEALER_ROUTER_REQREP tcp 64B latency main 0.083ms / baseline 0.098ms. 정정 벤치로 첫 cell(PAIR/tcp) 2회 측정에서 1024B
throughput이 0.98→0.89로 흔들림 → 판정 기준(D-040)은 그대로, 측정만 runner 자체 `--runs 3`(size별 median)으로 전환
(sweep2.sh에 SWEEP2_RUNS 환경변수 추가). 전 cell 재판정(percell.sh: cell → FAIL 시 즉시 1회 재측정).

## D-B56 (2026-09-03 15:10, 머신 B, 사용자 지시) 결과를 다 기다리지 않고 개선 포인트가 나오면 즉시 수정
사용자: "결과 다 보고 하면 늦다. 개선 포인트 나오면 바로 개선하고, 개선되면 다음 측정". 판정 기준 재확인: size cell 5%는 허용
오차, (pattern,transport) size 합계(기하평균)가 baseline보다 낮으면 개선 대상. 개선은 posddd 리팩토링과 같이(성능 이득 없어도
구조 개선이면 채택, D-044), codex sol ultra. 벤치 정정 커밋 1e91505a14(perf/phase2-judge). 정정 벤치·runs=3 판정 7 cell 후 배치
중단, 개선 대상: PAIR/tcp(1024B thr 0.95, p95/p99 집계 1.01~1.05), PAIR/ws·wss(thr 집계 0.989~0.995), PAIR/inproc(64K latency
p95 3.3~3.8×). 경계: PAIR/tls·ipc, PUBSUB/tcp p99 단일 size. job c016-perf-improve-r1(briefs/perf-improve-posddd-r1.b.prompt).

## D-B57 (2026-09-03 18:25, 머신 B, 사용자 승인) 남은 cell은 runs=1 스크리닝 → FAIL만 runs=3 확인
측정 시간이 병목(runs=3 + 재측정 = cell당 3.5~7분, 70 cell 4~5h). 판정 기준은 그대로 두고 측정 절차만: runs=1로 스크리닝해 PASS면
확정, FAIL cell만 runs=3으로 1회 확인 측정. 확인된 FAIL을 묶어 개선 job 투입.

## D-B58 (2026-09-03 21:05, 머신 B, 사용자 지시) 순서 변경 — 리팩토링 먼저, 성능은 1024B 경량 비교로 동행
사용자: "리팩토링 먼저. 성능 회귀는 ROUTER_ROUTER·SENDSEND·REQREP 1024B만 비교하면서, 그 다음 성능 갭 채우기". 브랜치·worktree 추가
금지("여기서 계속") → perf/phase2-judge 트리에서 모듈 범위별 job을 순서대로(rf1 api/socket → rf2 sockets common/dealer/router/
internal → rf3 pipe/ypipe/mailbox → rf4 wake 불변식 테스트), 각 job 뒤 tools의 light_perf.sh(1024B, 8 cell)로 비교 후 커밋.
개선 job c016-perf-improve-r1(15:08~21:00, sol ultra) 감독관 중단: 원인 5개 실증·수정(POLLIN probe helper mutex → atomic cache;
첫 HWM 대기 후 async owner 잔존 → 직접 owner 선출/retire; prefetched batch tail을 drain으로 오인한 sub-LWM 조기 wake; count-1 D/R
재분류 검사가 모든 PAIR flush에 실행 → peer type gate·FQ publication opt-in; PAIR 2-frame whole-record 경로 + WS 출력 batch 16KiB
한정) + 벤치 결함 1개 추가 정정(one-way in-flight 1 ack 경계) + posddd 정리 일부(−792/+476 시점). 최종 sweep: PAIR/tcp·inproc 전
cell·집계 PASS(tcp thr 집계 1.20, lat 0.83), ws/ipc/PUBSUB tcp는 tail 단일 cell이 run마다 다른 size로 이동(WSL2 drift) — 이 시점에서
중단하고 감독관 gate 후 커밋. D-045 재발(단일 5h job) 기록: 이후 job은 원인 하나·1.5h 상한.

## D-B59 (2026-09-03 21:10, 머신 B) 리팩토링 전 1024B 경량 비교(commit 10cc586a83 vs core/v0.15.1, runs=1)
| cell | thr | lat | p95 | p99 |
|---|---|---|---|---|
| single ROUTER_ROUTER/tcp | 0.937 | 0.963 | 0.868 | 0.913 |
| single ROUTER_ROUTER/inproc | 1.292 | 0.522 | 0.308 | 0.812 |
| single DEALER_ROUTER_REQREP/tcp | 1.035 | 0.838 | 0.712 | 0.816 |
| single ROUTER_ROUTER_REQREP/tcp | 1.157 | 0.808 | 0.761 | 0.787 |
| multi DEALER_ROUTER_SENDSEND/tcp | 0.952 | 2.149 | 2.422 | 2.570 |
| multi ROUTER_ROUTER_SENDSEND/tcp | 0.998 | 1.137 | 1.117 | 1.238 |
| multi DEALER_ROUTER_REQREP/tcp | (candidate 1회 FAIL, 재실행 1.553) | 0.90 | 0.93 | 0.95 |
| multi ROUTER_ROUTER_REQREP/tcp | 0.857 | 1.102 | 1.118 | 1.112 |
관찰: single REQREP·inproc은 크게 개선. 개선 후보(리팩토링 뒤 성능 갭 단계에서 처리): single ROUTER_ROUTER/tcp 1024B thr 0.94,
multi SENDSEND latency 1.1~2.1×(포화 latency — multi SENDSEND 벤치는 REQREP 정정 대상이 아니었음; 벤치 측정 방식 재확인 필요),
multi ROUTER_ROUTER_REQREP thr 0.86. 주의: candidate multi DEALER_ROUTER_REQREP 1024B가 1회 FAIL(report에 원인 없음, 재실행 PASS)
→ 간헐 실패 여부를 조용한 구간에서 5회 반복해 확인해야 함.

## D-B60 (2026-09-04 00:35, 머신 B) rf1(api/socket) 채택·커밋
c016-posddd-rf1(sol ultra, 21:03~) +607/−1478. 감독관 gate 전부 green. 1024B 경량 비교(vs 0.15.1): single ROUTER_ROUTER/tcp thr
0.94→1.01, REQREP 1.05/1.09, multi REQREP 1.22/0.96 — 리팩토링 전 대비 나빠진 cell 없음. multi SENDSEND latency(2.1→3.2×)는 포화
latency 지표로 run 간 편차가 커서 성능 갭 단계에서 벤치 측정 방식과 함께 다룬다. 커밋은 항목별 분리 대신 1건(hunk 겹침, 시간
우선; 요약에 항목별 파일 묶음 기록). BLOCKERS 16건(범위 밖 test/runtime 이동 필요)은 rf2·rf3에서 해당 범위 것을 처리.

## D-B61 (2026-09-04, 머신 B) rf2(runtime/sockets) 채택·커밋
c016-posddd-rf2(sol ultra) +1304/−1689. 감독관 gate 전부 green. 1024B 비교(vs 0.15.1): single thr 1.03~1.22, multi 0.95~1.29 —
rf1 대비 나빠진 cell 없음(tail 단일 run 편차만). BLOCKERS 3건 중 pipe 2건은 rf3 브리프에 이관, socket_send_pending_submit.cpp
물리 분리(CMake 등록)는 후속.

## D-B62 (2026-09-04 02:30, 머신 B, 사용자 지시) PR은 하나로, 시간 단축
사용자: "PR 하나로. 너무 오래 걸린다." → 플랜 §7의 3-PR 대신 perf/phase2-judge 단일 PR. 단축: hotpath gate 도구 job을 rf3와 병렬
(core/build-hp), rf4는 rf3 직후 병렬(core/build-wk). 70 cell 4-size 전체 판정은 PR 뒤 별도(태그 전 필수, D-050)로 이동 — PR 본문에는
1024B 경량 비교 표(리팩토링 전/후, 회귀 없음)를 넣는다.

## D-B63 (2026-09-04, 머신 B) rf3·hotpath gate 도구 커밋
rf3(pipe/ypipe/mailbox) 341974c4d6 +206/−357: gate green(test_backpressure_oneway_matrix_single_socket 1회 load timeout → 단독 2회
4.6s PASS; 최종 gate에서 10회 반복 재확인 예정). hotpath gate 도구 커밋: 결정성 3회 ±0.06%, 인위 회귀 3.8× FAIL 확인, 기준값은
감독관이 커밋 트리에서 --update-reference로 재생성(job 값과 0.02% 이내 일치). ctest 135. rf4(wake 테스트) 진행 중.

## D-B64 (2026-09-04, 머신 B) candidate multi DEALER_ROUTER_REQREP 간헐 실패 = Core completion 정지 회귀
runner 반복 9회 중 5회 client exit 1(baseline 9/9 성공). 벤치에 진단 출력 추가 후: 포화 구간 뒤 drain에서 client socket 1~3개가
3,094~11,703건 outstanding을 들고 정지(reply·200ms timeout completion 모두 1초 내 미도착). 다른 97~99 socket 정상. 이전 runner는
drain을 요구하지 않아 검출 못 했고(정정 벤치의 two-phase가 드러냄), 1024B 경량 비교의 '-'가 이것. PR 전 수정 필수 →
sol ultra job c016-reqrep-stall(briefs/reqrep-completion-stall.b.prompt, 1.5h 상한). 최종 gate(138/138·backpressure ×10)는 green.

## D-B65 (2026-09-04 05:05, 머신 B) 정지 회귀 상류 경계 확정 + origin/main 무결 확인
1차 job(c016-reqrep-stall, 1.5h 상한): 정지 경계 = server session→ROUTER application pipe가 ROUTER에서 한 frame도 소비되지 않은 채
(peer_read=0) 4MiB HWM에서 영구 정지; client 정지는 하류 backpressure. completion cache·timeout task·sub-LWM wake·client reader
wake·수동 HWM 가설 실증 배제. Core diff 0으로 종료. 감독관: origin/main(1ac16a22b2)을 zlink-main-check에 빌드해 같은 벤치로
6/6 성공 → 원인은 이 브랜치의 Core 커밋(8b6c2aa906 최우선: activate_read armed-flag gate / FQ publication opt-in / reclassify
wake 소비 조건 / memory-order 분기). 2차 job c016-reqrep-stall-r2(hunk 단위 A/B 10회씩, 근본 수정 + 결정적 회귀 테스트).

## D-B66 (2026-09-04 05:10, 머신 B) 정지 회귀 근본 원인·수정
1차 job이 상한 직전 확정: f3be895b3f의 ROUTER count-1 `xread_activated`/`xread_deactivated` fast path가 route-binding token을 확인하지
않아, pair admission이 ready cache를 먼저 세운 anonymous pipe(identity 프레임이 나중에 오는 경우)를 adopted/FQ 등록된 것으로 오판 →
첫 activation이 미등록 `_fq.activated()` no-op으로 소비되고 slow identity adoption을 영구히 잃음(peer_read=0). 수정: 두 fast path에
`router_route_binding_token() != 0` fence(2줄). 결정적 회귀 테스트 test_count1_router_adopts_anonymous_pipe_on_first_activation
(synthetic pipe/mailbox harness, sleep 없음). job 자체 직접 비교 10/10 성공. origin/main이 6/6 통과한 것은 8b6c2aa906의 타이밍
변화가 발현 확률을 높였기 때문으로 보며, 결함 자체는 main에도 있음(A에 통보 필요). 2차 job(r2) 취소.

## D-B67 (2026-09-04 07:40, 머신 B, 사용자 지시) origin/main 머지 → 검증 → main 머지
사용자: "main 최신화 후 머지, 이상 없으면 다시 main에 머지". 전체 70 cell 판정은 19 cell 진행 시점(PAIR 6·PUBSUB 6·DEALER_DEALER
6·DEALER_ROUTER 1; throughput 집계 전부 PASS, tail 집계 미달 PAIR/ws·PUBSUB/tcp·DEALER_DEALER tcp/tls/wss/ipc, DEALER_DEALER/tcp는
mean latency 집계 1.061로 실제 후보)에서 중단(sweep2-results.run3-partial-19cell.md 보관). origin/main(504d39fc6e, +26) 머지:
충돌은 decisions.ko.md 번호(A의 D-060~075 vs B의 D-054~066)뿐 → B 항목을 D-B54~B66으로 개명. 주의: A가 perf runner를 0.16.0
pull API로 포팅(cc81390c9b)해 baseline(0.15.1)에는 그 소스를 복사할 수 없음(zlink_send_part 시그니처 상이) → baseline worktree는
B의 이전 벤치(two-phase·6자리)를 유지, python(gate·run_comparison)만 동기화. 머지 트리 gate → 10× REQREP 재현 → 1024B 비교 →
main 머지. 전체 cell 판정은 머지된 main 트리에서 이어서 수행.

## D-076 (2026-09-04 05:xx) cpp codec fix 커밋 + B core merge 통합 + B-core 재검증 착수
**cpp codec content_type fix 커밋(180323e6fa, 로컬)**: 공개 계약 `serialized_payload_t`+`serialize_with_content_type()`
(bytes와 그 serializer의 content_type 함께 반환)로 generated protobuf가 wire에 x-protobuf를 싣도록. erased type_index 조회
제거. 16파일(공개헤더 serializer/channel/actor 3 + runtime 5 + test 5 + CMake + 새 private test-access header). focused 6/6·
sample_parity 통과·Bingo authenticate codec mismatch 0·cross-language C++↔.NET/Node codec stage 통과. 감독관 공개헤더 재검증.
잔여: Bingo `submit card` STREAM timeout(별개), cross-language browser=chromium(E).
**B core merge 통합**: 사용자 통지 — B의 core posddd/perf가 origin/main(8d58b7f891)에 머지됨(진행 중). 내 Core 수정 3건
(mesh·alias·version) origin/main에 생존 확인. working tree pre-existing dirty(bindings 버전헤더[B가 동일 커밋]·node dist-tools·
scripts·native sync·09-01 autostash)는 **사용자 작업 아님(사용자 확인 "내가 한게 아니라 정리해도 돼")** → git restore로 정리
(내 커밋 무손실). `git pull --no-rebase`로 merge commit `bebbc7b925` 생성(충돌 0, codec=framework/cpp vs B=core/bindings/docs).
**B core 재검증 착수(codex sol)**: 새 core 재빌드+sha256 gate+framework 재검증(codec Bingo·dotnet SUB cross-language·4언어
focused). B posddd는 동작보존 의도라 회귀 없어야 하나 재빌드 필수. 재검증 green 확인 후 merge push(broken main 방지).

## D-077 (2026-09-04 06:xx) B core 재검증 R-Bcore 없음 → merge push (9a2fc244ed)
codex sol reverify-bcore. **결론: R-Bcore 없음**(B의 core posddd/perf가 framework 회귀 유발 안 함, 동작보존 확인).
- Core ctest 139/139(B 신규 hotpath/wake test + 내 alias/mesh 공존), **sha256 gate 58/58**(새 lib 06f5e02455...).
- cpp framework-unit 38/40·contract 9/10(전부 (P): M6A·M6B1343·E2E inventory), codec/STREAM-001/sample_parity 통과.
- dotnet focused 재실행 57/57(56/57은 flake), SUB/fanout 37/37+8/8(SUB fix 유지). java 1205/1207((P)M6A). node backend-contract
  54/54, full runtime 1551/1553(2=(E)chromium). cross-language C++/.NET/Node channel·fanout·raw STREAM codec mismatch 0.
- Bingo/TTT stream-connector timeout=D-075 계열 기존 delivery debt(P), B pipe/mailbox(341974c4d6)와 인과 없음(A/B 배제).
**merge push `9a2fc244ed`**: codec fix(180323e6fa)+B merge(bebbc7b925)+D-076 통합. origin/main = B core + 내 캠페인 전부.
**node lint(내 fallout)**: ZlinkStreamConnectorOptions.ts:94 `!== Detailed` 추가로 enum 소진 → no-unnecessary-condition
에러(npm test lint 게이트 차단). validator가 런타임 invalid 값 거부하도록 수정 필요.
**잔여 delivery debt(캠페인서 계속 수정, 사용자 승인)**: cpp stream-connector delivery stall(Bingo/TTT 공통), java 샘플
authenticate(별개 root), node cross-language .NET→Node stream stage, ZoneWorld(4언어). 전부 pull-completion 무관 기존 부채.

## D-078 (2026-09-04 09:xx) 설계 원칙 확정(사용자) — API는 직관적 동작, 별도 해석 불필요; readiness는 진짜 level-trigger
사용자 원칙: "API는 직관적인 방법으로 동작해야 하고 그 외 다른 해석이 필요 없어야 한다." → poll/recv 같은 API는 자연스러운
"poll→recv 하나→poll" 사용으로 동작해야 하며, drain-until-EAGAIN 같은 **특별한 사용을 강요하면 API/readiness 설계·구현 결함**.
이는 기존 스펙과 **이미 일치**: `05-polling.ko.md:65-71` readiness=level-trigger + "readiness=true인데 timeout까지 잠듦
(lost wake)은 계약 위반, 구현은 command 소비 후 notification descriptor 재무장으로 이를 지킨다"; line 49 raw socket POLLIN=
"complete record 수신 가능"; line 78 "queue에 record 남아있는 동안 readiness 유지". line 83/88의 drain-until-NO_DATA는
completion polling(POLLCOMPLETION) 전용 규칙이지 일반 POLLIN 계약 아님.
**적용**: (1) STREAM stall = Core stream.cpp packet-mode의 lost-wake(buffered packet에 POLLIN 재무장 안 함) = **Core 계약
위반 버그**로 확정. framework drain 루프는 workaround라 기각(codex도 동일 결론, 자기 framework 변경 되돌림). Core에서 수정
(스펙 변경 아님, 구현을 스펙에 맞춤). (2) **소켓 사용 audit 기준**: perf(소켓별 최적 사용)와 대조하되, framework가 "특별 사용"을
해야만 동작하는 지점은 그 소켓의 readiness/API 구현 결함으로 판정해 API 레이어(Core/binding)에서 고친다. perf의 drain 루프는
API가 level-trigger면 최적화(선택)이지 필수 아님.
