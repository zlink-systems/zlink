## 측정 기록과 결과

Core `v0.10.1` Git release runtime, Release build, auto-HWM balanced, I/O thread 1,
timeout 200ms 조건을 사용했다. 각 paired target은 C를 먼저 실행한 뒤 binding을 실행했으며,
perf process는 동시에 실행하지 않았다. 아래에는 유효한 측정값과 결과만 기록한다.

| 대상 | size별 C throughput | size별 C++ throughput | size별 ratio | 중앙값 | latency ratio 중앙값 | 판정 |
|------|---------------------|-----------------------|--------------|--------|----------------------|------|
| Single `DEALER_ROUTER_REQREP / wss` | 169.9074 / 133.8474 / 56.7648 / 3.9784 / 2.7550 / 1.4842 Kops/s | 133.7758 / 122.6814 / 67.2556 / 4.0006 / 2.6088 / 1.3776 Kops/s | 78.73% / 91.66% / 118.48% / 100.56% / 94.69% / 92.82% | 93.76% | 1.067x | 통과 |
| Single `DEALER_ROUTER_REQREP / tls` | 201.5056 / 172.9374 / 117.1400 / 5.0832 / 3.6956 / 2.0056 Kops/s | 202.6744 / 161.0602 / 94.0698 / 5.6198 / 3.6312 / 1.9980 Kops/s | 100.58% / 93.13% / 80.31% / 110.56% / 98.26% / 99.62% | 98.94% | 1.021x | 통과 |

WSS reports: C `223651`, `223544`, `223757`, `223903`, `122459`, `124208`; C++ `223724`,
`223617`, `223828`, `223935`, `124133`, `124246`.

TLS reports: C `125057`, `224926`, `225036`, `225141`, `125201`, `125305`; C++ `125128`,
`224959`, `225107`, `225213`, `125233`, `125342`.

| 대상 | size별 throughput ratio | throughput 산술평균 | latency ratio 산술평균 | 판정 |
|------|-------------------------|--------------------|-----------------------|------|
| Single `PAIR / inproc` | 75.31% / 100.37% / 91.98% / 20.57% / 34.20% / 79.03% | 66.91% | 1.496x | 보류 |
| Single `PUBSUB / inproc` | 93.17% / 93.27% / 98.13% / 31.59% / 61.80% / 70.10% | 74.68% | aggregate 미달 | 보류 |
| Single `DEALER_DEALER / inproc` | 89.15% / 102.63% / 100.59% / 23.90% / 64.94% / 68.80% | 75.00% | aggregate 미달 | 보류 |
| Single `DEALER_ROUTER / inproc` | 85.68% / 93.39% / 92.89% / 26.72% / 63.11% / 68.82% | 71.77% | aggregate 미달 | 보류 |
| Single `DEALER_ROUTER_REQREP / inproc` | 94.86% / 93.67% / 93.65% / 30.48% / 97.49% / 97.51% | 84.61% | aggregate 미달 | 보류 |
| Single `ROUTER_ROUTER / inproc` | 90.24% / 92.39% / 83.22% / 19.63% / 57.02% / 63.97% | 67.75% | aggregate 미달 | 보류 |
| Single `ROUTER_ROUTER_REQREP / inproc` | 81.73% / 87.19% / 87.08% / 34.92% / 92.40% / 87.45% | 78.46% | aggregate 미달 | 보류 |

Multi `PUBSUB` throughput aggregate는 `ws 101.72%`, `wss 94.49%`, `tls 96.46%`로
각 목표를 충족했다. 평균 latency ratio는 각각 `6.576x`, `2.076x`, `2.017x`로 2.0x를
초과해 세 대상은 보류했다. 그 밖의 선택 Multi 대상은 throughput·평균 latency aggregate를
통과했다. `MULTI_DEALER_ROUTER_REQREP / wss`는 throughput 산술평균 `85.02%`로 통과했다.

