# Node native Message send 경로 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 병렬 실행: 없음
- 대상: `MULTI_PUBSUB / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM
- 순서: C를 먼저 실행하고 lazy `Message.from(Buffer)`와 direct Buffer를 각각 단독 실행

## 측정값

| Size | C throughput | `Message.from(Buffer)` | Direct Buffer | Message / C | Buffer / C |
|---:|---:|---:|---:|---:|---:|
| 64B | 1,596,017 msg/s | 306,661 msg/s | 296,375 msg/s | 19.214% | 18.570% |
| 256B | 1,586,962 msg/s | 322,337 msg/s | 326,512 msg/s | 20.312% | 20.575% |
| 1024B | 1,227,804 msg/s | 278,089 msg/s | 274,610 msg/s | 22.649% | 22.366% |
| 4096B | 470,584 msg/s | 169,208 msg/s | 148,101 msg/s | 35.956% | 31.472% |
| 65536B | 103,884 msg/s | 42,094 msg/s | 44,570 msg/s | 40.519% | 42.903% |
| 131072B | 59,882 msg/s | 23,315 msg/s | 26,204 msg/s | 38.935% | 43.759% |
| 평균 | - | - | - | 29.598% | 29.841% |

`Message.from(Buffer)`는 생성 때 source payload를 native `msg_t`에 복사하고 send 때 native frame을
공유한다. send-only 경로는 `data()`를 호출하지 않으므로 external Buffer view를 lazy하게 만들도록 변경했다.
PUB/SUB 전체 결과는 direct Buffer보다 0.24%p 낮아 이 경로만으로 standard aggregate를 올리지는 못했다.
다만 send-only Message가 사용하지 않는 JS Buffer·finalizer를 만들지 않으므로 구현은 유지한다.

이 측정 뒤 successful send에서 payload view를 detach해 native frame ownership을 Core로 이전하도록 수정했다.
수정 뒤의 `Message.allocate().data()` zero-copy 결과는
`2026-08-12-node-message-zero-copy.ko.md`에 기록한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_110152_node-pub-lazy-message-c.txt`
- lazy Message report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_110228_node-pub-lazy-message-from.txt`
- direct Buffer report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_110256_node-pub-lazy-buffer-direct.txt`
- allocate report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_105846_node-pub-message-allocate.txt`
