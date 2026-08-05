# Round 109: SPOT fast path A/B recheck

## 이번 라운드 목표

- retained `zlink_spot_send_spot_part()` FINAL-only fast path가 현재 환경에서도 실제 이득을 내는지 A/B로 재확인한다.
- 완료 기준:
  - retained 제거 빌드에서 SPOT_SENDSEND 64B tcp/tls/wss focused perf를 완료한다.
  - round108 retained 결과와 비교해 제거 쪽이 같거나 더 좋으면 retained 변경을 남기지 않는다.
  - retained 쪽이 일관되게 좋으면 fast path를 다시 적용하고 build/test 상태를 맞춘다.

## 기준 report

- round105 retained focused:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_163053_round105_spot_retained_fastpath_recheck.txt`
- round108 retained broad:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_170320_round108_retained_spot_broad_guard.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`

## 병목 가설

1. retained fast path는 staged send helper 비용을 줄이지만, `spot_send_spot_impl()` 내부 검증/route build/vector 생성 비용이 더 커서 실제 효과가 작을 수 있다.
2. round105의 상승은 측정 변동일 수 있다. round108은 SPOT 계열이 전반적으로 낮아 직접 판정이 어렵다.
3. 같은 시간대/같은 pattern에서 retained 제거 결과를 얻으면 fast path 유지 여부를 더 엄격하게 판단할 수 있다.

## POSD 검토

- fast path 자체는 공개 계약을 늘리지 않지만, 효과가 없으면 helper 경로를 하나 더 갖는 복잡도만 남는다.
- 제거는 원래 staged submit 경로로 되돌리는 것이므로 계약을 줄이거나 보안 의미를 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round109_spot_fastpath_removed_ab
```

## 실행

- 임시 변경: retained `zlink_spot_send_spot_part()` FINAL-only fast path 제거.
- build: pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_171258_round109_spot_fastpath_removed_ab.txt`
- load_avg: `40.59 15.05 11.10`
- CTest:
  - command: `ctest --test-dir core/build -R 'spot|zmp_request_reply|request_reply' --output-on-failure`
  - result: 38/38 pass
- `git diff -- core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`: diff 없음
- `git diff --check`: pass

## 결과

| state | pattern | transport | throughput |
|-------|---------|-----------|------------|
| round108 retained | MULTI_SPOT_SENDSEND | tcp | 239659.4 |
| round109 removed | MULTI_SPOT_SENDSEND | tcp | 239584.6 |
| round108 retained | MULTI_SPOT_SENDSEND | tls | 229154.0 |
| round109 removed | MULTI_SPOT_SENDSEND | tls | 224446.4 |
| round108 retained | MULTI_SPOT_SENDSEND | wss | 237525.6 |
| round109 removed | MULTI_SPOT_SENDSEND | wss | 230832.6 |

참고:

| state | pattern | transport | throughput |
|-------|---------|-----------|------------|
| May26 full | MULTI_SPOT_SENDSEND | tcp | 271206.0 |
| May26 full | MULTI_SPOT_SENDSEND | tls | 254009.6 |
| May26 full | MULTI_SPOT_SENDSEND | wss | 252557.8 |
| round105 retained | MULTI_SPOT_SENDSEND | tcp | 256003.6 |
| round105 retained | MULTI_SPOT_SENDSEND | tls | 251436.6 |
| round105 retained | MULTI_SPOT_SENDSEND | wss | 252776.8 |

## 판단

- round109는 load_avg 첫 값이 40.59라 절대 수치 근거로 약하다.
- 같은 시간대 비교로는 retained가 tls/wss에서 조금 높지만, 차이는 각각 약 2.1%, 2.9%로 5% 미만이다. tcp는 사실상 동일하다.
- 계획 문서 기준상 5% 미만 효과는 원칙적으로 오차이며, 효과가 없으면 변경을 남기지 않는다.
- 따라서 retained fast path는 다시 적용하지 않는다.
- 현재 core source diff는 없다. 이번 결론은 성능 개선 채택이 아니라, 불충분한 최적화 후보 제거다.
- 다음 후보는 새 fast path 추가보다 측정 변동을 줄인 상태에서 STREAM/ASIO 또는 PUBSUB one-way의 더 직접적인 병목을 찾아야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않았다.
  - mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않았다.
  - perf runner/client/server를 수정하지 않았다.
