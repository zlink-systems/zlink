# Node PUBSUB socket-owned submitter 후보 측정 결과

`publish(topic)`마다 만드는 capture lambda를 socket-owned submitter로 바꾸어 비교했다.
public interface, topic validation, Message ownership과 backpressure 동작은 변경하지 않았다.

Core release `0.10.1`, `MULTI_PUBSUB / tcp`, clients `100`, duration `1초`, runs `1`,
balanced auto-HWM, 64·256·1024·4096·65536·131072B 조건에서 C를 먼저, Node를 다음에
단독 실행했다.

| Message size | C throughput | Node throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,659,834 msg/s | 278,397 msg/s | 16.77% |
| 256B | 1,610,255 msg/s | 329,455 msg/s | 20.46% |
| 1024B | 1,389,230 msg/s | 261,249 msg/s | 18.81% |
| 4096B | 573,674 msg/s | 138,199 msg/s | 24.09% |
| 65536B | 101,378 msg/s | 40,491 msg/s | 39.94% |
| 131072B | 50,835 msg/s | 22,713 msg/s | 44.68% |
| 산술평균 | - | - | 27.46% |

이전 TCP PUBSUB 28.54%보다 낮아 후보를 원복했다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_092959_node-publish-invoker-c.txt`
- Node report: `/home/hep7hep7/project/zlink/bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_093031_node-publish-invoker.txt`
