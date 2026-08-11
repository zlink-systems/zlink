+# Node STREAM 보류 항목 2차 개선 기록
+
+## 대상과 조건
+
+Release Core `0.10.1`, `tcp`, `MULTI_STREAM`, clients `100`, duration `1초`, runs `1`,
+balanced auto-HWM, 64·256·1024·65536B에서 C와 Node를 순서대로 실행했다. perf는 한 번에
+하나만 실행했고 public interface, packet body materialization, message ownership은 변경하지
+않았다.
+
+## 자체 검토와 Sol 검토
+
+`stream_js_state_t`의 `peer_routing_ids`는 packet마다 mutex 안에서 선형 검색과 복사를 수행했지만
+그 뒤 어떤 동작도 읽지 않는 write-only 상태였다. 상태 필드, 등록·해제 clear와
+`remember_stream_peer_unsafe()`를 제거했다. Sol은 observable behavior가 없고 책임 경계를
+단순화하므로 POSDDD 관점에서 GO로 판정했다.
+
+Sol이 제안한 추가 후보는 TSFN payload 안의 routing-id와 고정 두 packet을 배열로 바꿔 vector
+allocation을 없애는 방식이었다. Node throughput이 dead-state 제거 버전보다 모든 size에서 낮아
+이 후보는 원복했다.
+
+## 검증
+
+- release Core prefix로 `node-gyp configure build` 성공
+- `stream.test.js`와 `multipart.test.js` 5개 통과
+
+## 측정 결과
+
+| Variant | 64B | 256B | 1024B | 65536B | C 대비 평균 | 결과 |
+|---|---:|---:|---:|---:|---:|---|
+| C msg/s | 345,673 | 343,657 | 333,045 | 65,573 | — | 기준 |
+| 기존 Node msg/s | 42,741 | 45,637 | 44,056 | 34,195 | 26.71% | 기존 보류 |
+| write-only peer 상태 제거 Node msg/s | 46,422 | 49,372 | 48,061 | 37,712 | 24.94% | POSDDD 개선 채택, 목표 미달 보류 |
+| 고정 payload 후보 Node msg/s | 45,008 | 47,292 | 47,884 | 36,042 | 24.03% | 원복 |
+
+write-only 상태 제거 후 Node 절대 throughput은 기존 측정 대비 모든 size에서 `8~10%` 올랐다.
+이번 paired C 기준 평균은 `24.94%`로 multi routed echo 최소 평균 `30%`에는 미달하므로
+`tcp/MULTI_STREAM`은 보류를 유지한다.
+
+결과 파일:
+
+- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_073823_node-held2-stream-tcp-c.txt`
+- dead-state 제거: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073858_node-held2-stream-tcp-remove-peer-cache.txt`
+- 제거한 고정 payload 후보: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_074208_node-held2-stream-tcp-fixed-payload.txt`

