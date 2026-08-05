# Round 131: SPOT_SENDSEND fast path final A/B

## 목표

- 현재 남은 source diff인 `zlink_spot_send_spot_part()` FINAL-only fast path를 다시 검증한다.
- round125 same-window A/B는 긍정적이었지만 round129 reduced full에서는 효과가 선명하지 않았으므로,
  유지/원복을 다시 결정한다.

## 기준

- round129 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_003229_round129_retained_spot_fastpath_lowload_all64_reduced_full.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round122:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`

## 병목 가설

1. `zlink_spot_send_spot_part()`의 단일 FINAL part는 staged sequence builder를 거치지 않아도 된다.
2. 이 fast path는 SPOT_SENDSEND의 C API helper hot path에서 작은 비용을 줄일 수 있다.
3. 다만 reduced full에서는 transport별 변동이 커서 개선이 고정되지 않을 수 있다.

## 먼저 검증할 가설

- `SPOT_SENDSEND tcp,tls,ws,wss 64B` focused A/B에서 retained fast path가 제거 상태보다 하락 없이 높게 나오는지 확인한다.

## POSD/보안 확인

- 공개 API, wire format, socket state, perf runner/client/server는 수정하지 않는다.
- 오류 시 caller part를 consume하는 기존 send 실패 소유권 의미를 유지해야 한다.
- security review 하드닝 항목은 수정하지 않는다.

## 변경 내용

- `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에서 `part_flag_ == ZLINK_PART_FINAL`이고 현재 진행 중인 staged
    sequence가 없으면, staged builder를 만들지 않고 `spot_send_spot_impl()`을 직접 호출한다.
  - 실패 시 `consume_send_part()`를 호출하는 기존 소유권 규칙은 유지한다.

## 검증

- `git diff --check`: 통과
- `cmake --build core/build --target libzlink -j$(nproc)`: 통과
- `ctest --test-dir core/build --output-on-failure -R 'spot|request_reply|zmp_request_reply'`
  - 1차: 37/38 통과, `test_discovery_resolve_spot` 1회 segfault
  - `ctest --test-dir core/build --output-on-failure -R '^test_discovery_resolve_spot$' --repeat until-fail:5`:
    5/5 통과
  - 2차 focused 재실행: 38/38 통과

## A/B 결과

Retained fast path:

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_011326_round131_spot_sendsend_fastpath_retained_current.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,1.10 4.71 4.77`

| pattern | transport | 64B kops |
|---|---:|---:|
| SPOT_SENDSEND | tcp | 265740.0 |
| SPOT_SENDSEND | tls | 253885.0 |
| SPOT_SENDSEND | ws | 245004.4 |
| SPOT_SENDSEND | wss | 256716.2 |

Removed fast path:

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_011928_round131_spot_sendsend_fastpath_removed_ab.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,1.57 3.04 4.01`

| pattern | transport | 64B kops |
|---|---:|---:|
| SPOT_SENDSEND | tcp | 255675.6 |
| SPOT_SENDSEND | tls | 246771.0 |
| SPOT_SENDSEND | ws | 241593.2 |
| SPOT_SENDSEND | wss | 256475.4 |

Retained vs removed:

| transport | delta |
|---|---:|
| tcp | +3.94% |
| tls | +2.88% |
| ws | +1.41% |
| wss | +0.09% |

## 판단

- 5% 이상 개선은 아니지만, focused A/B에서 네 전송 모두 하락 없이 플러스다.
- 사용자가 정한 "하락 항목 없이 플러스면 채택 가능" 기준에 맞으므로 유지한다.
- POSD 관점에서는 공개 인터페이스나 호출자 사전 조건을 늘리지 않고, 단일 FINAL send의 내부 staged
  sequence 비용만 숨기는 변경이다. 깊은 모듈/정보 은닉 원칙을 해치지 않는다.
- 이 변경은 SPOT_SENDSEND에만 좁게 적용되며 STREAM 400kops 목표를 해결하지 않는다. STREAM 개선은
  별도 후보가 필요하다.
