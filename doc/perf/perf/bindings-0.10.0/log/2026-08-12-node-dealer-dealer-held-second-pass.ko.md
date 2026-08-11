+# Node DEALER_DEALER 보류 항목 2차 개선 기록
+
+Release Core `0.10.1`, `tcp`, `MULTI_DEALER_DEALER`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서 C 뒤 Node를 직렬 실행했다.
+
+plain single-part receive는 native managed Buffer를 만든 뒤 `{ data: Buffer }` 내부 envelope에 넣고 TypeScript가 다시 해체했다. Sol 제안에 따라 routing metadata가 없는 이 경로만 Buffer를 직접 반환하고 materializer가 owned Buffer Message로 채택하게 바꿨다. routed·multipart·DONTWAIT, public Message/Received ownership과 `parts` immutability는 변경하지 않았다.
+
+`node-gyp configure build`와 pair·routed·multipart raw tests 20개가 통과했다.
+
+| Size (B) | C msg/s | Node msg/s | C 대비 |
+|---:|---:|---:|---:|
+| 64 | 1,898,785 | 481,059 | 25.33% |
+| 256 | 1,314,808 | 420,691 | 32.00% |
+| 1024 | 1,139,822 | 348,653 | 30.59% |
+| 4096 | 328,909 | 205,299 | 62.42% |
+| 65536 | 110,913 | 53,922 | 48.62% |
+| 131072 | 56,279 | 25,584 | 45.46% |
+
+산술평균은 `40.74%`이며 Node simple one-way 최소 평균 `35%`를 통과했다.
+
+- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_074644_node-held2-dealer-dealer-tcp-c.txt`
+- Node: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_074704_node-held2-dealer-dealer-tcp-direct-buffer-small.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_074726_node-held2-dealer-dealer-tcp-direct-buffer-mid.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_074752_node-held2-dealer-dealer-tcp-direct-buffer-large.txt`
