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

## D-054 (2026-09-03 14:10, 머신 B, 사용자 지시) 성능 판정은 cell 단위로 끊어서
사용자: "패턴별 + transport별로 끊어서 비교, 전체를 한 번에 돌리지 말 것". 전체 sweep2(single 33/42 진행)를 중단하고
percell.sh(cell 하나 → FAIL이면 그 자리에서 1회 재측정 → 최종 verdict 기록)로 전환. 환경: valgrind 3.23.0 소스 빌드
(~/.local/bin, sudo 불가), baseline worktree core/v0.15.1(ba78905c3d) 자체 빌드, 벤치 3파일 동일 확인.
1차 관찰: (1) 대부분 cell의 FAIL은 latency_p95/p99 단일 size(1µs 해상도) — 재측정 대상. (2) REQREP 6 transport 전부
latency 10~70× — 벤치가 포화 구간 queue 깊이를 latency로 보고(candidate가 Byte HWM까지 ≈9,500 in-flight vs baseline ≈160).
스펙 §5.2 "벤치를 고친다" 적용: briefs/reqrep-latency-bench.b.prompt(one-way와 같은 in-flight 1 latency 구간). (3) REQREP
inproc·wss는 throughput도 0.81~0.88 → 실제 회귀 후보, 벤치 정정 후 cell 재측정으로 확정.

## D-055 (2026-09-03 15:05, 머신 B) 벤치 정정 채택 + cell 측정을 runs=3(median)으로
c016-reqrep-bench(sol high) 산출 채택: single/multi REQREP two-phase(포화 throughput + in-flight 1 latency 1초), RESULT latency
3종 소수 6자리, baseline worktree 동일 파일 복사(callback API 경로는 compile-check로 선택), gate·policy unit 59/59. 검증 단발:
DEALER_ROUTER_REQREP tcp 64B latency main 0.083ms / baseline 0.098ms. 정정 벤치로 첫 cell(PAIR/tcp) 2회 측정에서 1024B
throughput이 0.98→0.89로 흔들림 → 판정 기준(D-040)은 그대로, 측정만 runner 자체 `--runs 3`(size별 median)으로 전환
(sweep2.sh에 SWEEP2_RUNS 환경변수 추가). 전 cell 재판정(percell.sh: cell → FAIL 시 즉시 1회 재측정).

## D-056 (2026-09-03 15:10, 머신 B, 사용자 지시) 결과를 다 기다리지 않고 개선 포인트가 나오면 즉시 수정
사용자: "결과 다 보고 하면 늦다. 개선 포인트 나오면 바로 개선하고, 개선되면 다음 측정". 판정 기준 재확인: size cell 5%는 허용
오차, (pattern,transport) size 합계(기하평균)가 baseline보다 낮으면 개선 대상. 개선은 posddd 리팩토링과 같이(성능 이득 없어도
구조 개선이면 채택, D-044), codex sol ultra. 벤치 정정 커밋 1e91505a14(perf/phase2-judge). 정정 벤치·runs=3 판정 7 cell 후 배치
중단, 개선 대상: PAIR/tcp(1024B thr 0.95, p95/p99 집계 1.01~1.05), PAIR/ws·wss(thr 집계 0.989~0.995), PAIR/inproc(64K latency
p95 3.3~3.8×). 경계: PAIR/tls·ipc, PUBSUB/tcp p99 단일 size. job c016-perf-improve-r1(briefs/perf-improve-posddd-r1.b.prompt).

## D-057 (2026-09-03 18:25, 머신 B, 사용자 승인) 남은 cell은 runs=1 스크리닝 → FAIL만 runs=3 확인
측정 시간이 병목(runs=3 + 재측정 = cell당 3.5~7분, 70 cell 4~5h). 판정 기준은 그대로 두고 측정 절차만: runs=1로 스크리닝해 PASS면
확정, FAIL cell만 runs=3으로 1회 확인 측정. 확인된 FAIL을 묶어 개선 job 투입.

