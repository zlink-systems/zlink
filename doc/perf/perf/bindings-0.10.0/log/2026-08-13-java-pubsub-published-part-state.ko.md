# Java PUB/SUB published part 상태 결과

`TopicMessage`가 직전 수신 Message를 public `parts()` List에서 다시 찾지 않고 내부 상태로
직접 유지하도록 변경했다. caller-provided 결과의 소유권과 public interface는 변경하지 않았다.

| Transport | Size | C throughput | Java throughput | Java / C |
|---|---:|---:|---:|---:|
| tcp | 64B | 1,742,117.5 | 1,070,331.5 | 61.44% |
| tcp | 256B | 1,748,858.0 | 1,072,426.5 | 61.32% |
| tcp | 1024B | 617,493.0 | 988,987.5 | 160.16% |
| tcp | 4096B | 610,638.5 | 441,449.0 | 72.29% |
| tcp | 65536B | 112,771.5 | 100,336.5 | 88.97% |
| tcp | 131072B | 49,971.5 | 48,115.5 | 96.29% |
| tcp | 산술평균 | - | - | 90.08% |
| ws | 64B | 1,158,896.5 | 873,234.5 | 75.35% |
| ws | 256B | 1,443,774.5 | 1,016,104.0 | 70.38% |
| ws | 1024B | 1,251,021.5 | 802,373.0 | 64.14% |
| ws | 4096B | 416,525.0 | 311,944.0 | 74.89% |
| ws | 65536B | 58,912.0 | 50,126.5 | 85.09% |
| ws | 131072B | 26,391.5 | 31,885.0 | 120.81% |
| ws | 산술평균 | - | - | 81.78% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_PUBSUB`, balanced auto-HWM
- TCP C report: `/tmp/zlink-java-pubsub-topic-direct-c/multi/report/perf_c_multi_linux_20260813_231343_java-pubsub-topic-direct-c.txt`
- TCP Java report: `/tmp/zlink-java-pubsub-topic-direct-java/multi/report/perf_java_multi_linux_20260813_231412_java-pubsub-topic-direct-java.txt`
- WS C report: `/tmp/zlink-java-pubsub-topic-direct-ws-c/multi/report/perf_c_multi_linux_20260813_231442_java-pubsub-topic-direct-ws-c.txt`
- WS Java report: `/tmp/zlink-java-pubsub-topic-direct-ws-java/multi/report/perf_java_multi_linux_20260813_231502_java-pubsub-topic-direct-ws-java.txt`
