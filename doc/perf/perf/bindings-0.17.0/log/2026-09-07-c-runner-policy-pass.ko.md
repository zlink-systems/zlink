# C canonical perf 러너 정책 정합 pass — 2026-09-07

> 범위: `bindings/c/perf/**` 만. 정책 문서·계획서·`decisions.ko.md`·다른 binding·`framework/**`·`core/**` 무수정.
> 근거 문서: `doc/perf/PERF_POLICY.md` v2.2, `doc/perf/PERF_SINGLE_TEST_POLICY.md` v2.3,
> `doc/perf/PERF_MULTI_TEST_POLICY.md` v2.2 (2026-09-07 개정본),
> `log/2026-09-07-policy-amendment.ko.md` §5(R1~R20), `log/2026-09-07-runner-parity-design.ko.md` §5 단계 2.
> 결정: D-BP1(B2)·D-BP2·D-BP3·D-BP4.
> Core artifact 고정: `core/build/lib/libzlink.so.0.17.0`, Build ID `af759a1c5532fb7100c6baede89144814200d798` (실측 확인). Core 재빌드 없음, `ZLINK_CORE_SOURCE=local`.

---

## 1. 정책 조항별 수정 내역

### R20 / P2 — one-way 하드코딩 in-flight 상한 제거

- 정책: `PERF_POLICY.md:1277-1290`(§7.2 "하드코딩 flow control … 인위적 제한 없이 auto-HWM send
  admission backpressure를 사용한다"), `PERF_SINGLE_TEST_POLICY.md:128-133`("in-flight 메시지 수를
  코드로 관리하거나 상한으로 고정하지 않는다"),
  `BINDINGS_OPTIMIZATION_GUIDE.ko.md:111-112`(이미 기각된 후보).
- 수정: `bindings/c/perf/single/common/perf_single_one_way.hpp`
  - `send_active_samples()`에서 `max_in_flight_`·`received_count_` 파라미터와 `sent - received`
    gate 루프(`std::this_thread::yield()`) 삭제. 이제 deadline까지 연속 제출만 한다.
  - `latency_phase_max_in_flight()` 삭제.

### R19 / P2 — 별도 1초 latency 단계 제거, 같은 active 구간 집계

- 정책: `PERF_SINGLE_TEST_POLICY.md:37`("같은 active 구간에서 동일 메시지 집합으로 latency도 함께
  집계한다"), `:293`("throughput과 latency는 동일한 유효 메시지 집합을 사용한다"),
  `:206-210`(phase 표에 latency 단계 없음).
- 수정: `perf_single_one_way.hpp`
  - `run_measurement_phase()` + 2-phase `run_active_phase()` 구조를 **단일 `run_active_phase()`**
    로 통합. `capture_latency_` 분기와 `latency_phase_duration_seconds()` 삭제.
  - 수신 루프가 유효 메시지 하나마다 `active_received` 증가 **와** `latency.add()`를 함께 수행한다.
  - in-flight-1 전용이던 "drain이 EAGAIN에 닿은 뒤에 ack를 늦게 publish" 로직도 함께 삭제
    (그 로직의 존재 이유가 in-flight 1 gate였다).
  - reqrep 쪽 별도 latency 단계(`perf_single_reqrep.hpp:504-507`, `:537-548`)는 R1의 파일 삭제로
    함께 사라졌다. one-way 경로에 남은 latency 단계는 없다.
- 외부 계약(`run_active_phase` 시그니처)은 그대로라 5개 pattern 소스는 무수정.

### R4 / P3.1 — one-way active 유효 메시지 규칙(수신 deadline 필터)

- 정책: `PERF_SINGLE_TEST_POLICY.md:284-290` — "수신 시각이 active deadline 이전인 유효 header
  메시지 … 판정에 쓰는 시각은 recv 루프가 그 메시지를 처리한 monotonic 시각이며 `sent_ts_ns`가
  아니다 … stop token 도착 여부와 무관하게 집계에서 제외", `:170-174`(§1.4 drain 산문).
- 수정: `perf_single_one_way.hpp` `run_active_phase()`
  - active deadline을 `perf_single_metric::now_ns()`(steady_clock) 기준 `uint64_t`로 한 번 계산해
    sender/receiver 두 thread가 **같은 값**을 쓴다.
  - 수신 루프는 메시지마다 `recv_ts_ns = now_ns()`를 한 번 읽어
    `recv_ts_ns < active_deadline_ns` 일 때만 집계하고, 그 뒤에도 stop token까지 계속 소비한다
    (종료 정리).
- 지원 변경: `bindings/c/perf/single/common/perf_single_phase.hpp`
  - `single_latency_ns(header)`(내부에서 `now_ns()` 재호출) → `single_latency_ns_at(header,
    recv_ts_ns)`. 한 메시지의 deadline 판정과 latency가 **같은 timestamp 하나**를 쓴다.
  - 사용처가 없어진 `single_record_active_header()` 삭제(§7.6.6 죽은 코드).

### R7 / P3.2 — wire 길이 불일치는 집계 제외(fatal 아님)

- 정책: `PERF_SINGLE_TEST_POLICY.md:283-284` — "수신 프레임의 실제 byte 길이가 기대 payload
  크기와 다른 메시지는 집계에서 제외한다. 이 불일치는 실패가 아니라 집계 제외 사유이며 러너를
  중단시키지 않는다."
- 수정: 4곳에서 `recv_result_error`/`-1` 반환을 없애고 `header_ok=false`인 payload로 반환
  (= 집계 제외, 소비는 계속). debug 로그 문구도 `unexpected` → `excluded`.
  - `perf_single_one_way.hpp` `recv_single_part_header_flags()`
  - `single/src/perf_dealer_router.cpp` `recv_router_header_flags()`
  - `single/src/perf_router_router.cpp`
  - `single/src/perf_pubsub.cpp`

### R1 / R3 — Single REQREP 제거 (D-BP3)

- 정책: `PERF_SINGLE_TEST_POLICY.md:31-45`(§1 "Single suite는 one-way 5 pattern만 측정한다"),
  `:69-73`(§1.1 공식 패턴 5개), `:379-390`(§6.1 지원 패턴 + 제외 주석),
  `PERF_POLICY.md`(§1.3 binding single `--pattern ALL` 5개).
- 삭제한 파일:
  - `bindings/c/perf/single/common/perf_single_reqrep.hpp`
  - `bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp`
  - `bindings/c/perf/single/src/perf_router_router_reqrep.cpp`
- 등록 해제:
  - `bindings/c/perf/CMakeLists.txt` — `add_current_bench_single(perf_*_reqrep …)` 2행
  - `bindings/c/perf/run_benchmarks.sh:184` `STANDARD_PATTERNS` → 5개, target 매핑 2블록
  - `bindings/c/perf/run_benchmarks.ps1` `$SinglePatterns` → 5개, `$TargetMap` 2행
  - `bindings/c/perf/single/run_comparison.py` `DEFAULT_PATTERNS`·`PATTERN_TO_BINARY` → 5개
- §6.1 "패턴 방향 분류"(single에는 왕복 패턴이 없다)에 맞춰
  `pattern_direction_label()`은 항상 `one-way`, `format_throughput()`은 항상 `Kmsg/s`.
- `expected_result_lines`(§3.1)는 조합 수 × 5로 계산되므로 pattern 목록 축소만으로 자동 정합
  (R3). `--pattern ALL` = `['PAIR','PUBSUB','DEALER_DEALER','DEALER_ROUTER','ROUTER_ROUTER']` 확인.
- README·AGENTS.md에는 single REQREP 언급이 없었다(README의 REQREP은 전부 multi).
- 회귀 테스트 추가: `single/tests/test_run_comparison_policy.py`
  - `test_reqrep_patterns_removed_from_single_runner`
  - `test_single_throughput_unit_has_no_round_trip_variant`

### R10 / P4.2 — 가중 분위수를 누적 weight 축 선형 보간으로

- 정책: `PERF_POLICY.md:112-118` — weight = `유효 관측 수 / 보관 sample 수`,
  `c_i = Σ_{j<=i} w_j`, `W = Σ w_j`, `pos = (W-1)*q`, 위치 `p`의 sample은 `c_i - 1 >= p`를
  만족하는 가장 작은 `i`, 그 축에서 `:108-111`과 같은 선형 보간. 모든 weight가 1이면 항등.
- 수정: `bindings/c/perf/multi/common/perf_multi_weighted_latency.hpp`
  - 보간 없는 nearest-rank(`cumulative >= total_weight*q`) → `weighted_sample_at()` +
    `lo=floor(pos)`, `hi=min(lo+1, W-1)`, `frac=pos-lo` 선형 보간.
- 표본 0개 경계(`P4.1`, `PERF_POLICY.md:119-122`)는 `aggregate()`가 이미 p95=p99=mean이라 무수정.
- 회귀 테스트 추가: `multi/tests/test_perf_multi_metrics.cpp`
  `test_weighted_percentile_matches_unit_weight_interpolation` — weight 전부 1일 때
  `pos=(n-1)q` 단일 경로 값과 정확히 일치(9.55 / 9.91). 기존
  `test_weighted_child_aggregation`(2.0 / 2.0)도 그대로 통과.

### R12 — `default_sample_cap()` 음수 입력

- `PERF_SINGLE_LATENCY_SAMPLE_CAP=-1`이 `strtoull` wrap으로 거대한 cap이 되던 단순 버그.
- 수정: `single/common/perf_single_latency.hpp` — `'-'` 선검사 + `errno`/`end==value`/
  `size_t` 범위 검사(삭제한 `perf_single_reqrep.hpp:93-107`의 검증과 동일한 규칙을 흡수).

### 시간원(monotonic) — 위반 없음, 확인만

- 정책: `PERF_POLICY.md:127-140`, `:346`, `:352-353`.
- `grep -E "chrono::[a-z_]+clock"` 전수: `bindings/c/perf/**`에 `steady_clock` 외의 clock이 없다.
  `system_clock`/`gettimeofday`/`clock_gettime`/`CLOCK_REALTIME` 사용처 0건.
  `sent_ts_ns`(`perf_single_metric_header.hpp:36-43`, `perf_multi_metric_header.hpp:40-46`),
  active deadline, stop-token retry deadline, drain 한도가 모두 `steady_clock`.
  이번 변경으로 수신 판정 시각도 같은 시간원으로 명시화됐다.

### RESULT 출력 정밀도 — 위반 없음, 확인만

- 정책: `PERF_POLICY.md:1094-1100`(throughput·bandwidth 소수 3자리, latency 3종 소수 6자리 고정).
- `perf_single_monitor.hpp:173-183`, `perf_multi_metrics.hpp:226-236` 모두
  `std::fixed` + `setprecision(3)/(6)`. 무수정.

### transient 재시도 — 위반 없음, 확인만

- 정책: `PERF_SINGLE_TEST_POLICY.md:113-121`(1 ms 대기 + 새 `sent_ts_ns` 재stamp, busy retry 금지),
  `PERF_POLICY.md:1273`(§7.1 표).
- `send_active_samples()`는 `send_step_retry`에서 `sleep_for(1ms)` 후 같은 `seq`로 재호출하고,
  `init_active_payload_part()`가 매 호출 `now_ns()`로 재stamp한다(`perf_single_one_way.hpp:152-155`).
  통합 후에도 유지. 무수정.

### `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` (R17) — 위반 없음

- `multi/common/perf_multi_client_helpers.hpp:38` 기본값 5000 = `PERF_MULTI_TEST_POLICY.md:1265`.

### multi REQREP client의 size 순회 (`PERF_MULTI_TEST_POLICY.md:594`) — 위반 아님

- 정책 §3.4는 "각 size 케이스는 반드시 독립된 server/client 프로세스 쌍으로 실행한다.
  **runner는** size마다 server/client 바이너리를 다시 실행해야 한다"이며, 대상은 runner다.
- 실제 실행 경로: `run_comparison.py` `run_sizes_test()`(`:2846-2930`)가 size 목록을 1개씩 잘라
  `run_sizes_test_split(..., [case_size], ...)`로 프로세스 쌍을 매번 새로 띄운다.
  `_prepare_case_env()`(`:1571-1572`)가 `PERF_MSG_SIZES`를 그 한 size로 설정하므로
  `perf_multi_socket_reqrep.hpp:769-793`의 loop는 **항상 1회만** 돈다.
- 이 loop 구조는 REQREP 전용이 아니라 C multi client 6종 전부(`resolve_case_msg_sizes` 사용처)의
  공통 골격이다. runner가 정책을 지키고 있으므로 위반이 아니라고 판정하고 무수정.
  → 감독자 판단 항목 §5-(2)에 남긴다.

### (부수) 사전 존재하던 빌드 파손 수정

- `bindings/c/perf/multi/src/perf_multi_router_router_matched_client.cpp:164-166`이
  `run_active_window()`의 `websocket_transport` 인자(커밋 `21746768ca`에서 추가)를 넘기지 않아
  **컴파일이 되지 않았다**(내 변경 이전부터). matched client는 R10이 바꾼 가중 분위수 경로의
  유일한 사용처라 빌드 없이는 검증이 불가능하므로 최소 수정했다:
  `run_reqrep_case()`에 `websocket_transport` 파라미터를 추가하고 호출부에서
  `transport == "ws" || transport == "wss"`를 넘긴다.

---

## 2. 검증

### 자동 테스트

- `python3 -m unittest discover -s bindings/c/perf/single/tests` → **49 tests OK**
  (기존 45 + 신규 3 + 아래 hermetic 수정 반영).
- `bindings/c/build/perf/perf_multi_metrics_test` → 통과(신규 weighted percentile 테스트 포함).
- `bash -n run_benchmarks.sh`, `py_compile` 통과.

### smoke (전부 직렬 실행, 동시 perf 프로세스 0)

Core `ZLINK_CORE_SOURCE=local`, `PERF_FAIL_FAST=1`, tcp, 1024 B, duration 1, runs 1.

| suite | pattern | status | report |
|---|---|---|---|
| single | PAIR, DEALER_ROUTER | complete (10/10) | `bindings/c/perf/results/single/report/perf_c_single_linux_20260907_100129_smoke_policy_pass.txt` |
| single | PUBSUB, ROUTER_ROUTER, DEALER_DEALER | complete (15/15) | `bindings/c/perf/results/single/report/perf_c_single_linux_20260907_100219_smoke_policy_pass2.txt` |
| multi | DEALER_DEALER | complete (5/5) | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_100207_smoke_policy_pass.txt` |
| multi | ROUTER_ROUTER_REQREP | complete (5/5) | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_100213_smoke_policy_pass_reqrep.txt` |

single 5 pattern 전부 5 metric 0 없음. `--pattern ALL` = 5 pattern.

관찰(정보): B2 적용으로 single one-way latency가 **포화 상태 편도 지연**으로 바뀌어 크게 올랐다
(1024 B tcp: PAIR 0.726 ms, DEALER_DEALER 0.708 ms, DEALER_ROUTER 0.716 ms, PUBSUB 0.471 ms,
**ROUTER_ROUTER 19.10 ms**). throughput은 이전과 같은 수준(0.77~1.08 Mmsg/s). 설계 문서 D4가
예고한 변화이며, ROUTER_ROUTER만 한 자릿수 큰 것은 별도 확인 대상으로 §5에 남긴다.

---

## 3. 기존 실패 테스트 4건의 원인과 처리

`test_multi_run_comparison_policy.py`의 4건
(`test_multi_split_runner_isolates_each_size_case`,
`test_multi_sizes_run_as_isolated_cases_without_transition_sleep`,
`test_multi_size_case_does_not_retry_before_failing`,
`test_multi_size_failure_continues_without_merging_failed_metrics`).

**원인: 정책 위반도, 개정 전 정책을 검사하는 테스트도 아니다. 빌드 상태 의존이다.**
`run_comparison.py:2896-2906`의 `run_sizes_test()`는 delegate 전에
`os.path.exists(BUILD_DIR/<server>)`, `.../<client>`를 확인하고 없으면
`reason="missing_split_binaries"`로 실패한다. 4건은 `RC.run_sizes_test_split`만 stub할 뿐
이 존재 검사를 stub하지 않으므로, `bindings/c/build/perf`에 해당 바이너리가 빌드돼 있어야만
통과한다. 실제로 작업 시작 시 그 디렉터리에는 `comp_src_dealer_dealer_*`,
`comp_src_dealer_router_sendsend_*` 4개뿐이었고 `comp_src_pubsub_*`가 없었다.

처리: (a) 이번 검증을 위해 C perf 전체 타깃을 빌드했고, (b) 테스트가 다시 빌드 상태에 좌우되지
않도록 `split_binaries_present()` 컨텍스트 매니저를 테스트 모듈에 추가해 `BUILD_DIR` 하위 경로에
대해서만 `os.path.exists`를 True로 만든 뒤 4건의 `run_sizes_test()` 호출을 감쌌다.
프로덕션 코드는 건드리지 않았다. 바이너리를 지운 상태로도 49건 전부 통과함을 확인했다.

---

## 4. 고치지 않고 남긴 항목

| 항목 | 이유 |
|---|---|
| multi client의 프로세스 내 size loop | §1 마지막 항목 참조 — runner가 size마다 프로세스 쌍을 새로 띄우므로 정책 §3.4를 충족한다. C multi client 6종 공통 골격이라 REQREP만 떼어내면 오히려 비대칭이 된다 |
| R2·R5·R6·R8·R9·R11·R13~R18 | 다른 binding 담당분. 범위 밖 |
| `bindings/c/perf/baseline/perf_c_single_linux_*.txt`의 REQREP 행 | 과거 기록이므로 보존(D-BP4 "완결된 판정을 소급 무효화하지 않는다"). baseline 갱신은 새 full matrix 측정 시 runner가 수행한다 |
| Single 기준값 재측정 | 감독자 판단 사항(policy-amendment §6-1). 이번에는 smoke만 했다 |

---

## 5. 감독자 판단이 필요한 항목

1. **Single 기준값 재측정 범위.** R4(deadline 필터)와 R19(latency 단계 제거)로 C single latency의
   의미가 바뀌었다(무부하 편도 → 포화 편도). 기존 Single 표를 어디까지 보존하고 어디부터
   재측정할지 결정 필요(policy-amendment §6-1과 같은 항목).
2. **multi client의 size loop 유지 여부.** §1 마지막 항목의 판정(위반 아님)을 승인할지,
   아니면 "바이너리가 여러 size를 순회할 수 있는 코드 자체를 제거"까지 요구할지.
   후자라면 C multi client 6종 + `resolve_case_msg_sizes`/`resolve_case_max_msg_size`를 함께
   손대야 하므로 별도 pass가 필요하다.
3. **single ROUTER_ROUTER의 19 ms latency.** 같은 조건의 다른 one-way pattern이 0.47~0.73 ms인데
   ROUTER_ROUTER만 한 자릿수 크다. 포화 큐 깊이 차이일 수도, ROUTER→ROUTER 송신 경로의 admission
   특성일 수도 있다. 러너 정책 문제로는 보이지 않아 이번 pass에서는 손대지 않았다.
4. **matched client 빌드 파손 수정을 이 pass에 포함시킬지.** 사전 파손이라 별도 커밋으로 떼어내는
   편이 이력상 깔끔할 수 있다.
5. **테스트 hermetic 수정.** 프로덕션 코드는 무수정이지만 테스트 파일을 손댔다. 대안은
   "이 4건은 빌드 후에만 실행한다"를 규칙으로 남기는 것.
