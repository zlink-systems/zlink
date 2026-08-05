# Round 102-104: STREAM routing-id decode once probe

## 변경

- `core/src/api/socket/socket_message_send_api.cpp`
  - STREAM routed send에서 `zlink_routing_id_t`를 한 번만 decode한다.
  - callback 내부 current-routing-id fast path의 비교도 decode된 `uint32_t` 값을 재사용한다.
  - 공개 API, benchmark 조건, socket 상태는 바꾸지 않았다.

## POSD 판단

- 인터페이스를 늘리지 않고 기존 `send_stream_message()` 내부 중복만 줄인다.
- 새 상태나 cross-module 캐시를 추가하지 않는다.
- 실패 시 메시지 consume/errno 복구 흐름은 기존과 같다.
- 따라서 설계 복잡도 증가는 작지만, 성능 채택 여부는 별도 검증이 필요하다.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build -R 'stream|multi_stream' --output-on-failure
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round102_stream_decode_once_probe
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round103_stream_decode_once_probe_retry
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round104_decode_once_reduced_guard
```

- build: pass
- stream CTest: 20/20 pass
- round102: complete, fail 0
- round103: complete, fail 0
- round104: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`

## STREAM focused 결과

| transport | May26 full | round90 | round100 | round102 | round103 |
|-----------|------------|---------|----------|----------|----------|
| tcp | 305177.4 | 320253.2 | 280166.0 | 327340.4 | 329223.4 |
| tls | 214574.6 | 219801.0 | 209046.0 | 224044.6 | 212360.4 |
| wss | 184722.2 | 190692.6 | 175863.2 | 183232.6 | 184720.8 |

round103 기준:

- tcp: May26 full 대비 +7.88%, round90 대비 +2.80%, round100 대비 +17.51%
- tls: May26 full 대비 -1.03%, round90 대비 -3.39%, round100 대비 +1.59%
- wss: May26 full 대비 -0.00%, round90 대비 -3.13%, round100 대비 +5.04%

## reduced guard 결과

| pattern | transport | May26 full | round90 | round100 | round104 |
|---------|-----------|------------|---------|----------|----------|
| MULTI_PUBSUB | tcp | 2661635.6 | 2548363.4 | 2368520.0 | 2417467.8 |
| MULTI_PUBSUB | tls | 2623065.0 | 2278477.4 | 2216652.6 | 2264853.0 |
| MULTI_PUBSUB | wss | 2760571.0 | 2494252.0 | 2465419.0 | 2503941.8 |
| MULTI_SPOT_SENDSEND | tcp | 271206.0 | 245644.8 | 253359.0 | 255037.0 |
| MULTI_SPOT_SENDSEND | tls | 254009.6 | 233617.4 | 249998.2 | 239642.8 |
| MULTI_SPOT_SENDSEND | wss | 252557.8 | 248696.6 | 244308.4 | 254028.6 |
| MULTI_SPOT_REQREP | tcp | 252212.6 | n/a | 252806.8 | 256498.0 |
| MULTI_SPOT_REQREP | tls | 229720.4 | n/a | 223681.2 | 229104.0 |
| MULTI_SPOT_REQREP | wss | 219301.2 | n/a | 214421.2 | 209621.2 |
| MULTI_STREAM | tcp | 305177.4 | 320253.2 | 280166.0 | 325469.2 |
| MULTI_STREAM | tls | 214574.6 | 219801.0 | 209046.0 | 215810.6 |
| MULTI_STREAM | wss | 184722.2 | 190692.6 | 175863.2 | 178303.6 |

round104 기준 주요 변화:

- STREAM/tcp: May26 full 대비 +6.65%, round90 대비 +1.63%, round100 대비 +16.17%
- STREAM/tls: May26 full 대비 +0.58%, round90 대비 -1.82%, round100 대비 +3.24%
- STREAM/wss: May26 full 대비 -3.47%, round90 대비 -6.50%, round100 대비 +1.39%
- SPOT_SENDSEND/tls: round100 대비 -4.14%
- SPOT_REQREP/wss: round100 대비 -2.24%

## 판단

- 이 후보는 POSD 관점에서는 허용 가능한 작은 내부 정리다.
- 하지만 사용자가 말한 "하락 항목 없이 +면 채택" 기준에는 아직 맞지 않는다.
- STREAM/tcp는 반복적으로 개선되지만 tls/wss는 변동성이 있고, reduced guard에서 SPOT 일부 하락도 같이 관측됐다.
- 따라서 채택하지 않고 source 변경은 되돌렸다. 다음 단계에서 재검증할 수 있도록 결과만 로그로 남긴다.
