# Round 154: SPOT request and PUBSUB triage

## goal

- core 64B one-way hot path에서 아직 검증되지 않은 SPOT request 경로와 PUB/SUB matching 경로를 재검토한다.
- 완료 기준: public contract 변경 없이 targeted 64B set에서 하락 없는 개선 후보를 찾거나, 후보를 기각한 근거를 남긴다.

## 시작 상태

- 기준 commit: `903a366c0`
- 시작 core diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path만 retained source diff로 남아 있다.
- perf code는 수정하지 않는다.

## 기준 report

- 원 계획 기준:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 비교 기준으로 더 신뢰하는 May26 full:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 최근 current retained reduced full:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`

## 가설

- 가설 1: `SPOT_REQREP` client가 쓰는 `zlink_spot_request_spot_part(..., ZLINK_PART_FINAL)`도
  staged sequence 비용을 피하면 echo 64B 일부를 올릴 수 있다.
- 가설 2: `PUBSUB`/`SPOT` one-way는 payload보다 fanout/matching 비용이 지배적이므로 mtrie matching 또는
  fanout enqueue 쪽의 중복 작업이 64B 평균을 누르고 있을 수 있다.

## 먼저 검증할 가설

- 먼저 SPOT request 단일 FINAL fast path가 POSD와 소유권 계약을 지키는지 코드로 확인한다.
- 안전하지 않으면 구현하지 않고 PUB/SUB matching 쪽으로 넘어간다.

## 읽은 코드

- `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에는 retained 단일 FINAL fast path가 있다.
  - `zlink_spot_request_spot_part()`는 아직 staged sequence를 항상 사용한다.
- `core/src/api/spot/request_reply/service_spot_request_reply_submit_api.cpp`
  - `spot_request_spot_impl()`은 pending reply 등록 뒤 request message를 build하고 dispatch한다.
  - `start_spot_request_common()`은 build 전 실패와 build 중/dispatch 실패의 part ownership 처리가 다르다.
- `core/src/runtime/services/spot/request_reply/spot_request_reply_local_dispatch.cpp`
  - `build_spot_request_reply_message()`는 control part 생성 또는 payload move 실패 시 caller parts를 소비한다.

## SPOT request fast path 판정

- 기각한다.
- 단일 FINAL direct call 자체는 staged allocation을 줄일 수 있지만 실패 ownership이 명확하지 않다.
- build 전 실패는 wrapper가 caller part를 소비해야 하고, build 중 실패는 builder가 이미 caller part를 소비한다.
- 이 차이를 wrapper가 직접 알아야 하는 구조는 request submit 내부의 세부 실패 지식을 API layer로 새게 만든다.
- POSD 관점에서 정보 은닉을 깨는 후보이므로 perf 수치 측정 전에 배제한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 현재까지 소스 변경 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되살리지 않았다.
  - mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서, decoder/message/send guard, `maxmsgsize` 정책을 수정하지 않았다.
- 추가로 실행한 회귀 테스트:
  - 아직 없음.


## 후보: SPOT local fanout target reserve

적용한 후보:

- `core/src/runtime/services/spot/node/spot_node_pubsub_fanout.cpp`
- `fanout_local_publish()`에서 matching target 목록을 만들기 전에 `targets.reserve(states.size())`를 호출했다.

의도:

- local SPOT fanout에서 구독 대상 수만큼 `targets` vector가 증가할 때 재할당을 줄인다.
- public API, perf code, pub/sub 계약은 수정하지 않는다.

기능 검증:

```bash
cmake --build core/build --target libzlink -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_spot_(pubsub_scenario|dispatch_event|poller|runtime_activation|service_introspection)|unittest_spot_data_plane_'
```

- build: 통과
- CTest: 28/28 통과

후보 perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT,PUBSUB \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round154_spot_fanout_targets_reserve_candidate
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061913_round154_spot_fanout_targets_reserve_candidate.txt`
- runtime: `core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.75 1.75 2.07`
- success 8, fail 0

| case | candidate | round149 refresh | delta |
|------|-----------|------------------|-------|
| `SPOT/tcp` | 4,080,800.0 | 4,146,060.0 | -1.57% |
| `SPOT/tls` | 6,020,188.8 | 6,033,697.0 | -0.22% |
| `SPOT/ws` | 6,104,184.4 | 6,133,867.6 | -0.48% |
| `SPOT/wss` | 5,979,901.4 | 6,076,363.2 | -1.59% |

PUBSUB는 이 후보가 직접 건드리지 않는 경로지만, 같은 targeted run에 포함해 주변 영향을 확인했다.

| case | candidate | round149 refresh | delta |
|------|-----------|------------------|-------|
| `PUBSUB/tcp` | 2,580,930.4 | 2,710,752.4 | -4.79% |
| `PUBSUB/tls` | 2,416,584.4 | 2,397,700.2 | +0.79% |
| `PUBSUB/ws` | 2,251,171.0 | 2,261,698.0 | -0.47% |
| `PUBSUB/wss` | 2,694,702.2 | 2,693,882.4 | +0.03% |

## 후보 판정

- 기각하고 되돌렸다.
- SPOT 4개 transport가 모두 round149 refresh보다 낮아졌다. 차이는 5% 미만이지만, 사용자가 정한
  "하락 항목 없이 플러스면 채택" 기준에는 맞지 않는다.
- `targets.reserve()` 자체는 구조적으로 나쁘지 않지만, 이 작업의 성능 개선 목표에 대한 검증된 이득이 없으므로
  남기지 않는다.
- 되돌린 뒤 `cmake --build core/build --target libzlink -j$(nproc)`를 다시 실행해 runtime을 최종 소스와 맞췄다.

## 보안 하드닝 보존 확인 업데이트

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 후보가 건드린 보안 항목:
  - 없음. SPOT local fanout의 vector capacity 힌트만 임시로 바꿨다가 되돌렸다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되살리지 않았다.
  - mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서, decoder/message/send guard, `maxmsgsize` 정책을 수정하지 않았다.
- 추가로 실행한 회귀 테스트:
  - SPOT/pubsub/data-plane 관련 CTest 28/28 통과.

## 최종 상태

- 이 라운드의 새 source diff는 남기지 않았다.
- retained source diff는 기존 `zlink_spot_send_spot_part()` 단일 FINAL fast path뿐이다.
