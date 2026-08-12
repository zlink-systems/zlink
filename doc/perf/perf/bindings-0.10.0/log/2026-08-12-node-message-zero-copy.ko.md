# Node Message successful-send zero-copy 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_PUBSUB / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | `Message.allocate` zero-copy | Direct Buffer | Zero-copy / C | Buffer / C |
|---:|---:|---:|---:|---:|---:|
| 64B | 1,619,077 msg/s | 343,687 msg/s | 322,813 msg/s | 21.227% | 19.938% |
| 256B | 1,546,825 msg/s | 329,955 msg/s | 362,189 msg/s | 21.331% | 23.415% |
| 1024B | 1,335,309 msg/s | 282,338 msg/s | 302,590 msg/s | 21.144% | 22.661% |
| 4096B | 564,419 msg/s | 163,499 msg/s | 164,667 msg/s | 28.968% | 29.176% |
| 65536B | 105,604 msg/s | 42,250 msg/s | 41,693 msg/s | 40.008% | 39.480% |
| 131072B | 58,245 msg/s | 18,696 msg/s | 24,859 msg/s | 32.098% | 42.680% |
| 평균 | - | - | - | 27.463% | 29.558% |

successful send에서 `Message.data()`가 반환한 external Buffer view를 detach하고 native `msg_t`를 Core로
직접 넘겼다. view가 send 직후 `byteLength == 0`이 되는 contract test를 통과했다. `Message`를 이미
사용하는 application 경로는 이 ownership transfer를 사용한다. 표준 perf는 per-message Message wrapper와
external Buffer 생성 비용이 더 커 direct Buffer 경로를 유지한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_111216_node-pub-zero-copy-c.txt`
- zero-copy report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_111248_node-pub-zero-copy.txt`
- direct Buffer report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_111321_node-pub-zero-copy-buffer.txt`