## D-058 (2026-09-03 21:05, 머신 B, 사용자 지시) 순서 변경 — 리팩토링 먼저, 성능은 1024B 경량 비교로 동행
사용자: "리팩토링 먼저. 성능 회귀는 ROUTER_ROUTER·SENDSEND·REQREP 1024B만 비교하면서, 그 다음 성능 갭 채우기". 브랜치·worktree 추가
금지("여기서 계속") → perf/phase2-judge 트리에서 모듈 범위별 job을 순서대로(rf1 api/socket → rf2 sockets common/dealer/router/
internal → rf3 pipe/ypipe/mailbox → rf4 wake 불변식 테스트), 각 job 뒤 tools의 light_perf.sh(1024B, 8 cell)로 비교 후 커밋.
개선 job c016-perf-improve-r1(15:08~21:00, sol ultra) 감독관 중단: 원인 5개 실증·수정(POLLIN probe helper mutex → atomic cache;
첫 HWM 대기 후 async owner 잔존 → 직접 owner 선출/retire; prefetched batch tail을 drain으로 오인한 sub-LWM 조기 wake; count-1 D/R
재분류 검사가 모든 PAIR flush에 실행 → peer type gate·FQ publication opt-in; PAIR 2-frame whole-record 경로 + WS 출력 batch 16KiB
한정) + 벤치 결함 1개 추가 정정(one-way in-flight 1 ack 경계) + posddd 정리 일부(−792/+476 시점). 최종 sweep: PAIR/tcp·inproc 전
cell·집계 PASS(tcp thr 집계 1.20, lat 0.83), ws/ipc/PUBSUB tcp는 tail 단일 cell이 run마다 다른 size로 이동(WSL2 drift) — 이 시점에서
중단하고 감독관 gate 후 커밋. D-045 재발(단일 5h job) 기록: 이후 job은 원인 하나·1.5h 상한.

## D-059 (2026-09-03 21:10, 머신 B) 리팩토링 전 1024B 경량 비교(commit 10cc586a83 vs core/v0.15.1, runs=1)
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

## D-060 (2026-09-04 00:35, 머신 B) rf1(api/socket) 채택·커밋
c016-posddd-rf1(sol ultra, 21:03~) +607/−1478. 감독관 gate 전부 green. 1024B 경량 비교(vs 0.15.1): single ROUTER_ROUTER/tcp thr
0.94→1.01, REQREP 1.05/1.09, multi REQREP 1.22/0.96 — 리팩토링 전 대비 나빠진 cell 없음. multi SENDSEND latency(2.1→3.2×)는 포화
latency 지표로 run 간 편차가 커서 성능 갭 단계에서 벤치 측정 방식과 함께 다룬다. 커밋은 항목별 분리 대신 1건(hunk 겹침, 시간
우선; 요약에 항목별 파일 묶음 기록). BLOCKERS 16건(범위 밖 test/runtime 이동 필요)은 rf2·rf3에서 해당 범위 것을 처리.

## D-061 (2026-09-04, 머신 B) rf2(runtime/sockets) 채택·커밋
c016-posddd-rf2(sol ultra) +1304/−1689. 감독관 gate 전부 green. 1024B 비교(vs 0.15.1): single thr 1.03~1.22, multi 0.95~1.29 —
rf1 대비 나빠진 cell 없음(tail 단일 run 편차만). BLOCKERS 3건 중 pipe 2건은 rf3 브리프에 이관, socket_send_pending_submit.cpp
물리 분리(CMake 등록)는 후속.

## D-062 (2026-09-04 02:30, 머신 B, 사용자 지시) PR은 하나로, 시간 단축
사용자: "PR 하나로. 너무 오래 걸린다." → 플랜 §7의 3-PR 대신 perf/phase2-judge 단일 PR. 단축: hotpath gate 도구 job을 rf3와 병렬
(core/build-hp), rf4는 rf3 직후 병렬(core/build-wk). 70 cell 4-size 전체 판정은 PR 뒤 별도(태그 전 필수, D-050)로 이동 — PR 본문에는
1024B 경량 비교 표(리팩토링 전/후, 회귀 없음)를 넣는다.

