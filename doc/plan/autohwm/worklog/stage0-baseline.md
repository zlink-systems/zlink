# Stage 0 baseline evidence (2026-08-22)

이 문서는 `core-byte-hwm-flow-control-plan.ko.md` §12.1 (시작과 기준점) 실행 증거다.
측정값과 실행 조건만 기록하고 구현 판단은 포함하지 않는다.

## 1. Branch와 dirty worktree

```text
git branch --show-current        -> codex/bindings-0.11.1-performance
git status --short | wc -l       -> 59
git tag -l 'backup/autohwm-worktree-20260822' -> backup/autohwm-worktree-20260822 (존재 확인)
```

`git diff --stat`(HEAD 대비, 54 files changed, 1488 insertions(+), 767 deletions(-))
요약(발췌, 전체는 `git diff --stat` 재실행으로 확인):

- `core/src/runtime/core/pipe.cpp` (+347/-)、`pipe.hpp` (+16/-)
- `core/src/runtime/core/ctx_physical_queue_registry.cpp` (680줄 diff, 대폭 축소: -473/+252)
- `core/src/runtime/core/ctx_auto_hwm_recalc.cpp` (+88)
- `core/src/runtime/protocol/decoder.hpp` (zero-copy fast path 17줄 제거)
- `core/src/runtime/sockets/router/router_recv_path.cpp` (+96/-)
- 나머지는 perf runner 스크립트, doc mirror, 신규 test 파일.

## 2. Symbol inventory (plan §1.3)

`rg`로 확인한 주요 symbol 위치 (worktree 기준):

| Symbol | 위치 |
|---|---|
| `frame_accounted_bytes` | `core/src/runtime/core/pipe.hpp:344-345`, 정의 `pipe.cpp:1990`, 호출부 `pipe.cpp:713,730,750,779,1392,1552,1808,2047,2166,2237` |
| `check_hwm_for_message` | 선언 `pipe.hpp:276`, 정의 `pipe.cpp:1793`, 호출 `core/src/runtime/sockets/internal/dist.cpp:253` |
| `account_inbound_frame` | 선언 `pipe.hpp:360`, 정의 `pipe.cpp:2235`, 호출 `pipe.cpp:715,740,755,782` |
| `compute_lwm` | 선언 `pipe.hpp:506`, 정의 `pipe.cpp:1660`, 호출 `pipe.cpp:202,540,552,1768,1781` |
| `apply_lwm_hint` | 선언 `pipe.hpp:507`, 정의 `pipe.cpp:1681`, 호출 `pipe.cpp:540,552,1767,1780` |
| `_bytes_written` | 선언 `pipe.hpp:412`, getter `pipe.cpp:422-425`, 사용 `pipe.cpp:463,806,823-832,944-998,1826-1854` 외 |
| `_peers_bytes_read` | 선언 `pipe.hpp:432`, 사용 `pipe.cpp:224,456,580-581,806-998,1334,1433,1826-1854` |
| `budget_insufficient` | 선언 `core/src/runtime/core/auto_hwm_policy.hpp:68`, 설정 `auto_hwm_policy.cpp:143,301,390,464`, `ctx_physical_queue_registry.cpp:1223`, 소비 `ctx_auto_hwm_state.cpp:310`, unit test `core/tests/unittest/unittest_auto_hwm_policy.cpp:88,149` |
| `transport_lane_application` | enum `core/src/runtime/core/options.hpp` 계열, 사용처 `socket_base_endpoint.cpp:182-390`, `socket_base_api.cpp:259-969`, `router_admission.cpp:100-122`, `socket_runtime.hpp:44`, `zmp_metadata.hpp:150`, `asio_ws_engine.cpp:846,1430` |
| `transport_lane_completion` | enum `core/src/runtime/core/options.hpp:40`, 사용처 `socket_base.cpp:188-282`, `socket_base_api.cpp:138-757`, `socket_base_dispatch.cpp:307`, `socket_base_endpoint.cpp:183-334`, `socket_monitor_runtime.cpp:160`, `ctx_inproc_registry.cpp:235`, `session_base.cpp:159`, test `test_router_multiple_dealers.cpp:828-830` |
| auto_hwm 정책 파일 | `core/src/runtime/core/auto_hwm_policy.hpp`, `.cpp` (확인) |
| ctx 재계산/snapshot | `core/src/runtime/core/ctx_auto_hwm_recalc.cpp`, `ctx_auto_hwm_state.*` (확인) |
| physical queue registry | `core/src/runtime/core/ctx_physical_queue_registry.hpp`, `.cpp` (확인, dirty에서 대폭 축소됨) |
| decoder admission | `core/src/runtime/core/session_base_pipe_io.cpp`, `core/src/runtime/protocol/zmp_decoder.hpp/.cpp` (둘 다 존재), `core/src/runtime/engine/asio/asio_zmp_engine.cpp` (확인) |

