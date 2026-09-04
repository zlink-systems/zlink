START main 브랜치 확인; bindings/java 기존 변경 없음; 타 바인딩의 사용자 변경은 보존하고 범위에서 제외
REVIEW CompletionOwner의 token/context/RID 분기, NO_DATA drain, runtime/public poller 소유권 및 close 경로 조사 시작
PERF_BEFORE DEALER_ROUTER tcp 1024B duration=3 runs=1: 984.667 msg/s, mean=1.740ms p95=4.842ms p99=8.231ms (JDK22 지정)
FIX runtime owner는 POLLCOMPLETION만 대기하도록 spin 제거; terminal errno typed mapping 및 close waiter typed 종료; 즉시 DONTWAIT 성공의 Pending/Future/map 할당 제거
RESUME 기존 4파일 미커밋 변경을 보존해 이어받음; main 브랜치와 Java 범위 확인
PERF_BASE pre-port 70a9998998 + current Core 직접 러너: DEALER_ROUTER tcp 1024B 314733.333 msg/s, mean 0.146ms p95 0.342ms p99 1.882ms (wrapper stale-timestamp gate는 빌드 산출물 직접 실행으로 우회)
PERF_CAUSE CompletionOwner가 매 part마다 shared-copy Message/Arena를 만들고 성공 후 동기 close하여 FFM scope close가 hot path를 지배함; JFR stack 표본과 6974 msg/s 재측정으로 확인
FIX_PERF native zlink_msg header를 attempt Arena에 일괄 shared-copy(refcount) 후 직접 submit; async 즉시 성공은 Pending/Future/map/retained snapshot 없이 완료하고 snapshot은 BACKPRESSURED에서만 보유
PERF_AFTER DEALER_ROUTER tcp 1024B duration=3 runs=1: 789010.333 msg/s, mean=1.142ms p95=2.740ms p99=13.217ms
TEST Contract B backpressure/terminal/route + close/send concurrency 회귀 테스트 5/5 green (sleep 없는 public API 테스트)
SMOKE_TESTS bindings/java/tests/run_tests.sh 전체 :test/:integrationTest/netty/kotlin + samples 7종 green (JDK22, local Core)
FIX_RUNNER single/multi report prune의 pipefail+head SIGPIPE(exit 141) 제거; multi exact smoke env CCU/DUR/SIZES/PATTERNS/TIMEOUT 호환 추가
SMOKE_SINGLE PAIR/DEALER_ROUTER/PUBSUB tcp,inproc 1024B duration=2 runs=1 모두 green; throughput 728193/789724.5, 737749.5/657896.5, 648569/638742.5 msg/s
SMOKE_MULTI exact CCU=8 DUR=2 SIZES=1024,65536 PATTERNS=DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB TIMEOUT=300 green 24/24 (tcp/tls/ws/wss); tcp throughput DD 567142.5/36024.5, DR 101762/35855, PUBSUB 833880.5/91920 msg/s
TEST_RUNNER :perf-multi:test green; single/multi shell syntax와 exact env 실행 검증 완료
FINAL git diff --check와 bash -n 통과; 변경은 bindings/java 6파일로 제한; summary 작성; BLOCKERS 없음
EXIT:0
