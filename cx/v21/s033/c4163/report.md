# #4163 Core bisect 측정 결과

## 측정

- B: Core 1.23, `79567d379162e67476ba13e18cfe0f69925e63c9` (`prefix-1.23`).
- M: Core `9dc2246dd07935daa138eb7bb1b6c4d19317c778` (`prefix-mid`).
- C: Core 1.24 후보, `0005a4d6c07160bbf661a6f7aea3d8093880514e` (`prefix-1.24`).
- 단일 `ROUTER_ROUTER`, `inproc`, 256B·1024B, 각 stage `--runs 5`; 순서 B→M→C, C→M→B. CPU 8–15 고정, `measure.lock` 아래 실행.
- M runtime SHA-256은 `18bd2797edb71eaead4b79c0192c21df1d50867d8f1c221aeec2cb89bb4b96ae`; B/C runtime SHA와 달랐고 harness RUNPATH 및 `ldd`가 모두 `prefix-mid/lib`를 가리켰다.
- M r1의 첫 시도는 release provenance metadata 누락으로 benchmark 시작 전에 중단됐다. 측정은 하지 않았고 로그를 보존했다: [실패 사전조건 로그](./runs/M-r1-single-ROUTER_ROUTER-inproc.log). 수정된 prefix metadata와 runner 호환 harness로 M r1을 다시 측정했다.

아래 각 배열은 10개 측정값이며, M 순서는 r1 재측정 5회 다음 r2 5회다. 값 단위는 ms다. 요약은 `중앙값 / 최솟값–최댓값`이다. per-run 값은 아래 stage 로그에 있다.

## 평균 지연과 p95


### 1024B

Raw paths: [B r1 log](./runs/B-r1-single-ROUTER_ROUTER-inproc.log), [B r2 log](./runs/B-r2-single-ROUTER_ROUTER-inproc.log) · [M r1 retry log](./runs/M-r1-retry-single-ROUTER_ROUTER-inproc.log), [M r2 log](./runs/M-r2-single-ROUTER_ROUTER-inproc.log) · [C r1 log](./runs/C-r1-single-ROUTER_ROUTER-inproc.log), [C r2 log](./runs/C-r2-single-ROUTER_ROUTER-inproc.log).

| 계열 | 실행 | Mean | P95 |
|---|---:|---:|---:|
| B | 1 | 0.119 | 1.206 |
| B | 2 | 1.015 | 1.898 |
| B | 3 | 0.038 | 0.107 |
| B | 4 | 0.190 | 1.421 |
| B | 5 | 0.391 | 1.672 |
| B | 6 | 0.538 | 1.673 |
| B | 7 | 0.014 | 0.016 |
| B | 8 | 0.962 | 1.871 |
| B | 9 | 0.022 | 0.016 |
| B | 10 | 1.116 | 1.918 |
| B 요약 | 중앙값 / 범위 | 0.290 / 0.014–1.116 | 1.546 / 0.016–1.918 |
| M | 1 | 0.114 | 0.997 |
| M | 2 | 0.052 | 0.307 |
| M | 3 | 0.893 | 1.837 |
| M | 4 | 0.980 | 1.882 |
| M | 5 | 0.031 | 0.132 |
| M | 6 | 0.988 | 1.828 |
| M | 7 | 0.030 | 0.037 |
| M | 8 | 0.287 | 0.982 |
| M | 9 | 0.062 | 0.252 |
| M | 10 | 0.060 | 0.091 |
| M 요약 | 중앙값 / 범위 | 0.088 / 0.030–0.988 | 0.644 / 0.037–1.882 |
| C | 1 | 1.056 | 1.865 |
| C | 2 | 1.138 | 1.931 |
| C | 3 | 1.087 | 1.869 |
| C | 4 | 0.136 | 1.328 |
| C | 5 | 0.091 | 0.622 |
| C | 6 | 1.153 | 1.960 |
| C | 7 | 1.097 | 1.891 |
| C | 8 | 0.082 | 0.351 |
| C | 9 | 1.095 | 1.889 |
| C | 10 | 1.152 | 1.984 |
| C 요약 | 중앙값 / 범위 | 1.091 / 0.082–1.153 | 1.879 / 0.351–1.984 |


