START main 확인; bindings/cpp 변경 없음, 사용자 소유 doc 변경 보존; 계약 B 독립 리뷰 시작
BASELINE DEALER_ROUTER tcp 1024B duration=3 runs=1; core/build 링크로 C++만 증분 구성·빌드 시작
BASELINE PASS throughput=697332.330 msg/s latency_mean=3.351ms p95=5.043ms p99=6.706ms
REVIEW 확정: close EBUSY 상태손실, terminal 오류 오매핑, poller 재진입 UAF/close 실패 상태손실, context term waiter hang, POLLOUT 빈 drain·level spin, test runner 병렬도 무시
FIX 1차: lifecycle/terminal·native -1 매핑, poller execution lease/실패 보존, close 순서, completion-only perf loop, 빌드 병렬도 적용
RESUME(2차 worker) 미완 diff 검토; test_cpp_perf_application_ready_queue abort 원인=zlink::poll()에 POLLCOMPLETION(zlink_poll은 EINVAL/702) + STREAM RID 4바이트 규칙 + close EBUSY 비결정 테스트; 테스트 재작성(재-backpressure 새 토큰 시나리오, 비결정 busy-close 삭제) 3/3 green
SMOKE run_tests.sh 15/15+samples PASS; 신규 테스트 5/5, exact_target_retry 5/5; perf single PASS(PAIR tcp 798.1K/inproc 857.3K, DEALER_ROUTER tcp 783.9K/inproc 758.8K, PUBSUB tcp 695.2K/inproc 705.7K msg/s @1024B d=2); AFTER DEALER_ROUTER tcp 1024B d=3 throughput=775154 msg/s lat_mean=4.207 p95=5.775 p99=7.722 (baseline 697332/3.351/5.043/6.706)
