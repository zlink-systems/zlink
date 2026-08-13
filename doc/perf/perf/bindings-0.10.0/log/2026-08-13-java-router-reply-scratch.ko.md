# Java Router reply scratch 재사용 결과

## 변경

Router reply는 part마다 confined `Arena`, native routing-id, native message slot을 새로
만들었다. reply의 기존 move-and-restore ownership 경계는 유지하고, thread-local
scratch의 routing-id와 message slot을 재사용하도록 변경했다.

public interface와 Core ABI는 변경하지 않았다. request send failure에서는 기존처럼
원래 `Message`로 restore한다.

## 측정

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 97,215.0 | 72,796.0 | 74.88% |
| 256B | 108,525.0 | 67,974.0 | 62.64% |
| 1024B | 101,360.5 | 61,442.0 | 60.62% |
| 4096B | 95,090.0 | 57,557.0 | 60.53% |
| 65536B | 25,793.0 | 34,054.0 | 132.03% |
| 131072B | 16,971.0 | 20,827.5 | 122.72% |
| 산술평균 | - | - | 85.57% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_REQREP / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Java 실행, 병렬 실행 없음
- C report: `/tmp/zlink-java-router-reply-c/multi/report/perf_c_multi_linux_20260813_220316_java-router-reply-c.txt`
- Java report: `/tmp/zlink-java-router-reply-java/multi/report/perf_java_multi_linux_20260813_220339_java-router-reply-java.txt`

`CallbackSendContractTest`, `RequestResultMappingContractTest`,
`RequestReplyTerminationContractTest`를 변경 후 다시 실행해 통과했다.