### 256B

Raw paths: [B r1 log](./runs/B-r1-single-ROUTER_ROUTER-inproc.log), [B r2 log](./runs/B-r2-single-ROUTER_ROUTER-inproc.log) · [M r1 retry log](./runs/M-r1-retry-single-ROUTER_ROUTER-inproc.log), [M r2 log](./runs/M-r2-single-ROUTER_ROUTER-inproc.log) · [C r1 log](./runs/C-r1-single-ROUTER_ROUTER-inproc.log), [C r2 log](./runs/C-r2-single-ROUTER_ROUTER-inproc.log).

| 계열 | 실행 | Mean | P95 |
|---|---:|---:|---:|
| B | 1 | 3.110 | 5.617 |
| B | 2 | 3.111 | 5.673 |
| B | 3 | 3.047 | 5.632 |
| B | 4 | 0.082 | 0.171 |
| B | 5 | 0.057 | 0.247 |
| B | 6 | 0.469 | 4.144 |
| B | 7 | 3.139 | 5.658 |
| B | 8 | 0.050 | 0.096 |
| B | 9 | 3.026 | 5.578 |
| B | 10 | 0.004 | 0.004 |
| B 요약 | 중앙값 / 범위 | 1.747 / 0.004–3.139 | 4.861 / 0.004–5.673 |
| M | 1 | 0.079 | 0.440 |
| M | 2 | 3.223 | 5.787 |
| M | 3 | 0.168 | 1.059 |
| M | 4 | 0.181 | 1.434 |
| M | 5 | 0.210 | 1.819 |
| M | 6 | 2.945 | 5.466 |
| M | 7 | 2.991 | 5.508 |
| M | 8 | 3.256 | 5.724 |
| M | 9 | 0.649 | 4.113 |
| M | 10 | 3.167 | 5.662 |
| M 요약 | 중앙값 / 범위 | 1.797 / 0.079–3.256 | 4.790 / 0.440–5.787 |
| C | 1 | 3.290 | 5.663 |
| C | 2 | 1.293 | 4.529 |
| C | 3 | 3.213 | 5.677 |
| C | 4 | 1.718 | 5.396 |
| C | 5 | 1.703 | 5.448 |
| C | 6 | 0.995 | 3.951 |
| C | 7 | 3.244 | 5.728 |
| C | 8 | 1.331 | 5.329 |
| C | 9 | 1.706 | 5.077 |
| C | 10 | 2.246 | 5.581 |
| C 요약 | 중앙값 / 범위 | 1.712 / 0.995–3.290 | 5.422 / 3.951–5.728 |

## 판정

1024B mean 기준: B 중앙값 0.290 ms, M 중앙값 0.088 ms, C 중앙값 1.091 ms. B 최대는 1.116 ms이며, **M 중앙값은 0.088 ms이고 M 10회 중 0/10회가 B 최댓값을 초과했다.**

중간 revision M은 B 결과에 가깝다. M은 #3930 (`e7e462dbec`, message content reference release)을 포함하고 #4135 (`0005a4d6c0`, complete multipart record admission)는 포함하지 않는다. 따라서 관측된 latency shift는 **#4135 이후**에 나타난 것으로 판정한다.

## Raw 결과

- Stage별 전체 실행 로그에는 5회별 mean/p95가 기록돼 있다. 위 표의 B·M·C 로그 링크를 참조한다.
- Runner 결과 파일은 [`runs/saved-results`](./runs/saved-results/)에 보존했다.
- B r1의 runner 결과 파일은 측정 뒤 기존 worktree에서 이 경로로 옮겨 보존했다: [`B r1 saved result`](./runs/saved-results/single/report/perf_c_single_linux_20261110_222244-B-r1.txt).
- M r1 사전조건 실패 로그는 raw 기록으로 유지했다; 실패 시도는 측정값에 포함하지 않았다.
