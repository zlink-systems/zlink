# Round 14: PUBSUB/SPOT tcp 64B 미달 항목 추적

- goal: PUBSUB/SPOT tcp 64B one-way 미달 원인을 core hot path에서 확인한다.
- 완료 기준: `PUBSUB,SPOT tcp 64B --runs 2` median이 현재 문제 report 대비 +10% 이상,
  관련 core test 통과, source 변경이 실제 core runtime hot path에 남을 수 있음.
- 기준 commit: `5e3c438a2`
- 시작 git status: round 9-13 로그만 미추적. core source diff 없음.

## 기준 report

- 과거 기준: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 현재 문제: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- zero-fail full 기준: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 반복 median 기준:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171324_round12_repeat_current.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171942_round13_dealer_stream_64_repeat_current.txt`

## 현재 목표 미달 상태

반복 median을 우선 사용하고, 반복 측정이 없는 항목은 zero-fail full 기준을 사용했다.

- 전체 64B 공통 항목: 평균 +2.41%, 중앙값 +1.80%
- one-way 64B: 평균 +0.68%, 중앙값 -0.14%
- echo 64B: 평균 +6.30%, 중앙값 +4.98%

목표인 전체 64B 중앙값 +10%, 평균 +8%, one-way 평균 +10%에 미달한다.

문제 report 대비 가장 큰 미달 항목은 아래와 같다.

| 패턴 | 전송 | 현재 반복 median | 문제 report | 변화 |
|------|------|------------------|-------------|------|
| `MULTI_PUBSUB` | tcp | 2,351,928.3 msg/s | 2,628,104.8 msg/s | -10.51% |
| `MULTI_SPOT` | tcp | 3,592,126.7 msg/s | 3,896,078.6 msg/s | -7.80% |

## 가설

1. PUB fanout의 pipe write/flush/wakeup 경로가 tcp one-way에서 병목이다.
   `PUBSUB`와 side-handle `SPOT` 모두 최종적으로 PUB/XPUB distributor 경로를 지난다.
2. subscription/ready-state 유지 비용이 steady-state send path에 남아 있다.
   특히 `xpub_t::compute_delivery_ready_count()`는 mtrie 전체 방문을 사용하지만, send hot path에서
   반복 호출되는지 확인해야 한다.
3. 측정 variance가 커서 문제 report의 tcp 수치가 현재 반복 median보다 높은 단일-run 값일 수 있다.
   source 변경 전 `PUBSUB,SPOT tcp --runs 2`를 다시 실행해 현재 재현성을 확인한다.

## 먼저 검증할 가설

가설 3을 먼저 검증한다. round 12는 네 전송을 모두 돌렸기 때문에 전송 간 cooldown과 순서 영향이 있다.
이번 round에서는 tcp만 `PUBSUB,SPOT`으로 좁혀 `--runs 2` median을 다시 확인한다.

## 읽은 코드

- `core/src/runtime/sockets/pubsub/pub.cpp`
  - `PUB`는 `xpub_t`를 상속하고 attach 시 pipe nodelay를 설정한다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`
  - subscription 처리, ready count 갱신, distributor activation을 담당한다.
- `core/src/runtime/sockets/internal/dist.cpp`
  - matching pipe fanout과 pipe write/flush를 담당한다.
- `core/src/runtime/services/spot/pubsub/spot_pub.cpp`
  - side-handle SPOT publish는 socket이 있으면 `logical_multipart_publish()` direct path를 사용한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음
- 보안 의미를 유지한 근거: source 변경 전 측정/추적 단계다.
- 추가로 실행한 회귀 테스트: 아직 없음

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT \
  --transports tcp \
  --duration 5 \
  --runs 2 \
  --results-tag round14_pubspot_tcp_repeat_current
```

- 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_172407_round14_pubspot_tcp_repeat_current.txt`
- success 2, fail 0
- runner 출력 runtime: `core/build/lib/libzlink.so.6.0.4`

## 결과

| 패턴 | 전송 | 문제 report | round 12 median | round 14 tcp-only median |
|------|------|-------------|-----------------|--------------------------|
| `MULTI_PUBSUB` | tcp | 2,628,104.8 msg/s | 2,351,928.3 msg/s | 2,664,617.4 msg/s |
| `MULTI_SPOT` | tcp | 3,896,078.6 msg/s | 3,592,126.7 msg/s | 3,871,779.6 msg/s |

round 14 tcp-only median 기준으로 `MULTI_PUBSUB tcp`는 문제 report 대비 +1.39%,
`MULTI_SPOT tcp`는 -0.62%다. 따라서 round 12의 tcp 하락은 source 회귀로 확정할 수 없고,
multi-transport 실행 순서 또는 측정 variance 영향으로 분리한다.

## 재산정

round 14 결과를 현재 반복 기준에 반영하면 다음과 같다.

- 전체 64B 공통 항목: 평균 +3.14%, 중앙값 +1.80%
- one-way 64B: 평균 +1.74%, 중앙값 +1.16%
- echo 64B: 평균 +6.30%, 중앙값 +4.98%

목표 수치에는 여전히 미달하지만, 문제 report 대비 -5%를 넘는 현재 반복 항목은 없다.

## 판정

- PUBSUB/SPOT tcp는 이번 라운드의 core 수정 대상으로 삼지 않는다.
- source 변경 없음.
- 다음 후보는 특정 pattern 회귀가 아니라 모든 public send가 매 메시지마다 호출하는
  `socket_base_t::send_direct_with_retry()`의 공통 command polling 비용이다.
