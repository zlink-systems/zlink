# 0.17.2 idle 재측정

## 결과

- 대상: `main`의 `c4c983325a` (0.17.2 태그 `core/v0.17.2`는 `ebe7813d00e4`), local Release `core/build/lib/libzlink.so.0.17.2`.
- 소스·스펙·커밋·stash 변경은 없었다. 시작 및 종료 시 `git status --short -- core bindings scripts`는 비어 있다.
- `JOBS=4 scripts/build-core.sh release --lib-only`로 03:57 KST에 라이브러리를 갱신했다. 모든 runner는 `ZLINK_CORE_SOURCE=local`과 해당 라이브러리를 출력해 확인했고, `--reuse-build`여서 자동 Core 재빌드는 없었다.
- with_stream은 27/27 PASS, perf/c single은 30/30 성공, 정정 multi는 40/40 성공이다. 실패·skip·unsupported는 모두 0이다.

## 유휴 및 load 기록

측정 전 `ninja`가 없는 상태에서 1분 load가 2분 이상 1.0 미만임을 확인했다.

| 구간 | 시각(KST) | load 1/5/15분 | 비고 |
|---|---|---:|---|
| 최초 유휴 | 03:54 / 03:55 / 03:56 | 0.27/1.20/1.64 → 0.23/1.01/1.54 → 0.12/0.81/1.43 | `ninja` 없음 |
| with_stream 직전 | 03:58 | 1.94/1.38/1.58 | Release 빌드 직후 잔류 load; CCU 1000 실행 중 load는 약 3.3 |
| single 전 유휴 | 04:04~04:06 | 0.92/1.91/1.82 → 0.10/1.20/1.57 | `ninja` 없음 |
| single 직전 | 04:06 | 0.06/1.10/1.52 | 실행 중 약 1.4~1.9 |
| 정정 multi 전 유휴 | 04:34~04:36 | 0.66/3.37/3.11 → 0.11/2.05/2.64 | `ninja` 없음 |
| 정정 multi 직전 | 04:37 | 0.10/1.98/2.62 | runner META 0.25/1.98/2.61; 실행 중 약 5~6 |

모든 벤치는 `flock /tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/PERF_LOCK` 안에서 포그라운드로 직렬 실행했다.

## with_stream (CCU 1000, runs 3 중앙값, kops/s)

결과: `bindings/c/bench/with_stream/results/20260908_035802/`.
`현재/기준`은 각각 해당 시점의 zlink 또는 asio 절대 처리량 비율이고, `z/a`는 zlink/asio다.

| size | 현재 zlink | 현재 asio | 현재 zmq | 현재 z/a | Phase 0 z/a | Phase 2S z/a | G-11b z/a | 현재/Phase 0 (z/a) | 현재/Phase 2S (z/a) | 현재/G-11b (z/a) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 290.74 | 370.24 | 339.30 | 0.785 | 0.835 | 0.821 | 0.814 | 1.081 / 1.150 | 1.004 / 1.049 | 0.974 / 1.010 |
| 1024 B | 272.86 | 334.67 | 300.87 | 0.815 | 0.768 | 0.823 | 0.817 | 1.123 / 1.058 | 1.019 / 1.029 | 0.983 / 0.986 |
| 65536 B | 34.68 | 41.48 | 28.41 | 0.836 | 0.775 | 0.787 | 0.820 | 1.141 / 1.058 | 1.064 / 1.002 | 1.023 / 1.002 |

Phase 0/Phase 2S/G-11b 값은 계획 §7.1의 idle 행을 사용했다. 모든 stack·size·run은 PASS이며 mismatch는 0이다.

## perf/c 1024 B: 계획 §7.4 절대 기준 대비

처리량 단위는 Kmsg/s 또는 Kops/s(패턴의 runner 표기). Phase 0 multi는 계획에서 부하 오염으로 폐기됐으므로 참고만 하고, 판정 기준은 Phase 2G다.

| 패턴 | 현재 | Phase 0 | 현재/Phase 0 | Phase 2G | 현재/Phase 2G |
|---|---:|---:|---:|---:|---:|
| single PAIR | 753.8 | 890.7 | 84.6% | 883.8 | 85.3% |
| single PUBSUB | 670.9 | 646.0 | 103.9% | 626.5 | 107.1% |
| single DEALER_DEALER | 772.9 | 787.4 | 98.2% | 769.8 | 100.4% |
| single DEALER_ROUTER | 775.1 | 767.6 | 101.0% | 760.7 | 101.9% |
| single ROUTER_ROUTER | 733.1 | 744.4 | 98.5% | 732.2 | 100.1% |
| multi DEALER_DEALER | 908.7 | 561.4 | 161.9% | 905.1 | 100.4% |
| multi DR_SENDSEND | 221.6 | 166.6 | 133.0% | 273.9 | 80.9% |
| multi RR_SENDSEND | 181.2 | 111.5 | 162.5% | 242.5 | 74.7% |
| multi DR_REQREP | 170.8 | 95.2 | 179.5% | 208.5 | 81.9% |
| multi RR_REQREP | 137.5 | 73.0 | 188.4% | 170.1 | 80.9% |
| multi PUBSUB | 848.8 | 541.5 | 156.8% | 1009.0 | 84.1% |
| multi STREAM | 210.2 | 124.2 | 169.2% | 227.1 | 92.5% |

