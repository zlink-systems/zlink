# Node STREAM 고정 packet 저장소 측정 결과

## 대상과 조건

`MULTI_STREAM / tcp`에서 STREAM callback이 항상 전달하는 header와 body 두 메시지를
가변 `vector` 대신 고정 2칸 저장소에 보관했다. 공개 interface와 packet body
materialization 선택은 변경하지 않았다.

Core release `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
64·256·1024·65536B 조건으로 C를 먼저, Node를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Node throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 296,777 ops/s | 44,152 ops/s | 14.88% |
| 256B | 294,879 ops/s | 46,296 ops/s | 15.70% |
| 1024B | 272,970 ops/s | 47,908 ops/s | 17.55% |
| 65536B | 55,874 ops/s | 32,701 ops/s | 58.53% |
| 산술평균 | - | - | 26.67% |

이전 같은 경로의 산술평균 24.94%보다 1.73%p 높다. `stream_js_payload_t`가
고정된 두 packet을 직접 소유하도록 하여 매 callback의 container allocation을 제거했다.
`dist-tools/tests/stream.test.js` 4개 테스트가 통과했다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_091001_node-stream-fixed-payload-c.txt`
- Node report: `/home/hep7hep7/project/zlink/bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_091035_node-stream-fixed-payload.txt`
