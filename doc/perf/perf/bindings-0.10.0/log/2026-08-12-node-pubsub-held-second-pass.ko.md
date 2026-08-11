# Node PUBSUB 보류 항목 2차 개선 기록

## 대상과 조건

Release Core `0.10.1`, `MULTI_PUBSUB`, clients `100`, duration `1초`, runs `1`,
balanced auto-HWM 조건으로 C와 Node를 순서대로 실행했다. 모든 perf는 한 번에 하나만
실행했다. 공개 interface와 message ownership, `DONTWAIT` 결과는 변경하지 않았다.

## 후보 검토

`PublisherSocket.publish()`의 per-call forwarding closure를 socket-lifetime submitter로
옮기는 후보를 먼저 비교했다. TCP 평균은 `27.84%`로 기존 `28.24%`보다 낮았다. Sol 검토는
이 변경이 책임 경계를 깊게 만들지 못하고 간접 호출만 추가한다고 판단해 제거했다.

Sol이 제안한 다음 후보는 SUB receive topic 임시 저장소다. addon의 blocking 및 `DONTWAIT`
경로가 매 수신마다 256B `std::vector`를 만들던 동작을 stack-first 저장소로 바꿨다. topic이
256B를 초과해 `EMSGSIZE`가 반환될 때만 heap 저장소를 만든다. 이 저장소는 topic 전달 책임만
가지며 multipart, routing id, native message ownership과 오류 처리는 기존 경로를 사용한다.

## 검증

- `node-gyp configure build`는 release Core prefix로 성공했다.
- `xpub_xsub.test.js`, `multipart.test.js`는 통과했다.
- `public_exports.test.js`의 ESM 검사는 WSL Node 18의 `require()` 제한으로 실패하며 변경과
  무관하다. 패키지가 요구하는 Node 22 환경에서는 이 제약이 없다.

## 측정 결과

| Transport | Size (B) | C msg/s | Node msg/s | C 대비 | 평균 | 판정 |
|---|---:|---:|---:|---:|---:|---|
| tcp | 64 | 1,592,543 | 291,177 | 18.28% | 28.54% | 미달, 개선 채택 |
| tcp | 256 | 1,601,010 | 310,671 | 19.40% |  |  |
| tcp | 1024 | 1,320,105 | 277,283 | 21.00% |  |  |
| tcp | 4096 | 599,845 | 155,263 | 25.88% |  |  |
| tcp | 65536 | 115,136 | 46,605 | 40.48% |  |  |
| tcp | 131072 | 51,749 | 23,919 | 46.22% |  |  |
| ws | 64 | 1,081,305 | 198,765 | 18.38% | 39.58% | 통과 |
| ws | 256 | 1,409,683 | 305,034 | 21.64% |  |  |
| ws | 1024 | 1,134,736 | 260,555 | 22.96% |  |  |
| ws | 4096 | 513,716 | 158,385 | 30.83% |  |  |
| ws | 65536 | 67,603 | 46,191 | 68.33% |  |  |
| ws | 131072 | 35,199 | 26,518 | 75.34% |  |  |

TCP는 직전 closure 후보 대비 `27.84% → 28.54%`로 개선됐지만 simple one-way 최소 평균
`35%`에는 미달한다. WS는 기존 보류 평균 `34.37%`에서 `39.58%`로 올라 최소 기준을 통과했다.

결과 파일:

- TCP C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_072852_node-held2-pub-tcp-c.txt`
- TCP Node: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073342_node-held2-pub-tcp-stack-topic-small.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073354_node-held2-pub-tcp-stack-topic-mid.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073404_node-held2-pub-tcp-stack-topic-large.txt`
- WS C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_073419_node-held2-pub-ws-c.txt`
- WS Node: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073439_node-held2-pub-ws-stack-topic-small.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073451_node-held2-pub-ws-stack-topic-mid.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_073502_node-held2-pub-ws-stack-topic-large.txt`