## D-063 (2026-09-04, 머신 B) rf3·hotpath gate 도구 커밋
rf3(pipe/ypipe/mailbox) 341974c4d6 +206/−357: gate green(test_backpressure_oneway_matrix_single_socket 1회 load timeout → 단독 2회
4.6s PASS; 최종 gate에서 10회 반복 재확인 예정). hotpath gate 도구 커밋: 결정성 3회 ±0.06%, 인위 회귀 3.8× FAIL 확인, 기준값은
감독관이 커밋 트리에서 --update-reference로 재생성(job 값과 0.02% 이내 일치). ctest 135. rf4(wake 테스트) 진행 중.

## D-064 (2026-09-04, 머신 B) candidate multi DEALER_ROUTER_REQREP 간헐 실패 = Core completion 정지 회귀
runner 반복 9회 중 5회 client exit 1(baseline 9/9 성공). 벤치에 진단 출력 추가 후: 포화 구간 뒤 drain에서 client socket 1~3개가
3,094~11,703건 outstanding을 들고 정지(reply·200ms timeout completion 모두 1초 내 미도착). 다른 97~99 socket 정상. 이전 runner는
drain을 요구하지 않아 검출 못 했고(정정 벤치의 two-phase가 드러냄), 1024B 경량 비교의 '-'가 이것. PR 전 수정 필수 →
sol ultra job c016-reqrep-stall(briefs/reqrep-completion-stall.b.prompt, 1.5h 상한). 최종 gate(138/138·backpressure ×10)는 green.

## D-065 (2026-09-04 05:05, 머신 B) 정지 회귀 상류 경계 확정 + origin/main 무결 확인
1차 job(c016-reqrep-stall, 1.5h 상한): 정지 경계 = server session→ROUTER application pipe가 ROUTER에서 한 frame도 소비되지 않은 채
(peer_read=0) 4MiB HWM에서 영구 정지; client 정지는 하류 backpressure. completion cache·timeout task·sub-LWM wake·client reader
wake·수동 HWM 가설 실증 배제. Core diff 0으로 종료. 감독관: origin/main(1ac16a22b2)을 zlink-main-check에 빌드해 같은 벤치로
6/6 성공 → 원인은 이 브랜치의 Core 커밋(8b6c2aa906 최우선: activate_read armed-flag gate / FQ publication opt-in / reclassify
wake 소비 조건 / memory-order 분기). 2차 job c016-reqrep-stall-r2(hunk 단위 A/B 10회씩, 근본 수정 + 결정적 회귀 테스트).

## D-066 (2026-09-04 05:10, 머신 B) 정지 회귀 근본 원인·수정
1차 job이 상한 직전 확정: f3be895b3f의 ROUTER count-1 `xread_activated`/`xread_deactivated` fast path가 route-binding token을 확인하지
않아, pair admission이 ready cache를 먼저 세운 anonymous pipe(identity 프레임이 나중에 오는 경우)를 adopted/FQ 등록된 것으로 오판 →
첫 activation이 미등록 `_fq.activated()` no-op으로 소비되고 slow identity adoption을 영구히 잃음(peer_read=0). 수정: 두 fast path에
`router_route_binding_token() != 0` fence(2줄). 결정적 회귀 테스트 test_count1_router_adopts_anonymous_pipe_on_first_activation
(synthetic pipe/mailbox harness, sleep 없음). job 자체 직접 비교 10/10 성공. origin/main이 6/6 통과한 것은 8b6c2aa906의 타이밍
변화가 발현 확률을 높였기 때문으로 보며, 결함 자체는 main에도 있음(A에 통보 필요). 2차 job(r2) 취소.