### 2.1 HEAD vs dirty worktree 존재 여부

`git show HEAD:<file> | grep -c <symbol>`로 확인한 결과, `frame_accounted_bytes`,
`check_hwm_for_message`, `account_inbound_frame`, `compute_lwm`, `apply_lwm_hint`,
`budget_insufficient` 모두 **HEAD의 `pipe.cpp`/`auto_hwm_policy.cpp`에도 이미 존재**한다.
즉 pipe-local byte accounting 골격은 HEAD(마지막 commit)에 이미 있고, 현재 dirty
worktree는 그 위에 세부 구현을 추가/수정한 상태다 (`pipe.cpp`/`pipe.hpp`는 가장 최근에
수정됨, mtime 03:50 vs 다른 파일들 03:11).

`ctx_physical_queue_registry.cpp`는 HEAD 1840줄 → worktree 1618줄로 대폭 축소되어
있어(-473/+252), registry 쪽 diff가 이번 stage 이후 "제거 기준점" 판단 시 가장 큰
검토 대상이다. `decoder.hpp`는 zero-copy fast-path 17줄이 dirty에서 제거되어 있다
(diff 위 참고).

`zmp_decoder.hpp`/`.cpp`는 HEAD에도 이미 존재한다(별도 신규 파일 아님).

## 3. Build

```text
$ cmake --build core/build --parallel 8
ninja: no work to do.
```

빌드는 이미 최신 상태였다(직전 세션에서 build됨). 에러 없음.

## 4. Runtime provenance

```text
core/build/lib/libzlink.so -> libzlink.so.0 -> libzlink.so.0.11.1
libzlink.so.0.11.1 mtime epoch 1787338462 (2026-08-22 03:54:22 +0900)
```

가장 최근에 수정된 core/src 파일: `pipe.cpp`/`pipe.hpp` (mtime epoch 1787338213,
2026-08-22 03:50:13 +0900). `.so` mtime(03:54:22)이 소스 mtime(03:50:13)보다
최신이므로 runtime이 source보다 오래되지 않았음을 확인했다.

## 5. Focused test 8개 (plan §8.1)

```text
$ ctest --test-dir core/build --output-on-failure \
  -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options|test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm)$'
```

| Test | 결과 | 시간 |
|---|---|---|
| test_ctx_options | Passed | 0.34s |
| test_retained_hwm_credit | Passed | 0.64s |
| test_connect_rid | Passed | 2.73s |
| test_router_handover | Passed | 13.08s |
| test_zmp_request_reply | Passed | 38.04s |
| test_router_mandatory_hwm | Passed | 2.04s |
| unittest_zmp_decoder | Passed | 0.00s |
| unittest_auto_hwm_policy | Passed | 0.00s |

**8/8 통과.** Total test time 56.89s.

## 6. Noise floor: 0.10.1 vs 0.10.1 (3회, plan §8.2.0)

Command (a/b/c 태그로 3회):

```text
./bindings/c/perf/run_benchmarks_multi.sh --core-version 0.10.1 \
  --pattern ROUTER_ROUTER_SENDSEND --transports tcp --msg-sizes 256 --runs 1 \
  --results-tag autohwm-noise-0101-{a,b,c}
```

Report 경로:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060544_autohwm-noise-0101-a.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060559_autohwm-noise-0101-b.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060609_autohwm-noise-0101-c.txt`

측정값:

| Run | Throughput (Kops/s) | Bandwidth (MB/s) | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) |
|---|---|---|---|---|---|
| a | 193.386 | 99.013 | 0.242 | 0.373 | 0.464 |
| b | 182.079 | 93.224 | 0.276 | 0.401 | 0.563 |
| c | 179.504 | 91.906 | 0.259 | 0.402 | 0.509 |

최대 상대 spread (max-min)/min:

| Metric | Spread |
|---|---|
| Throughput | 7.73% |
| Bandwidth | 7.73% |
| Lat.Mean | 14.05% |
| Lat.P95 | 7.77% |
| Lat.P99 | 21.34% |

호스트 분산이 특히 P99 latency에서 21%까지 나타난다. 이는 판정 근거가 아니라
host 안정성 참고용이며(plan §8.2.0), 이후 local vs 0.10.1 비교에서 큰 폭(약 45%
이상)의 차이만 회귀로 판단한다.

## 7. 첫 paired baseline: local vs 0.10.1 (plan §8.2.1)

Local/release를 local, release, local, release, local, release 순으로 각 3회 실행.

Command:

```text
ZLINK_CORE_SOURCE=local ./bindings/c/perf/run_benchmarks_multi.sh \
  --pattern ROUTER_ROUTER_SENDSEND --transports tcp --msg-sizes 256 --runs 1 \
  --results-tag autohwm-rr-tcp-256-local

