# #4352 Java L2 request-backpressure 1K — 추가 측정

## 결과

측정은 **실행하지 않음 (0/5)**. 공통 규칙의 receive parity 조건을 만족하지 않아 유효한 비교가 아니기 때문이다. binding은 요청마다 blocking `recv` 한 번이며 프레임워크 permit이나 drain이 없다([`ZLinkRawBenchServer.java:94-109`](../../../../../../worktree/zlink-v21-s033/lad-java-l2r/framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java)). L2는 프레임워크 permit을 획득하고 blocking `recv` 뒤 `DONT_WAIT` drain을 수행한다([`ZLinkFrameworkBenchServer.java:123-155`](../../../../../../worktree/zlink-v21-s033/lad-java-l2r/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java)). 도구 설정만으로 이 차이를 없앨 수 없어 lock 측정을 시작하지 않았다.

| 구분 | L2 처리량 (msg/s) | median | G5 spread | 근거 |
|---|---:|---:|---:|---|
| 추가 5회 | 측정 안 함 | — | — | parity gate에서 중단 |
| 이전 5회 run1 | 142,235.4 | — | — | [raw](../lad-java-l2b/raw/v2/request-backpressure-1K/run1/zlink-framework-java-request-backpressure-1024/results.json) |
| 이전 5회 run2 | 134,050.8 | — | — | [raw](../lad-java-l2b/raw/v2/request-backpressure-1K/run2/zlink-framework-java-request-backpressure-1024/results.json) |
| 이전 5회 run3 | 136,888.0 | — | — | [raw](../lad-java-l2b/raw/v2/request-backpressure-1K/run3/zlink-framework-java-request-backpressure-1024/results.json) |
| 이전 5회 run4 | 136,130.6 | — | — | [raw](../lad-java-l2b/raw/v2/request-backpressure-1K/run4/zlink-framework-java-request-backpressure-1024/results.json) |
| 이전 5회 run5 | 112,731.0 | — | — | [raw](../lad-java-l2b/raw/v2/request-backpressure-1K/run5/zlink-framework-java-request-backpressure-1024/results.json), UTC `2026-11-13T11:05:53.520930342Z` |

이전 집합은 median **136,130.6**, G5 spread **17.19% 실패**이며 run5가 저측 이상치다(위 5개 raw 파일). 새 결과가 없어 10회 통합 median/spread와 단계 손실은 계산할 수 없다. 따라서 이번 verdict는 **판정 불가**다. 1K 추가 측정의 raw 파일은 생성되지 않았다.

## 준비

`framework/bench/grpc/java/build.sh` 성공([build log](raw/build.log)); client·framework-server·raw-server JAR 모두 이 worktree의 대응 Java source보다 최신이다(sha256·빌드 commit은 [progress.md](progress.md)). L2b 도구 복사본은 `tools/`에 두고 JOB/raw/worktree 경로만 L2r로 바꿨다. 코드 변경·commit·push 없음.
