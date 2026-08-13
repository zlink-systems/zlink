# Java Dealer/Router request zero-copy 결과

## 변경

`Dealer.request()`의 각 part는 이미 native `msg_t`를 소유한다. 기존 구현은 part마다
`Arena`와 임시 `msg_t`를 만든 뒤 payload를 복사해 `zlink_dealer_request_part`에 전달했다.
성공한 request는 send와 같은 ownership 소비 contract를 따르므로, 원래 native frame을
직접 전달하고 성공 시 Java `Message`를 transferred 상태로 표시하도록 변경했다.

public interface는 변경하지 않았다. 실패한 send는 frame을 소비하지 않으므로 기존 retry와
backpressure 처리를 그대로 사용한다.

## 측정

- Core: release `0.10.1`
- Node: 해당 없음. Java `22.0.2+9`
- 대상: `MULTI_DEALER_ROUTER_REQREP / tcp`
- clients: 100, duration: 2초, runs: 1, balanced auto-HWM
- C 종료 후 Java를 실행했고, 병렬 실행하지 않았다.

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 106,533.5 | 61,634.0 | 57.85% |
| 256B | 103,291.0 | 62,714.0 | 60.72% |
| 1024B | 92,518.5 | 55,582.5 | 60.08% |
| 4096B | 69,180.0 | 58,835.0 | 85.05% |
| 65536B | 24,171.5 | 33,949.5 | 140.45% |
| 131072B | 16,430.5 | 17,893.5 | 108.90% |
| 산술평균 | - | - | 85.51% |

64B는 전체 run에서 client timeout이 한 번 발생했지만 같은 조건의 단독 재실행에서 완료했다.
표에는 단독 64B 결과를 사용했다.

- C report: `/tmp/zlink-java-req-zero-copy-c/multi/report/perf_c_multi_linux_20260813_212927_java-req-zero-copy-c.txt`
- Java 256B 이상 report: `/tmp/zlink-java-req-zero-copy/multi/report/perf_java_multi_linux_20260813_212954_java-req-zero-copy.txt`
- Java 64B report: `/tmp/zlink-java-req-zero-copy-64/multi/report/perf_java_multi_linux_20260813_213046_java-req-zero-copy-64.txt`

## 검증

- JDK 22 `:test --tests systems.zlink.contract.CallbackSendContractTest` 통과
- request/reply termination와 result mapping contract test 통과
