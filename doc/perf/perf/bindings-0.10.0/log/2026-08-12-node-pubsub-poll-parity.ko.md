# Node PUB/SUB poll 측정 방식 정렬 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`를 C 먼저, Node 다음 순서로 단독 실행했다. Release Core `0.10.1`,
clients `100`, duration `1초`, runs `1`, balanced auto-HWM, message size
`64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

Node client는 C `run_recv_duration`과 같이 active deadline 전까지만 측정하고, 한 번의
poll 대기 시간을 최대 `100ms`로 제한한 뒤 ready socket을 `DONT_WAIT`로 drain하도록
수정했다. poller와 event buffer는 active window 전에 준비하므로 초기화 비용은 측정에
포함하지 않는다.

## 결과

| message size | C throughput (msg/s) | Node throughput (msg/s) | C 대비 |
|-------------:|---------------------:|------------------------:|-------:|
| 64B | 1,599,227 | 316,401 | 19.78% |
| 256B | 1,564,132 | 311,896 | 19.94% |
| 1024B | 1,397,798 | 273,426 | 19.56% |
| 4096B | 585,680 | 165,600 | 28.28% |
| 65536B | 97,428 | 47,380 | 48.63% |
| 131072B | 57,320 | 25,630 | 44.72% |
| 산술평균 | - | - | 30.15% |

이전 `poll(-1)` 기반 결과는 active deadline 이후에 drain한 메시지를 포함할 수 있어,
이 표의 C 대비 성능 판정에 사용하지 않는다.

## 결과 파일

- C: `/tmp/zlink-node-pub-topiccache-c/multi/report/perf_c_multi_linux_20260812_220602.txt`
- Node: `/tmp/zlink-node-pub-poll-parity-node3/multi/report/perf_node_multi_linux_20260812_221208.txt`
