# Round 10: 소형 LMSG 블록 풀 검토

- 기준 커밋: `5e3c438a2`
- 시작 상태: core source diff 없음. `round-9-publish-part-fast-path` 로그만 미추적 상태.
- 대상: core runtime 메시지 소유 버퍼 할당/해제 경로
- 제외: `bindings/c/perf` runner, client, server 수정

## 기준 수치

- 이전 zero-fail 전체 기준: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
  - success 192, fail 0
  - runtime: `core/build`
- 문제 재현 기준: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 장기 기준: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`

`151925`는 failure 0 상태를 확보했지만, 64B one-way 계열은 아직 이전 수치보다 낮다.
문제 재현 기준 대비 큰 차이는 `MULTI_SPOT tcp` -9.94%, `MULTI_PUBSUB tcp`
-8.92%이고, 장기 기준 대비로는 `MULTI_SPOT`, `MULTI_PUBSUB`,
`MULTI_DEALER_DEALER`, `MULTI_STREAM` 64B 계열의 하락 폭이 더 크다.

## 가설

1. 64B payload는 `msg_t::max_vsm_size`를 넘어서 LMSG가 된다. 짧은 one-way
   경로에서는 `content_t + payload` malloc/free 비용이 공통 병목일 수 있다.
2. 병목이 pipe, lifecycle, transport 내부라면 소형 LMSG 블록 재사용은 10% 이상의
   의미 있는 개선을 만들지 못한다.

## 실험 방향

- `msg_t::init_size()`에서 생성하는 소유 LMSG storage만 대상으로 한다.
- 외부 storage, decoder shared buffer, zero-copy view storage는 기존 수명 규칙을 유지한다.
- LMSG view가 원본 LMSG를 마지막으로 해제하는 경로도 같은 storage release helper를
  사용하게 하여 direct `free()` 경로가 남지 않게 한다.
- 공개 `zlink_msg_t` 크기와 API 계약은 변경하지 않는다.

## 보안/계약 보존 항목

- `maxmsgsize` 검증, decoder allocation overflow guard, multipart send guard는 변경하지 않는다.
- WS receive copy hardening, mtrie recursion hardening, proxy/monitor 보안 수정은 변경하지 않는다.
- 같은 handle 동시 호출 thread-safe 계약은 약화하지 않는다.

## 판정 기준

- build와 관련 unit/integration test가 먼저 통과해야 한다.
- targeted 64B perf에서 failure 0을 유지해야 한다.
- 반복 가능한 10% 이상 개선이 없거나 transport별 결과가 혼재하면 source 변경을 되돌린다.

## 적용 내용

임시 실험으로 `msg_t::content_t`에 private `capacity` 필드를 추가하고,
`msg_t::init_size()`가 만드는 소유 LMSG storage에 thread-local pool을 붙였다.
LMSG 원본을 view가 마지막으로 해제하는 경로도 같은 release helper를 타도록 바꾸어
direct `free()` 경로가 남지 않게 했다.

이 변경은 공개 `zlink_msg_t` 크기를 바꾸지 않았지만, `content_t` 크기를 바꾸므로
decoder shared allocator와 message view 테스트를 함께 확인했다.

## 검증

- `cmake --build core/build`
  - 통과
- `ctest --test-dir core/build --output-on-failure -R 'unittest_msg_view|unittest_zmp_decoder|test_public_inproc_multipart_send|test_pubsub$|test_transport_matrix|test_backpressure_(oneway_)?matrix|test_spot_pubsub_scenario|unittest_spot_data_plane_'`
  - 24/24 통과
- `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5 --results-tag round10_lmsg_pool`
  - 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_165636_round10_lmsg_pool.txt`
  - success 12, fail 0

## 64B targeted 비교

비교 기준은 zero-fail 전체 기준
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`이다.

| 패턴 | 전송 | 변화 |
|------|------|------|
| `MULTI_DEALER_DEALER` | tcp | -1.44% |
| `MULTI_DEALER_DEALER` | tls | -1.05% |
| `MULTI_DEALER_DEALER` | ws | -0.16% |
| `MULTI_DEALER_DEALER` | wss | -0.86% |
| `MULTI_PUBSUB` | tcp | +4.38% |
| `MULTI_PUBSUB` | tls | -2.80% |
| `MULTI_PUBSUB` | ws | -9.28% |
| `MULTI_PUBSUB` | wss | -2.07% |
| `MULTI_SPOT` | tcp | -6.90% |
| `MULTI_SPOT` | tls | -19.14% |
| `MULTI_SPOT` | ws | -19.60% |
| `MULTI_SPOT` | wss | +0.17% |

## 판정

소형 LMSG storage pool은 failure 0을 유지했지만, 10% 이상 개선을 만들지 못했고
transport별 결과가 혼재했다. 특히 `MULTI_SPOT tls/ws`는 큰 하락을 보였다.
따라서 source 변경은 되돌렸다.

복원 후 `cmake --build core/build`를 다시 실행해 `core/build` runtime을 현재 source와
맞췄다.

## 결론

- H1은 기각한다. 64B one-way 하락의 주 병목은 단순 LMSG malloc/free가 아니다.
- 다음 후보는 pipe/transport/lifecycle 경로에서 pattern별로 갈라지는 비용이다.
- DEALER public API sync 제거와 auto-HWM counter 제거는 thread-safe 계약과 monitor
  관측값을 약화하므로 실험하지 않았다.
