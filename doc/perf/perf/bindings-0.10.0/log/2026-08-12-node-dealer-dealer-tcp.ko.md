# Node multi DEALER/DEALER tcp 측정

release Core `0.10.1`에서 C 다음 Node를 각각 한 번 실행했다. 조건은 tcp,
client 100, duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-node-dd-current-c/multi/report/perf_c_multi_linux_20260812_230643.txt` |
| Node | `/tmp/zlink-node-dd-current-node/multi/report/perf_node_multi_linux_20260812_230740.txt` |

| Size | C throughput | Node throughput | Node/C |
|---:|---:|---:|---:|
| 64B | 2732717 | 502387 | 18.38% |
| 256B | 1485415 | 476932 | 32.11% |
| 1024B | 1028157 | 354865 | 34.51% |
| 4096B | 297415 | 181799 | 61.13% |
| 65536B | 101506 | 40799 | 40.19% |
| 131072B | 45815 | 21450 | 46.82% |
| 산술평균 | - | - | 38.86% |

client는 message Buffer를 재사용하고, server는 Node에 native memory view가 없다는 규칙에 따라
managed Buffer로 수신한다. 남은 작은 message 비용은 public send builder와 addon 경계를 매
message마다 통과하는 비용이다. public interface를 바꾸지 않고 이를 우회하는 경로는 없다.
