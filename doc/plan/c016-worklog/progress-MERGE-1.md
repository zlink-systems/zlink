# MERGE-1 진행

- 17:20 시작. worktree `~/project/zlink-work/rel174`, 브랜치 `wip/0.17.4`.
- 17:30 5개 패치 적용 완료·rebase origin/main 성공·push 완료.
  - 커밋: 50ae7ffb42(ALL-2) 45a389ff7c(ALL-2b) fd1a055e31(ALL-3) c414d95d52(RR-1) 84d25131a6(MAC-2)
  - 충돌 1건: core/tests/CMakeLists.txt (RR-1 TIMEOUT 블록 vs MAC-2 scale_test_timeouts include) → 둘 다 유지, include를 마지막에.
  - rebase 충돌 0. public interface diff vs origin/main = 0.
- 다음: dev 빌드 + 전체 ctest.
- 17:40 dev 빌드 성공. 전체 ctest(-E hotpath_gate) 211/211 PASS(234.10s).
- 17:55 변경 suite 147개 until-fail:3 → 147/147 PASS(529.11s).
- 다음: 신규 테스트 until-fail:10, lost-wake until-fail:20.
- 18:05 신규·변경 11 target until-fail:10 → 41/41 PASS(537.75s).
- 18:16 wake 5 target until-fail:20 → 5/5 PASS(641.24s).
- 다음: TSan 트리 구성·빌드.
- 18:25 TSan 트리 구성·빌드 시작(-j4). Windows 호스트 clone(D:\project\zlink-rel174, wip/0.17.4) + CI 구성 빌드 병행 시작.
- 18:35 public interface diff vs origin/main = 0. C 헤더 mirror 8/8 동일(check_c_header_mirror.py PASS). 4개 바인딩 32개 중 31 동일, 1건 bindings/rust/include/zlink/common.h PATCH 2 — origin/main에도 동일하게 존재하는 기존 결함(4cdafee9b7이 zlink.h만 고침).
- 18:40 Windows CI 빌드 성공, 전체 ctest 진행 중(211개).
- 18:50 TSan 전체 ctest 시작(setarch -R, suppression 없음).
- 19:05 TSan 전체 ctest 211/211 PASS, ThreadSanitizer 경고 0(405.69s).
- 19:10 Windows 전체 ctest 211개 중 13 실패 → 실패 target 재실행에서 8개 PASS(BAD_COMMAND 6건은 일시적). 남은 실패 5개:
  writable_resubmit_...(2), test_wake_invariants(large HWM drain), unittest_flow_state_socket(10s timeout), unittest_mutex.
- 19:35 ASan 빌드 중. 다음: ASan 테스트 → release 빌드 → 성능.
- 19:50 ASan 신규·변경 41/41 PASS(54.93s). release-gate 빌드 성공.
- 20:05 hotpath 5셀(idle, PERF_LOCK): 4 PASS / 1 하한 FAIL(dealer_router_reqrep 0.9494 = 개선).
- 20:12 with_stream 6스택 측정 시작.
- 20:52 with_stream 재실행 완료(ZLINK_CORE_SOURCE=local, 실제 worktree release lib). cppserver/cppserver_pull/zmq는 vendored upstream·libzmq 부재로 build_failed(측정 불가).
- 20:55 perf/c multi 4패턴 3-run 시작. 남은 시간상 ws/latency-RSS gate는 미실행 예정.
- 21:25 종료. 보고서 doc/plan/c016-worklog/merge-1-summary.md 작성 완료.
  - 남은 실패: Windows 5 target, WS 비율 gate(ws 0.534). 미실행: latency/RSS gate, with_stream 3개 스택.
