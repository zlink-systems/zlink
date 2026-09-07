# G-11b-3 with_stream idle 비교 측정

## 작업 트리 상태

측정 전후 `main` 브랜치였다. `git status --short`에서 감독관의 미커밋
`core/doc/spec/**` 8개 수정과 G-11b-3의 staged runtime 파일 3개를 확인했으며, 둘 다
수정하지 않았다. `git diff --cached --stat`은 다음과 같았다.

| staged 파일 | 변경 |
|---|---:|
| `core/src/runtime/core/pipe.cpp` | 329 insertions, 157 deletions |
| `core/src/runtime/core/pipe.hpp` | 47 insertions, 16 deletions |
| `core/src/runtime/sockets/common/socket_base_monitor.cpp` | 11 insertions, 4 deletions |
| 합계 | 387 insertions, 177 deletions |

기존의 다른 worklog/progress 미추적 파일도 유지했다. 이 job이 만든 파일은 이 요약과
`progress-measure-g11b3.md`뿐이며, 소스·스펙·staged patch·커밋·stash는 건드리지 않았다.

## 사전 조건과 Release library

`pgrep -x ninja`는 비어 있었고, 최초 idle 확인의 1분 load는 0.18 → 0.37이었다.
`JOBS=4 scripts/build-core.sh release --lib-only`를 실행해 `core/build`의 Release+LTO
`libzlink.so.0.17.1`을 18:03:59에 만들었다. 빌드 직후 load는 2.86까지 올라갔으나,
측정 전에 0.69 → 0.35 → 0.08로 2분 이상 1.0 미만을 유지한 것을 확인했다.

with_stream 실행 명령은 다음과 같다.

```bash
ZLINK_CORE_SOURCE=local \
RESULT_DIR=bindings/c/bench/with_stream/results/G-11b3-after-measure-20260907_180800 \
flock /tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/PERF_LOCK \
  ./bindings/c/bench/with_stream/run_benchmarks.sh \
  --stack zlink,asio,zmq --size all --ccu 1000 --runs 3 --reuse-build
```

README의 `ZLINK_CORE_SOURCE=local` 경로를 따라 실제 runtime은
`core/build/lib/libzlink.so.0.17.1`이었다. 시작 load average(18:08:00)는
**0.08 / 0.54 / 0.82**, runner 완료(18:13:21) 뒤 첫 관측(18:14:01)은
**1.31 / 1.78 / 1.36**이었다.

결과 경로: `bindings/c/bench/with_stream/results/G-11b3-after-measure-20260907_180800/`
(raw `metrics.csv`, `summary.json`, `comparison.md`). 27개 실제 case 모두 PASS,
mismatch 0, skip 없음이다.

## with_stream 3회 중앙값

| size | zlink kops | asio kops | zmq kops | zlink/asio | zlink/zmq | pristine zlink/asio | 비율 변화 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 298.477 | 366.680 | 330.035 | 0.813998 | 0.904380 | 0.812322 | +0.206% |
| 1024 B | 277.500 | 339.459 | 307.829 | 0.817478 | 0.901476 | 0.813308 | +0.513% |
| 65536 B | 33.934 | 41.388 | 27.738 | 0.819894 | 1.223344 | 0.819557 | +0.041% |

pristine raw directory로 지정된
`bindings/c/bench/with_stream/results/G-11b3-pristine-dfe6ec-runs3/`는 현재
worktree에 존재하지 않았다. 따라서 pristine 값은 기존
`doc/plan/c016-worklog/core-rf-G-11b3-summary.md`에 기록된 동일 3회 중앙값
(각각 0.812322 / 0.813308 / 0.819557)을 사용했다.

계획 §7.1의 최근 idle 참고 행은 다음과 같다.

| size | recent idle zlink kops | recent idle zlink/zmq |
|---:|---:|---:|
| 64 B | 289.7 | 0.91 |
| 1024 B | 267.8 | 0.98 |
| 65536 B | 32.6 | 1.27 |

**판정: 예. pristine 대비 zlink/asio 비율 변화가 세 size 모두 −5 % 이내이다.**

## perf/c 1024 B tcp 경량 3셀 (1회, 비교 기준 없음)

with_stream 완료 뒤 동일 `PERF_LOCK` 아래 다음을 실행했다.

```bash
./bindings/c/perf/run_benchmarks.sh \
  --pattern ROUTER_ROUTER --transports tcp --msg-sizes 1024 --runs 1 --reuse-build
./bindings/c/perf/run_benchmarks_multi.sh \
  --pattern ROUTER_ROUTER_SENDSEND,ROUTER_ROUTER_REQREP \
  --transports tcp --msg-sizes 1024 --runs 1 --reuse-build
```

모두 local `core/build/lib/libzlink.so.0.17.1`, Release, core dirty 상태(staged G-11b-3
patch)로 실행했고 성공했다. multi 시작 시 runner가 기록한 load average는
0.95 / 1.64 / 1.32였다.

| 셀 | 처리량 | mean latency | 결과 파일 |
|---|---:|---:|---|
| single ROUTER_ROUTER | 868.030 Kmsg/s | 0.039932 ms | `bindings/c/perf/results/single/report/perf_c_single_linux_20260907_181425.txt` |
| multi ROUTER_ROUTER_SENDSEND | 270.161 Kops/s | 0.636771 ms | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_181431.txt` |
| multi ROUTER_ROUTER_REQREP | 196.631 Kops/s | 0.669272 ms | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_181431.txt` |

비교 기준 없이 기록만 수행했으며, 세 셀 모두 success/complete다.
