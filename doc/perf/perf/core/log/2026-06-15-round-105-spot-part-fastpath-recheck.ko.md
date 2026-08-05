# Round 105: SPOT part fast path 재검토

- goal: retained SPOT_SENDSEND single FINAL fast path를 기준 runtime으로 고정하고, 같은 구조의 추가 part fast path가 POSD-safe인지 재검토한다.
- 시작 시각: 2026-06-15 16:29:02 KST
- 기준 문서: `doc/plan/perf/core/core-library-performance-improvement-plan.ko.md`
- 주요 비교 기준:
  - May26 full: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - round90 current recalibration: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_145417_round90_current_regression_recalibration.txt`
  - round100 reduced stability: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
- 현재 source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`의 `zlink_spot_send_spot_part()` FINAL-only fast path만 남김.
  - STREAM decode-once 후보는 round102-104에서 mixed 결과라 되돌림.
- perf 전용 변경: 없음.

## 병목 가설

1. SPOT_SENDSEND 64B는 single FINAL part 호출에서 staged send sequence 준비 비용이 hot path에 남는다.
   - 이미 남긴 fast path는 `spot_send_spot_impl()`으로 바로 넘겨 `handle_state`/`vector` staging을 피한다.
2. SPOT_REQREP 64B client/server도 request/reply part API에서 같은 staging 비용을 가진다.
   - 하지만 request는 pending reply 등록, timeout, 실패 시 pending 제거가 엮인다.
   - reply fast path는 round95에서 wss 하락이 있어 폐기했다.
3. PUBSUB empty subscription 경로는 SUB 수신과 mtrie root match가 이미 최단 경로에 가깝다.
   - XPUB send-all 우회는 공개 필터 계약을 깨뜨릴 수 있어 제외한다.

## 먼저 검증할 가설

- H1: 현재 남긴 `zlink_spot_send_spot_part()` fast path는 source/runtime 기준을 흐리지 않고 재현 가능한 이득을 유지하는가.
- H2: 추가 fast path는 `spot_request_spot_part()`에만 한정해도 복잡도 대비 수치가 충분한가. 실패 경로가 넓으므로 구현 전 코드 기준 검토를 먼저 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. WS/WSS pending copy, mtrie, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
- 보안 의미를 유지한 근거: 현재 검토 대상은 spot part API submit staging이며 보안 하드닝 대상 파일/정책을 변경하지 않는다.
- 추가로 실행할 회귀 테스트: spot/request-reply 관련 CTest, 필요 시 reduced perf.

## 실행

```bash
ctest --test-dir core/build -R 'spot|zmp_request_reply|request_reply' --output-on-failure
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND,SPOT_REQREP --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round105_spot_retained_fastpath_recheck
```

- CTest: 38/38 pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`

## 결과

| pattern | transport | May26 full | round90 | round100 | round105 | vs May26 | vs round90 | vs round100 |
|---------|-----------|------------|---------|----------|----------|----------|------------|-------------|
| MULTI_SPOT_SENDSEND | tcp | 271206.0 | 245644.8 | 253359.0 | 256003.6 | -5.61% | +4.22% | +1.04% |
| MULTI_SPOT_SENDSEND | tls | 254009.6 | 233617.4 | 249998.2 | 251436.6 | -1.01% | +7.63% | +0.58% |
| MULTI_SPOT_SENDSEND | wss | 252557.8 | 248696.6 | 244308.4 | 252776.8 | +0.09% | +1.64% | +3.47% |
| MULTI_SPOT_REQREP | tcp | 252212.6 | n/a | 252806.8 | 255914.6 | +1.47% | n/a | +1.23% |
| MULTI_SPOT_REQREP | tls | 229720.4 | n/a | 223681.2 | 232934.0 | +1.40% | n/a | +4.14% |
| MULTI_SPOT_REQREP | wss | 219301.2 | n/a | 214421.2 | 210854.2 | -3.85% | n/a | -1.66% |

## 판단

- `zlink_spot_send_spot_part()` FINAL-only fast path는 SPOT_SENDSEND focused 재검증에서 tcp/tls/wss 모두 round100보다 상승했다.
- May26 full 대비로는 tcp가 아직 -5.61%지만, tls는 -1.01%, wss는 +0.09%라 과거 기준에 거의 붙었다.
- 추가 `spot_request_spot_part()` fast path는 이번 라운드에서 구현하지 않는다.
  - request 경로는 pending reply 등록, timeout, 실패 시 pending 제거가 엮여 단순 send fast path보다 실패면이 넓다.
  - SPOT_REQREP/tcp/tls는 이미 May26 full보다 높고, 남은 낮은 항목은 wss다.
  - wss 하락은 request staging 비용보다 transport/routed delivery 변동 가능성이 커서, 지금 추가 fast path를 넣으면 POSD 대비 성능 근거가 약하다.
- 현재 retained 변경은 유지하고, 다음 후보는 SPOT_REQREP/wss 또는 STREAM/wss transport 경로의 공통 원인 확인으로 넘긴다.
