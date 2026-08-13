# Node TCP PUB/SUB TopicMessage two-slot 수신 결과

release Core `0.10.1`을 사용했고, C와 Node를 직렬로 실행했다. 조건은 `tcp`,
`MULTI_PUBSUB`, message size `64,256,1024,4096,65536,131072`, duration 5초,
client 100, auto-HWM `balanced`다.

| Size | C throughput (msg/s) | Node throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 1,704,476.0 | 375,803.6 | 22.05% |
| 256 | 1,746,451.6 | 379,911.8 | 21.75% |
| 1,024 | 1,478,228.0 | 313,858.8 | 21.23% |
| 4,096 | 608,708.4 | 199,576.4 | 32.79% |
| 65,536 | 123,280.2 | 54,884.8 | 44.52% |
| 131,072 | 50,665.8 | 21,249.4 | 41.94% |
| 산술평균 | - | - | **30.71%** |

이전 30.15%에서 0.56%p 상승했다. caller-provided `TopicMessage`가 single-part
수신을 반복할 때 wrapper pool acquire/release와 frozen parts 배열 생성을 피한다.
측정 파일은 다음 경로에 있다.

- C: `/tmp/zlink-node-topic-twoslot-c/multi/report/perf_c_multi_linux_20260813_015413_node-topic-twoslot-c.txt`
- Node: `/tmp/zlink-node-topic-twoslot-node/multi/report/perf_node_multi_linux_20260813_015545_node-topic-twoslot-node.txt`
