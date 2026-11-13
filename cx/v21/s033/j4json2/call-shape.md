# Java L3/L4 profile 호출 비교

기준 `9d9901eb2c`, scratch 전용 build. CPU·allocation shape만 비교한다. throughput 판정·측정 lock 사용 없음. bench DTO, 10 s warmup·5 s active, window 100·submit concurrency 8은 유지한다. JVM별 수명 상한은 timeout 19 s + kill grace 0.3 s다. affinity는 모든 process와 thread에 6-7,14-15다.

경로의 전체 prefix는 `framework/bench/grpc/java/`이며 원문은 raw의 `RawStack.txt`, `ZLinkRawBenchServer.txt`, `ZLinkFrameworkBenchServer.txt`에 보존한다.

| 경로 | binding | L3 | L4 | 복원 기능·비용 |
|---|---|---|---|---|
| source send | RawStack.java:243-274 | :280-318 | :309-339,457-466 | 같은 binding submit·completion. L3 frame pack, L4 기본 typed JSON encode 추가 |
| target receive | ZLinkRawBenchServer.java:94-109 | ZLinkFrameworkBenchServer.java:351-419 | :272-323 | blocking recv 1회, 같은 metrics. permit·envelope·service frame decode는 공통 복원분. L4 typed JSON decode 추가 |
| target reply | ZLinkRawBenchServer.java:115-127 | ZLinkFrameworkBenchServer.java:400-419 | :324-340 | 새 reply envelope/body, 같은 submit·part 수. L3 framing, L4 typed JSON encode 추가; request part 재사용 없음 |
| source reply completion | RawStack.java:151-180,404-433 | :402-433 | :434-477 | 같은 completion poller·검증; L3 frame unpack, L4 typed JSON decode 추가 |

L3→L4의 typed payload와 content type 변경은 복원하는 codec 기능이다. receive primitive·wait 수·metrics atomic에는 codec 변경이 없다. profile 중 output JSON은 진단 자료이며 msg/s 비교나 ladder gate의 근거로 사용하지 않는다.
