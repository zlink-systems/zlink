# Node STREAM inline routing-id 측정 결과

STREAM callback payload의 routing id는 최대 255 bytes인 고정 Core 구조체다. callback마다
`vector<unsigned char>`를 만들지 않고 payload 안의 inline `zlink_routing_id_t`에 복사하도록
바꿨다. public routing-id bytes와 packet callback 계약은 변경하지 않았다.

Core release `0.10.1`, `MULTI_STREAM / tcp`, clients `100`, duration `1초`, runs `1`,
balanced auto-HWM, 64·256·1024·65536B 조건에서 C를 먼저, Node를 다음에 단독 실행했다.

| Message size | C throughput | Node throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 347,874 ops/s | 44,907 ops/s | 12.91% |
| 256B | 351,850 ops/s | 47,603 ops/s | 13.53% |
| 1024B | 340,060 ops/s | 50,682 ops/s | 14.90% |
| 65536B | 61,928 ops/s | 34,701 ops/s | 56.03% |
| 산술평균 | - | - | 24.34% |

새 C paired 평균은 이전 26.67%보다 낮다. 다만 Node throughput은 이전 결과의
44,152/46,296/47,908/32,701 ops/s보다 각각 높고, packet의 고정 routing-id를 가변 container로
표현하던 책임 혼합을 제거하므로 POSDDD 정리로 유지한다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_093740_node-stream-inline-rid-c.txt`
- Node report: `/home/hep7hep7/project/zlink/bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_093819_node-stream-inline-rid.txt`
