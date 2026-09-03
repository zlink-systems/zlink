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
