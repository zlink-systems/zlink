# Node PUB scalar Buffer consume 최적화 측정 결과

## 변경

scalar Buffer publish는 native-backed `Message`가 아니므로 성공 뒤 `nativeMessage` property를
찾아 ownership을 consume할 필요가 없다. native-backed scalar와 multipart만 기존 consume 경로를
사용하도록 분리했다. public interface, Message ownership, HWM/backpressure, topic semantics은
변경하지 않았다.

Core release `0.10.1`, `MULTI_PUBSUB / tcp`, clients `100`, duration `1초`, runs `1`,
balanced auto-HWM, 64·256·1024·4096·65536·131072B 조건에서 C를 먼저, Node를 다음에
단독 실행했다.

## 결과

| Message size | C throughput | Node throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,660,821 msg/s | 290,817 msg/s | 17.51% |
| 256B | 1,664,088 msg/s | 341,203 msg/s | 20.50% |
| 1024B | 1,410,922 msg/s | 268,071 msg/s | 19.00% |
| 4096B | 583,209 msg/s | 160,490 msg/s | 27.52% |
| 65536B | 100,484 msg/s | 46,774 msg/s | 46.55% |
| 131072B | 51,295 msg/s | 27,013 msg/s | 52.66% |
| 산술평균 | - | - | 30.62% |

이전 TCP PUBSUB 28.54%보다 2.08%p 높다. native build와 `xpub_xsub.test.js`가 통과했다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_093553_node-pub-buffer-consume-c.txt`
- Node report: `/home/hep7hep7/project/zlink/bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_093630_node-pub-buffer-consume.txt`