## 전 size 비율: Phase 2G full-size raw reference 대비

계획 §7.4가 가리킨 full-size 파일은 당시 runs 1이며 multi 시작 load가 2.67이어서, 아래는 전 size 관측 비교이지 Phase 2G idle 판정값을 대체하지 않는다. 표의 각 값은 `현재 처리량 K/s (현재 / reference)`이다.

single reference: `bindings/c/perf/results/single/report/perf_c_single_linux_20260907_045258_phase2g-fullsize.txt`.

| single | 64 B | 256 B | 1024 B | 65536 B | 131072 B | 262144 B |
|---|---:|---:|---:|---:|---:|---:|
| PAIR | 1277.0 (0.989) | 1067.6 (1.011) | 753.8 (0.911) | 41.35 (0.681) | 27.44 (0.784) | 16.46 (0.923) |
| PUBSUB | 1052.4 (1.111) | 926.5 (1.069) | 670.9 (1.087) | 12.90 (1.011) | 5.91 (0.988) | 2.52 (1.029) |
| DEALER_DEALER | 1128.9 (0.953) | 974.6 (0.955) | 772.9 (1.038) | 42.06 (1.027) | 27.25 (1.087) | 16.55 (1.069) |
| DEALER_ROUTER | 1140.4 (1.044) | 977.1 (0.993) | 775.1 (1.045) | 41.57 (1.065) | 26.71 (1.056) | 16.43 (1.046) |
| ROUTER_ROUTER | 1026.8 (1.093) | 916.9 (1.136) | 733.1 (1.176) | 40.57 (1.190) | 25.85 (1.178) | 15.56 (1.175) |

multi reference: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_044847_phase2g-fullsize.txt`.

| multi | 64 B | 256 B | 1024 B | 4096 B | 65536 B | 131072 B |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 1039.2 (0.918) | 944.1 (0.905) | 908.7 (1.102) | 375.8 (1.089) | 74.31 (1.082) | 38.02 (1.102) |
| DR_SENDSEND | 275.5 (1.093) | 235.0 (0.972) | 221.6 (1.051) | 176.6 (1.079) | 32.04 (1.110) | 12.56 (1.389) |
| RR_SENDSEND | 236.8 (1.014) | 165.7 (1.052) | 181.2 (1.202) | 153.8 (1.168) | 28.01 (1.237) | 6.02 (0.535) |
| DR_REQREP | 209.7 (1.178) | 183.2 (1.469) | 170.8 (1.358) | 149.3 (1.537) | 24.77 (1.380) | 16.01 (1.278) |
| RR_REQREP | 195.0 (1.144) | 168.3 (1.303) | 137.5 (1.082) | 123.2 (1.118) | 22.86 (1.063) | 15.19 (1.094) |
| PUBSUB | 916.6 (0.983) | 849.0 (1.130) | 848.8 (1.091) | 676.6 (1.069) | 73.33 (1.253) | 32.45 (1.089) |
| STREAM | 250.8 (1.106) | 216.0 (1.256) | 210.2 (1.292) | — | 28.68 (1.517) | — |

## 실행 및 산출물

- with_stream: `ZLINK_CORE_SOURCE=local ./bindings/c/bench/with_stream/run_benchmarks.sh --stack zlink,asio,zmq --size all --ccu 1000 --runs 3 --reuse-build`
- single: `ZLINK_CORE_SOURCE=local .../run_benchmarks.sh --pattern PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER --transports tcp --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 --reuse-build`
- multi 채택 결과: `ZLINK_CORE_SOURCE=local .../run_benchmarks_multi.sh --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB,STREAM --transports tcp --runs 3 --reuse-build`
- single 결과: `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_040657_measure-0.17.2-idle.txt`
- multi 채택 결과: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_043702_measure-0.17.2-idle-corrected.txt`
- 첫 multi 파일 `perf_c_multi_linux_20260908_041947_measure-0.17.2-idle.txt`은 4096 B 누락/262144 B 추가의 범위 오류 때문에 비교·판정에 사용하지 않았다.

변경 파일: 이 보고서와 진행 파일뿐이다. 코드 변경이 없으므로 변경 분류·스펙 동작 변경은 해당하지 않는다.