PAIR/inproc C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_125802_pair-inproc-current-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_130047_pair-inproc-current-cpp1.txt`.

### .NET Single PAIR/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2452.925 / 1264.220 / 702.028 / 41.942 / 26.358 / 15.910 | 1.051 / 0.229 / 0.292 / 4.841 / 7.766 / 12.915 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_133934_dotnet-pair-tcp-c-full.txt` |
| .NET 자체 pass before | 1800.074 / 915.362 / 590.222 / 34.316 / 23.467 / 14.556 | 0.096 / 0.278 / 0.394 / 5.904 / 8.651 / 14.057 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143203_dotnet-pair-tcp-own-before-pool.txt` |
| .NET 자체 pass after | 1908.233 / 904.756 / 588.862 / 36.686 / 25.019 / 14.296 | 0.137 / 0.264 / 0.374 / 5.531 / 8.168 / 14.237 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142021_dotnet-pair-tcp-parity-baseline.txt` |

After throughput ratio is `77.79% / 71.57% / 83.88% / 87.47% / 94.92% / 89.86%`, arithmetic mean `84.25%`; before was `82.03%`. After average latency ratio is `0.130x / 1.153x / 1.281x / 1.143x / 1.052x / 1.102x`, arithmetic mean `0.977x`. The throughput target remains unmet. Sol second pass found no contract-safe candidate beyond the existing pool and reusable receive path, so the target is held.

### .NET Single PUBSUB/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1601.681 / 1052.561 / 598.716 / 40.742 / 26.514 / 16.116 | 0.129 / 0.275 / 0.344 / 4.990 / 7.719 / 12.789 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135104_dotnet-pubsub-tcp-c1.txt` |
| .NET 자체 pass before | 1011.038 / 701.552 / 535.344 / 38.566 / 23.859 / 12.613 | 0.104 / 0.229 / 0.447 / 5.305 / 8.593 / 15.936 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142030_dotnet-pubsub-tcp-parity-baseline.txt` |
| .NET 자체 pass after | 997.907 / 744.385 / 538.418 / 40.003 / 22.923 / 14.812 | 0.093 / 0.208 / 0.708 / 5.110 / 8.850 / 13.772 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142943_dotnet-pubsub-tcp-own-after-pool.txt` |

After throughput ratio is `62.30% / 70.72% / 89.93% / 98.19% / 86.46% / 91.91%`, arithmetic mean `83.25%`; before was `80.35%`. After average latency ratio is `0.721x / 0.756x / 2.058x / 1.024x / 1.147x / 1.077x`, arithmetic mean `1.130x`. The throughput target remains unmet. Sol second pass found no contract-safe message/builder reuse or direct path, so the target is held.

### .NET Single DEALER_DEALER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2564.108 / 1322.261 / 711.897 / 41.628 / 26.101 / 15.477 | 1.100 / 0.259 / 0.351 / 4.886 / 7.846 / 13.294 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135400_dotnet-dealer-dealer-tcp-c1.txt` |
| .NET 자체 pass before | 1437.752 / 933.404 / 637.397 / 33.244 / 21.741 / 12.501 | 49.561 / 0.218 / 0.488 / 6.082 / 9.357 / 16.244 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143235_dotnet-dealer-dealer-tcp-own-before-pool.txt` |
| .NET 자체 pass after | 1197.629 / 911.056 / 623.238 / 34.232 / 23.788 / 14.258 | 47.569 / 0.218 / 0.491 / 5.876 / 8.564 / 14.283 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142040_dotnet-dealer-dealer-tcp-parity-baseline.txt` |

After throughput ratio is `46.71% / 68.90% / 87.55% / 82.23% / 91.14% / 92.12%`, arithmetic mean `78.11%`; before was `76.69%`. After average latency ratio is `43.245x / 0.842x / 1.399x / 1.203x / 1.092x / 1.074x`, arithmetic mean `8.142x`. The throughput target remains unmet. Sol second pass found no contract-safe candidate; builder pool, private direct send and ownership changes are excluded, so the target is held.

