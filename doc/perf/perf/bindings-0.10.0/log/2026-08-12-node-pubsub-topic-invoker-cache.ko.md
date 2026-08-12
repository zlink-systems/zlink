# Node MULTI_PUBSUB topic invoker cache 결과

## 대상과 조건

- 대상: `tcp / MULTI_PUBSUB`
- 조건: C를 먼저 단독 실행한 뒤 Node를 단독 실행했다. `duration=1`, `runs=1`, `clients=100`, balanced auto-HWM을 사용했다.
- Core: release `0.10.1`

## 변경

`PublisherSocket.publish(topic)`이 동일 topic을 반복 사용할 때 topic 검증 결과와 submit invoker를 재사용하도록 변경했다. 호출마다 독립적인 `SendOperation`은 계속 생성하며 public interface, message ownership, multipart와 backpressure 결과는 바꾸지 않았다.

## 결과

| Size | C Kmsg/s | Node Kmsg/s | Node/C |
|------|---------:|------------:|-------:|
| 64B | 1422.116 | 322.778 | 22.70% |
| 256B | 1463.380 | 297.843 | 20.35% |
| 1024B | 1325.424 | 291.788 | 22.01% |
| 4096B | 462.391 | 173.590 | 37.54% |
| 65536B | 97.011 | 44.589 | 45.96% |
| 131072B | 50.472 | 23.477 | 46.52% |
| 산술 평균 | - | - | **32.51%** |

기존 30.56%보다 1.95%p 개선됐지만 최소 기준 35%에는 2.49%p 미달한다. Node typecheck, native Release build와 `xpub_xsub.test.js`가 통과했다.

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_134004_node-pub-topic-cache-c.txt`
- Node: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_134200_node-pub-topic-cache.txt`

