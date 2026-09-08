# MP-2 진행

| 시각 (KST) | 상태 |
|---|---|
| 2026-09-07 16:02 | 공통 규칙, MP-1 A안, D-B197/D-B198 확인. detached worktree `zlink-work/mp2`를 main `3a7db427bc`에서 생성. 구현 대상 정찰 시작. |
| 2026-09-07 16:14 | caller별 weak thread-lifetime identity slot 골격 구현. P/D/R의 public marker·control lease 제거, MORE generic lifecycle admission 배선. REPLY socket-wide owner 제거 및 slot-owned RAII checkout으로 전환 중. 첫 빌드 전 정적 오류 정리 단계. |
| 2026-09-07 16:30 | dev 빌드 성공. PAIR·DEALER·ROUTER 4 caller×100 2-part 무혼합 테스트와 REQUEST/REPLY 4 caller 동시 테스트 추가·통과. 기존 거절 계약 테스트 3개를 독립 진행/중복 token registry 계약으로 수정해 관련 4 target 통과. public staging 중 FLOW/WEIGHT 전달 테스트 통과. |
| 2026-09-07 16:41 | FINAL admission을 재점검해 DONTWAIT REQUEST는 complete scope 1쌍을 helper 전부터 physical submit까지 재사용하도록 정리. REPLY 단일 FINAL fast path에는 generic scope가 추가되지 않게 조건화. 관련 target 재빌드 준비. |
| 2026-09-07 16:45 | 최신 scope 수정 포함 관련 8개 target 재빌드 및 핵심 계약 8 tests 통과. dev 전체 build/ctest 회귀 검증으로 확장. |
| 2026-09-07 16:51 | dev 전체 208 tests 완료: 206 pass, REPLY failure-injection unit 1 target(2 assertion)와 dev hotpath gate 실패. REPLY owner 제거로 달라진 실패 주입/복구 경로를 진단하고 release gate는 별도 판정 예정. |
| 2026-09-07 16:56 | REPLY checkout이 첫 MORE로 이동한 계약에 맞춰 OOM 주입 test를 최소 수정하고 통과. 만료 weak caller slot을 다음 유효 helper 접근에서 lock 밖으로 분리·해제하도록 보완했으며 관련 4 tests 재통과. |
| 2026-09-07 17:00 | 관련 정규식 80 tests의 until-fail:5 실행 중. `test_wake_invariants` 5회 연속 통과, 장시간 serial 항목 계속 진행. |
| 2026-09-07 17:04 | 관련 80 tests × until-fail:5 전부 통과(실시간 646.52초). lost-wake 전용 20회 반복으로 확장. |
| 2026-09-07 17:09 | lost-wake 20회 반복 진행 중(장기 `test_wake_invariants` 연속 통과). 요구 (d)를 명시화하려고 DEALER SEND prefix 중 다른 caller REQUEST FINAL 독립 동작 test를 추가했으며 반복 종료 후 빌드 예정. |
| 2026-09-07 17:16 | wake 4 tests × until-fail:20 전부 통과(633.00초), 다른-family 독립 test도 5회 통과. valgrind는 호스트 ld.so debug symbol 부재로 시작 불가하여 ASan 검증으로 전환. |
| 2026-09-07 17:19 | ASan 전용 트리 구성 완료, close/abandoned-prefix 통합 test target 빌드 진행 중. |
| 2026-09-07 17:21 | ASan `detect_leaks=1`로 helper/close 6 cases 전부 통과, sanitizer/leak 오류 없음. 기본 ENABLE_TSAN의 memory/atomic 비계측을 피하려고 GCC `-fsanitize=thread` 직접 구성으로 TSan 트리 준비. |
| 2026-09-07 17:27 | TSan full instrumentation build를 `--parallel 2`로 진행 중(235/572). 기존 `atomic_thread_fence` 관련 GCC 경고 외 compile error 없음. |
| 2026-09-07 17:36 | TSan 전체 계측 관련 80개 실행 완료: 75 통과. 기존 monitor/ctx lock-order 3건, flow-state 동기화 1건, TSan 지연으로 10ms 한계를 넘은 HWM 1건으로 분리했으며 신규 MP-2 계약 테스트는 통과. |
| 2026-09-07 17:41 | Release LTO lib와 `hotpath_bench`를 빌드하고 `PERF_LOCK` 아래에서 5셀 1회 측정. 기준 대비 ratio 0.9734~1.0106으로 전부 ±5% PASS. |
| 2026-09-07 17:46 | 최신 dev 전체 비성능 207개 실행: 206 통과, 기존 `unittest_ctx_lifecycle` transport-owner timing assertion 1건만 병렬 실패. 같은 target 단독 즉시 재실행은 통과하여 비연관 load flake로 판정. |
| 2026-09-07 17:50 | 같은 caller의 유효 flags 변경(`NONE` MORE→`DONTWAIT` FINAL)이 sequence 불일치로 EINVAL·폐기되는 공개 계약 assertion을 명시화. helper ownership과 REQUEST/REPLY 계약 target을 각각 5회 통과. |
| 2026-09-07 17:53 | 마지막 변경을 전체 계측 TSan 트리에 `--parallel 2`로 반영. 신규·직접 수정 8개 target이 suppression 적용 후 전부 통과. 공개 header/version script diff와 금지된 REPLY socket-wide owner 잔존 여부도 재확인. |
| 2026-09-07 17:58 | 구현·규칙 수·expectation 변경·전체 검증·hotpath 5셀·TSan 제한·남은 위험을 `core-rf-MP-2-report.md`에 기록. 최종 diff/보호 경로/worktree 상태 감사 진행. |