### .NET Single DEALER_ROUTER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2385.893 / 1154.776 / 713.551 / 40.904 / 25.679 / 16.368 | 20.331 / 0.751 / 9.451 / 4.976 / 8.038 / 12.730 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135529_dotnet-dealer-router-tcp-c1.txt` |
| .NET baseline | 1189.906 / 1005.534 / 676.978 / 36.908 / 23.706 / 14.595 | 53.346 / 0.274 / 0.516 / 5.517 / 8.690 / 14.259 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_141217_dotnet-dealer-router-tcp-baseline.txt` |

Throughput ratio is `49.87% / 87.08% / 94.87% / 90.23% / 92.32% / 89.17%`, arithmetic mean `83.92%`. Average latency ratio is `2.624x / 0.365x / 0.055x / 1.109x / 1.081x / 1.120x`, arithmetic mean `1.059x`. The aggregate throughput and latency targets pass; the 64B throughput ratio is recorded as an individual result and does not change the aggregate decision.

### .NET Single DEALER_ROUTER_REQREP/tcp

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 212.246 / 192.438 / 176.010 / 17.175 / 11.981 / 7.340 | 0.186 / 0.211 / 0.293 / 0.691 / 0.496 / 0.404 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt` |
| .NET | 144.297 / 131.794 / 129.872 / 19.124 / 13.039 / 8.069 | 0.239 / 0.263 / 0.262 / 0.602 / 0.442 / 0.357 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143942_dotnet-dealer-router-reqrep-tcp-paired-v2.txt` |

Throughput ratio is `67.99% / 68.49% / 73.79% / 111.35% / 108.83% / 109.94%`, arithmetic mean `90.06%`. Average latency ratio is `1.285x / 1.246x / 0.894x / 0.871x / 0.891x / 0.884x`, arithmetic mean `1.012x`. The request/reply aggregate targets pass. The Router setup wait was aligned with the C activity-driven monitor gate before this measurement.

### .NET Single ROUTER_ROUTER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2301.443 / 1294.360 / 710.263 / 40.080 / 25.590 / 16.033 | 0.153 / 0.269 / 0.292 / 5.099 / 8.056 / 12.995 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_144914_router-router-tcp-paired-c1.txt` |
| .NET 자체 pass before | 1056.839 / 966.593 / 653.096 / 36.327 / 23.264 / 14.245 | 38.877 / 0.362 / 0.444 / 5.562 / 8.770 / 14.382 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_144617_dotnet-router-router-tcp-parity-final.txt` |
| .NET 자체 pass after | 1089.388 / 958.659 / 674.102 / 36.141 / 23.993 / 14.095 | 35.744 / 0.281 / 0.371 / 5.605 / 8.519 / 14.530 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_144719_dotnet-router-router-tcp-own-after.txt` |
| .NET 최종 paired | 960.579 / 951.917 / 657.919 / 35.811 / 23.811 / 14.371 | 47.393 / 0.262 / 0.373 / 5.659 / 8.578 / 14.175 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_144931_dotnet-router-router-tcp-paired-final.txt` |

최종 paired throughput ratio는 `41.74% / 73.54% / 92.63% / 89.35% / 93.05% / 89.63%`,
산술평균 `79.99%`다. 평균 latency ratio는 `309.758x / 0.974x / 1.277x / 1.110x /
1.065x / 1.091x`, 산술평균 `52.546x`다. 자체 routed `flags == None` 분기는 진단 A/B에서
throughput `99.17% → 100.26%`, latency `43.945x → 40.405x`를 기록했지만, Sol pass에서
추가 builder/message pool·private direct·queue 조정 외의 contract-safe 후보를 찾지 못했다.
throughput과 latency aggregate가 모두 목표를 충족하지 않아 측정값으로 보류한다.
