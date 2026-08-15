# Node PUB/SUB Buffer type check 순서 측정 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Node를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다. C와 Node의
`connect_ready_timeout_ms`는 모두 `10000`으로 맞췄다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| 기존: array·type 확인 후 Buffer 확인 | 22.16 / 22.62 / 19.38 / 34.13 / 35.21 / 53.19% | 31.11% | 비교값 |
| 변경: Buffer를 먼저 확인하고 나머지 payload만 array·type 확인 | 21.79 / 23.70 / 21.56 / 32.66 / 49.05 / 50.09% | 33.14% | 채택 |

일반 `Buffer` publish는 Node 사용자가 실제로 가장 많이 사용하는 단일 payload 경로다.
이 경로에서 불필요한 N-API array와 type 확인을 생략했다. native message와 multipart payload의
처리는 기존 분기로 유지하므로 public interface와 message ownership 계약은 바뀌지 않는다.

## 결과 파일

- C: `/tmp/zlink-node-pubbuf-parity-c/multi/report/perf_c_multi_linux_20260812_213824.txt`
- Node 후보: `/tmp/zlink-node-pubbuf-parity-node/multi/report/perf_node_multi_linux_20260812_213855.txt`
- Node 비교값: `/tmp/zlink-node-pubbuf-baseline-node/multi/report/perf_node_multi_linux_20260812_214002.txt`
