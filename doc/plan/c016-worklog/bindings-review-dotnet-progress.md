START: main branch 확인; 기존 변경은 doc/plan/c016-worklog/handoff-A-dontwait-backpressure.ko.md 1건이며 요청 범위 밖이라 보존
BASELINE: DEALER_ROUTER tcp 1024B duration=3 runs=1 throughput=246596.333 msg/s latency=2.701 ms p95=10.452 ms p99=14.262 ms
REVIEW: 계약 B·poller·lifecycle·hot path 감사 완료; busy spin, ownership rollback, 비-completion socket 등록, close/context 종료, ESHUTDOWN mapping, snapshot/HashSet 할당 결함 수정 중
REGRESSION: 신규 public API·sleep-free 7 tests 5회 연속 green; 기존 계약/최적화 포함 focused 45 tests green
RESUME(claude): 이전 worker 미커밋 diff(13 files) 재검토; Core socket_poller가 EINTR을 -1로 반환하므로 Poller.Wait 모든 wait 오류→전체 대기자 실패, runtime pump EINTR→pump 종료·대기자 실패는 과잉 → 종료 오류(ETERM/ESHUTDOWN)에서만 lifecycle 실패, pump는 EINTR 시 계속 대기하도록 수정
TESTS: test_contract_b_regressions.cs 4건 신규(no-route NotConnected Async/TrySubmit, 재-backpressure 재시도 후 각 1회 전달, TrySubmit 토큰 close 회수, 동시 Wait EBUSY가 대기자 실패시키지 않음) + optimization guard 1건; 신규·계약 49 tests 5회 연속 green
NOTE: Core 50d77800f2 router_send_path.cpp:77은 MANDATORY off일 때 no-route RID를 조용히 drop(spec README.en.md:1015 'regardless of MANDATORY'와 불일치) → 바인딩 범위 밖, BLOCKER 항목으로 보고
PERF: DEALER_ROUTER tcp 1024B after=160731 msg/s (load 15); interleaved A/B vs git-archive HEAD copy r1-r5: HEAD 145885/75588/89022/266904/296591 vs current 104626/90830/76217/281153/287042 → 저부하 r4/r5에서 ±5% 이내, 회귀 없음
SMOKE: run_tests.sh 191/191 + samples 7/7 PASS(최종 트리); single smoke 6/6 complete; multi smoke(실제 옵션 --clients 8 --duration 2 --msg-sizes 1024,65536 --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB --transports tcp) 6/6 complete exit 0; dotnet format verify clean; git diff --check clean
SUMMARY: /home/hep7hep7/project/zlink-work/c016/bindings-review-dotnet-summary.md 작성
EXIT:0
