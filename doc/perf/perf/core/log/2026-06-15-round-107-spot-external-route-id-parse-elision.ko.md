# Round 107: SPOT external route id parse elision

- goal: SPOT routed delivery가 외부 라우터로 보낼 때 이미 알고 있는 destination node routing id를 재사용해 envelope 재파싱 비용을 줄인다.
- 시작 시각: 2026-06-15 KST
- 기준 문서: `doc/plan/perf/core/core-library-performance-improvement-plan.ko.md`
- 비교 기준:
  - May26 full: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - round100: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
  - round105: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_163053_round105_spot_retained_fastpath_recheck.txt`
- 현재 retained diff:
  - `zlink_spot_send_spot_part()` FINAL-only fast path
  - 이번 후보: `dispatch_spot_routed_delivery()`에 optional external route id를 전달한다.

## POSD 검토

- 문제: submit 계층은 destination node routing id를 이미 알고 있는데, delivery 계층은 외부 라우터 전송 직전에 built envelope를 다시 파싱해 같은 id를 꺼낸다.
- 선택한 방향: transport 전송 결정은 delivery 계층에 그대로 두고, submit 계층은 이미 보유한 routing id 포인터만 전달한다.
- 피한 방향:
  - submit 계층에서 external router를 직접 다루지 않는다.
  - request pending/timeout 경로를 우회하는 별도 request fast path를 만들지 않는다.
  - WebSocket frame 의미나 pending message 정책을 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - WebSocket frame read/write 의미와 max message 정책을 바꾸지 않는다.
  - mtrie, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build -R 'spot|zmp_request_reply|request_reply|transport_matrix|multi_socket_contract' --output-on-failure
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND,SPOT_REQREP --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round107_spot_external_route_id_parse_elision
```

## 실행

- build: pass
- CTest:
  - command: `ctest --test-dir core/build -R 'spot|zmp_request_reply|request_reply|transport_matrix|multi_socket_contract|actor' --output-on-failure`
  - result: 40/40 pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- result file: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_165252_round107_spot_external_route_id_parse_elision.txt`

## 결과

| pattern | transport | May26 full | round100 | round105 | round107 | vs round105 |
|---------|-----------|------------|----------|----------|----------|-------------|
| MULTI_SPOT_SENDSEND | tcp | 271206.0 | 253359.0 | 256003.6 | 250696.4 | -2.07% |
| MULTI_SPOT_SENDSEND | tls | 254009.6 | 249998.2 | 251436.6 | 253042.4 | +0.64% |
| MULTI_SPOT_SENDSEND | wss | 252557.8 | 244308.4 | 252776.8 | 250614.0 | -0.86% |
| MULTI_SPOT_REQREP | tcp | 252212.6 | 252806.8 | 255914.6 | 252092.4 | -1.49% |
| MULTI_SPOT_REQREP | tls | 229720.4 | 223681.2 | 232934.0 | 232051.2 | -0.38% |
| MULTI_SPOT_REQREP | wss | 219301.2 | 214421.2 | 210854.2 | 207675.6 | -1.51% |

## 판단

- 후보 폐기. 설계상 delivery 계층에 routing id를 전달하는 것은 허용 가능했지만, focused perf에서 하락 항목이 여러 개 나왔다.
- "작은 개선이라도 하락 항목이 없으면 채택" 기준을 충족하지 못한다.
- source는 되돌렸다. 이 라운드 뒤에 유지되는 source 변경은 `zlink_spot_send_spot_part()` FINAL-only fast path뿐이다.
- 원복 확인:
  - `git diff -- core/src/api/actor/spot/service_spot_actor_api.cpp core/src/api/spot/request_reply/service_spot_request_reply_internal.hpp core/src/api/spot/request_reply/service_spot_request_reply_routed_delivery.cpp core/src/api/spot/request_reply/service_spot_request_reply_submit_api.cpp`: diff 없음
  - `git diff --check`: pass
  - `cmake --build core/build -j$(nproc)`: pass after revert
  - second `cmake --build core/build -j$(nproc)`: pass, no clock skew warning repeated
