# Java ROUTER critical receive 측정 결과

## 대상과 변경

`MULTI_DEALER_ROUTER / tcp`의 `DONT_WAIT` 수신은 non-blocking Core 호출이다. 이 호출을
이미 정의된 critical FFM downcall로 전환했다. 또한 receive support와 socket이 연속으로
수행하던 routed sender 연결을 socket 계층 한 곳으로 제한했다. receive support는 수신 결과를
채우고 socket은 send capability를 연결한다.

공개 interface, Core ABI, `EINTR` 재시도, `NO_DATA`·`BUSY` error mapping, Message ownership은
변경하지 않았다. Sol read-only review는 두 변경을 GO로 판정했다.

Core release `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
64·256·1024·4096·65536·131072B 조건에서 C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 180,351 ops/s | 55,275 ops/s | 30.65% |
| 256B | 179,834 ops/s | 50,378 ops/s | 28.01% |
| 1024B | 173,108 ops/s | 53,654 ops/s | 30.99% |
| 4096B | 165,104 ops/s | 50,140 ops/s | 30.37% |
| 65536B | 37,068 ops/s | 30,099 ops/s | 81.20% |
| 131072B | 20,526 ops/s | 16,106 ops/s | 78.47% |
| 산술평균 | - | - | 46.62% |

이전 같은 TCP pattern의 42.45%보다 4.17%p 높다. Java 전체 테스트가 통과했다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_091306_java-router-critical-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_091324_java-router-critical.txt`