./bindings/c/perf/run_benchmarks_multi.sh --core-version 0.10.1 \
  --pattern ROUTER_ROUTER_SENDSEND --transports tcp --msg-sizes 256 --runs 1 \
  --results-tag autohwm-rr-tcp-256-release-0101
```

Report 경로 (실행 순서대로):

1. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060624_autohwm-rr-tcp-256-local.txt`
2. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060634_autohwm-rr-tcp-256-release-0101.txt`
3. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060650_autohwm-rr-tcp-256-local.txt`
4. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060700_autohwm-rr-tcp-256-release-0101.txt`
5. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060712_autohwm-rr-tcp-256-local.txt`
6. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_060723_autohwm-rr-tcp-256-release-0101.txt`

### 7.1 Provenance 확인 (각 report의 META 라인)

Local (3개 report 모두 동일):

```text
META,core_source,local
META,core_version,0.11.1
META,core_runtime,/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.11.1
META,core_revision,16f896c532e3742717869d1c7019c6237791a968
META,core_dirty,1
```

Release (3개 report 모두 동일):

```text
META,core_source,release
META,core_version,0.10.1
META,core_runtime,/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/lib/libzlink.so.0.10.1
META,core_revision,09d34089c0956b5ef3f308a976a3addec21bafc5
META,core_dirty,0
META,core_release_tag,core/v0.10.1
```

Pattern/transport/size: 모든 report에서 `MULTI_ROUTER_ROUTER_SENDSEND / tcp / 256`,
`clients=100`, `runs=1`로 동일.

Auto-HWM/OS buffer detail (client=router 행):

- Local: `MsgUnit(B)=?`, `SNDHWM=4194304`, `RCVHWM=4194304`, SNDBUF/RCVBUF 열 비어있음(auto).
- Release(0.10.1): `SNDHWM=4294967295`(=0xFFFFFFFF, count-HWM unlimited 표기), `RCVHWM=0`,
  `SNDBUF(KB)=4`, `RCVBUF(KB)=0`. 0.10.1은 byte 단위 Auto-HWM snapshot ABI가 없어
  plan §8.2.1이 명시한 대로 detail이 다르게 보고된다. 나머지 workload 옵션
  (clients=100, io-threads=4, duration=5s, auto-hwm profile=balanced)은 두 조건에서
  동일하다.

### 7.2 측정값

| Run | Version | Throughput (Kops/s) | Bandwidth (MB/s) | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) |
|---|---|---|---|---|---|---|
| 1 | local | 126.554 | 64.796 | 0.369 | 0.589 | 0.721 |
| 2 | release 0.10.1 | 186.803 | 95.643 | 0.250 | 0.384 | 0.487 |
| 3 | local | 131.142 | 67.144 | 0.356 | 0.562 | 0.690 |
| 4 | release 0.10.1 | 190.707 | 97.642 | 0.266 | 0.374 | 0.460 |
| 5 | local | 129.677 | 66.394 | 0.361 | 0.569 | 0.701 |
| 6 | release 0.10.1 | 187.935 | 96.223 | 0.249 | 0.384 | 0.477 |

### 7.3 Median 비교 (local vs 0.10.1)

| Metric | Local median | 0.10.1 median | Local/0.10.1 |
|---|---|---|---|
| Throughput (Kops/s) | 129.677 | 187.935 | 69.0% |
| Bandwidth (MB/s) | 66.394 | 96.223 | 69.0% |
| Lat.Mean (ms) | 0.361 | 0.250 | 144.4% (높을수록 나쁨) |
| Lat.P95 (ms) | 0.569 | 0.384 | 148.2% |
| Lat.P99 (ms) | 0.701 | 0.477 | 147.0% |

**판정 (plan §8.2.3 기준, 완화 없이 median 직접 비교):** local throughput·bandwidth
median이 0.10.1보다 낮고(약 31% 낮음), local mean·p95·p99 latency median이 0.10.1보다
높다(약 44~48% 높음). 이 case는 현재 미달이며 이는 **이미 알려진 첫 회귀(plan §2.2,
handoff 문서)와 일치**한다. 노이즈 플로어(§6, 최대 21%)를 감안해도 약 45~48%
격차는 host 분산으로 설명되지 않는 실제 회귀다. 이번 stage에서는 코드 수정을
하지 않고 회귀 존재만 재확인·기록한다.

## 8. 다음 stage로 넘어가는 조건 확인

Plan §7.1 "기준점" 행의 "다음 단계로 넘어가는 조건"은 "Public HWM option을
unlimited로 바꾸지 않고 알려진 perf case가 회복됨"이다. 이번 stage는 기준점
제거를 수행하지 않았으므로(§12.2 대상), 이 조건은 stage0 완료 조건이 아니다.
Stage0(§12.1)의 마지막 항목("첫 paired report 확보")은 위 §7로 충족했다.
