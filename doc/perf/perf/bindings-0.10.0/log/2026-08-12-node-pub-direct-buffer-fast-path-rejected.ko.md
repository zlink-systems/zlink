# Node PUBSUB direct Buffer fast path 측정 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Node를 다음에 단독 실행했다. Release Core `0.10.1`,
clients `100`, duration `1초`, runs `1`, balanced auto-HWM, message size
`64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| 표준 public publish builder의 단일 payload 경로 | 18.41 / 20.52 / 20.42 / 26.17 / 41.65 / 42.11% | 28.21% | 비교값 |
| addon direct Buffer fast path | 19.71 / 21.68 / 21.24 / 31.09 / 45.68 / 41.34% | 30.12% | 원복 |

direct Buffer fast path는 일반 단일 part publish의 동적 타입·multipart 분기를 우회했지만, 선택된
표준 경로보다 종합 성능 개선으로 채택할 수준에 이르지 못했다. public interface는 추가하지 않았고
후보 구현은 원복했다.

Sol 리뷰는 caller-provided `TopicMessage`의 single-part storage 재사용을 다음 후보로 제시했다.
그러나 같은 후보는 이미 `11.22 Node PUBSUB TopicMessage 단일 part 재충전 후보`에서 29.20%로
측정되어 채택값보다 낮아 원복했다. `zlink_msg_init_data()` 기반 direct Buffer zero-copy는 Core
thread에서 실행될 수 있는 release callback과 N-API lifetime을 함께 관리해야 하므로, 현재 public
Buffer send 계약을 유지하는 hot path 후보로 채택하지 않았다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_114456_node-pub-single-part-harness-c.txt`
- Node 표준 비교: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_114528_node-pub-single-part-harness.txt`
- Node direct Buffer: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_114747_node-pub-direct-buffer-fast-path.txt`
