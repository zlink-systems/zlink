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

### .NET Single ROUTER_ROUTER_REQREP/tcp

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 224539 / 230228 / 216393 / 20018 / 13572 / 8164 | 0.178 / 0.171 / 0.237 / 0.594 / 0.438 / 0.364 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145246_router-router-reqrep-tcp-paired-c1.txt` |
| .NET | 138790 / 145207 / 125575 / 19227 / 13384 / 8058 | 0.243 / 0.225 / 0.257 / 0.596 / 0.427 / 0.354 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145502_dotnet-router-router-reqrep-tcp-paired-final.txt` |

throughput ratio는 `61.81% / 63.07% / 58.03% / 96.05% / 98.61% / 98.70%`, 산술평균은
`79.38%`다. latency ratio는 `1.365x / 1.316x / 1.084x / 1.003x / 0.975x / 0.973x`,
산술평균은 `1.119x`다. .NET socket request/reply 기준을 충족해 통과로 기록한다.

### .NET Single PAIR/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1888260 / 1062209 / 434911 / 23059 / 15331 / 9309 | 40.495 / 24.090 / 20.763 / 13.255 / 13.052 / 21.383 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145939_dotnet-pair-ws-c-paired.txt` |
| .NET | 1476467 / 785953 / 423259 / 21887 / 14422 / 9070 | 45.809 / 38.210 / 18.671 / 14.608 / 13.782 / 21.832 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145953_dotnet-pair-ws-paired-final.txt` |

throughput ratio는 `78.19% / 73.99% / 97.32% / 94.92% / 94.07% / 97.43%`, 산술평균은
`89.32%`다. latency ratio는 `1.131x / 1.586x / 0.899x / 1.102x / 1.056x / 1.021x`,
산술평균은 `1.133x`다. .NET 단순 one-way 기준을 충족해 통과로 기록한다.

### .NET Single PUBSUB/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1160780 / 855560 / 438672 / 25388 / 16169 / 11457 | 59.594 / 33.044 / 20.319 / 12.343 / 16.621 / 17.610 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_150225_dotnet-pubsub-ws-c-paired.txt` |
| .NET baseline | 739341 / 588520 / 406582 / 24060 / 12222 / 9947 | 61.966 / 32.397 / 15.508 / 13.002 / 21.786 / 20.095 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150246_dotnet-pubsub-ws-paired-final.txt` |
| .NET 자체 1차 after | 792687 / 454521 / 414700 / 23063 / 15380 / 11274 | 64.776 / 49.475 / 16.822 / 13.588 / 16.896 / 17.838 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150638_dotnet-pubsub-ws-own-after-unchecked-publish.txt` |
| .NET Sol 2차 후보 after | 722346 / 427185 / 399433 / 22182 / 14749 / 10148 | 68.158 / 49.183 / 22.252 / 15.035 / 18.170 / 19.748 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151007_dotnet-pubsub-ws-sol-after-lock-coalesce.txt` |

baseline throughput ratio는 `63.69% / 68.79% / 92.68% / 94.77% / 75.59% / 86.82%`,
산술평균 `80.39%`다. 자체 1차 `PublishMessageUnchecked` 후보 후 ratio는
`68.29% / 53.13% / 94.54% / 90.84% / 95.12% / 98.40%`, 산술평균 `83.39%`다.
평균 latency ratio는 `1.087x / 1.497x / 0.828x / 1.101x / 1.017x / 1.013x`,
산술평균 `1.090x`다. Sol 2차 lock coalesce 후보는 throughput ratio
`62.23% / 49.93% / 91.06% / 87.37% / 91.22% / 88.57%`, 산술평균 `78.40%`,
latency 산술평균 `1.193x`로 1차 후보보다 악화되어 제거했다. 최종은 자체 1차 후보를
유지하며 aggregate throughput 목표 미달과 추가 후보 no-go로 보류한다.

### .NET Single DEALER_DEALER/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1589034 / 1049658 / 455499 / 22908 / 15237 / 9224 | 48.835 / 32.324 / 19.929 / 13.725 / 13.099 / 21.614 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151412_dotnet-dealer-dealer-ws-c-paired.txt` |
| .NET | 1125067 / 651578 / 430151 / 20869 / 14136 / 9112 | 36.100 / 36.486 / 20.972 / 14.822 / 14.023 / 21.729 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151426_dotnet-dealer-dealer-ws-paired-final.txt` |

throughput ratio는 `70.80% / 62.08% / 94.44% / 91.10% / 92.77% / 98.79%`, 산술평균은
`85.00%`다. latency ratio는 `0.739x / 1.129x / 1.052x / 1.080x / 1.071x / 1.005x`,
산술평균은 `1.013x`다. .NET 단순 one-way 기준을 충족해 통과로 기록한다.

### .NET Single DEALER_ROUTER/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1665890 / 1043057 / 474111 / 23698 / 16272 / 9494 | 45.946 / 33.212 / 15.461 / 13.138 / 12.431 / 21.452 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151702_dotnet-dealer-router-ws-c-paired.txt` |
| .NET | 1197993 / 724849 / 456349 / 21523 / 14608 / 8721 | 51.536 / 39.517 / 19.643 / 14.805 / 13.806 / 23.185 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151909_dotnet-dealer-router-ws-paired-final.txt` |

throughput ratio는 `71.91% / 69.49% / 96.25% / 90.82% / 89.77% / 91.86%`, 산술평균은
`85.02%`다. latency ratio는 `1.122x / 1.190x / 1.270x / 1.127x / 1.111x / 1.081x`,
산술평균은 `1.150x`다. 64B·256B 개별 ratio는 결과로 기록하고 .NET routed one-way aggregate
기준을 충족해 통과로 기록한다. 다음 대상은 `ROUTER_ROUTER/ws`다.

### .NET Single ROUTER_ROUTER/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1881512 / 1076459 / 447336 / 22799 / 15505 / 9555 | 40.848 / 32.138 / 20.489 / 14.272 / 13.062 / 21.334 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_152401_router-router-ws-paired-c1.txt` |
| .NET | 1035497 / 683245 / 446108 / 21073 / 14592 / 9143 | 77.563 / 32.237 / 17.334 / 14.925 / 13.652 / 21.710 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_152419_dotnet-router-router-ws-paired-final.txt` |

throughput ratio는 `55.04% / 63.47% / 99.73% / 92.43% / 94.11% / 95.69%`, 산술평균은
`83.41%`다. latency ratio는 `1.899x / 1.003x / 0.846x / 1.046x / 1.045x / 1.018x`,
산술평균은 `1.143x`다. 64B·256B 개별 ratio는 결과로 기록하고 .NET routed one-way aggregate
기준을 충족해 통과로 기록한다. 다음 대상은 `DEALER_ROUTER_REQREP/ws`다.

### .NET Single DEALER_ROUTER_REQREP/ws

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 222790 / 187340 / 106091 / 12421 / 8428 / 5234 | 0.219 / 0.287 / 0.576 / 0.963 / 0.709 / 0.570 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_152626_dealer-router-reqrep-ws-paired-c1.txt` |
| .NET before | 134937 / 126862 / 91794 / 11558 / 8074 / 5140 | 0.258 / 760.274 / 0.554 / 1.017 / 0.726 / 0.569 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_152639_dotnet-dealer-router-reqrep-ws-paired-final.txt` |
| .NET after | 138597 / 132138 / 94561 / 11797 / 8122 / 5327 | 0.254 / 0.297 / 0.548 / 0.992 / 0.721 / 0.546 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153119_dotnet-dealer-router-reqrep-ws-own-after-c-parity.txt` |

수정 전 throughput ratio는 `60.57% / 67.72% / 86.52% / 93.05% / 95.80% / 98.20%`,
산술평균 `83.64%`이고 latency ratio 산술평균은 `442.376x`였다. C와 같은
`SendFlags.None` request submit 의미로 맞춘 후 throughput ratio는
`62.21% / 70.53% / 89.13% / 94.98% / 96.37% / 101.78%`, 산술평균 `85.83%`,
latency ratio는 `1.160x / 1.035x / 0.951x / 1.030x / 1.017x / 0.958x`,
산술평균 `1.025x`다. Sol 2차 후보는 no-go로 기록하고, socket request/reply aggregate
기준을 충족해 통과로 판정한다. 다음 대상은 `ROUTER_ROUTER_REQREP/ws`다.

### .NET Single ROUTER_ROUTER_REQREP/ws

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 209462 / 180411 / 102852 / 12195 / 8260 / 5267 | 0.235 / 0.296 / 0.593 / 0.980 / 0.724 / 0.567 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153631_router-router-reqrep-ws-paired-c1.txt` |
| .NET | 136405 / 117800 / 92203 / 11609 / 8149 / 5207 | 0.253 / 0.343 / 0.525 / 1.008 / 0.718 / 0.557 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153727_dotnet-router-router-reqrep-ws-own-after-c-parity.txt` |

throughput ratio는 `65.12% / 65.30% / 89.65% / 95.19% / 98.66% / 98.86%`, 산술평균은
`85.46%`다. latency ratio는 `1.077x / 1.159x / 0.885x / 1.029x / 0.992x / 0.982x`,
산술평균은 `1.021x`다. 64B·256B 개별 ratio는 결과로 기록하고 .NET socket request/reply
aggregate 기준을 충족해 통과로 판정한다. 다음 대상은 `PAIR/wss`다.

### .NET Single PAIR/wss

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1786771 / 718571 / 238726 / 10063 / 5809 / 3308 | 48.844 / 43.400 / 29.686 / 29.063 / 41.453 / 64.802 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153917_pair-wss-paired-c1.txt` |
| .NET | 1422053 / 634651 / 228984 / 9356 / 5656 / 3181 | 57.558 / 39.040 / 30.981 / 31.687 / 42.110 / 68.531 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153929_dotnet-pair-wss-paired-final.txt` |

throughput ratio는 `79.59% / 88.32% / 95.92% / 92.97% / 97.37% / 96.16%`, 산술평균은
`91.72%`다. latency ratio는 `1.178x / 0.900x / 1.044x / 1.090x / 1.016x / 1.058x`,
산술평균은 `1.048x`다. .NET 단순 one-way aggregate 기준을 충족해 통과로 판정한다.
다음 대상은 `PUBSUB/wss`다.

### .NET Single PUBSUB/wss

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1115675 / 638822 / 235404 / 10407 / 6175 / 3499 | 59.771 / 45.295 / 29.523 / 31.521 / 39.207 / 61.587 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154100_pubsub-wss-paired-c1.txt` |
| .NET | 860856 / 515748 / 219898 / 9831 / 6147 / 3371 | 70.857 / 45.045 / 38.809 / 31.194 / 40.388 / 63.580 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154120_dotnet-pubsub-wss-paired-final.txt` |

throughput ratio는 `77.16% / 80.73% / 93.41% / 94.47% / 99.55% / 96.34%`, 산술평균은
`90.28%`다. latency ratio는 `1.185x / 0.994x / 1.315x / 0.990x / 1.030x / 1.032x`,
산술평균은 `1.091x`다. .NET 단순 one-way aggregate 기준을 충족해 통과로 판정한다.
다음 대상은 `DEALER_DEALER/wss`다.

### .NET Single DEALER_DEALER/wss

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1726070 / 747007 / 234884 / 9706 / 5885 / 3227 | 49.117 / 33.673 / 27.755 / 33.869 / 41.439 / 66.812 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154250_dealer-dealer-wss-paired-c1.txt` |
| .NET | 1201142 / 623882 / 227920 / 9119 / 5597 / 2951 | 62.120 / 39.728 / 36.648 / 33.555 / 43.220 / 72.174 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154304_dotnet-dealer-dealer-wss-paired-final.txt` |

throughput ratio는 `69.59% / 83.52% / 97.04% / 93.95% / 95.11% / 91.45%`, 산술평균은
`88.44%`다. latency ratio는 `1.265x / 1.180x / 1.320x / 0.991x / 1.043x / 1.080x`,
산술평균은 `1.146x`다. .NET 단순 one-way aggregate 기준을 충족해 통과로 판정한다.
다음 대상은 `DEALER_ROUTER/wss`다.

### .NET Single DEALER_ROUTER/wss

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1761888 / 768779 / 244319 / 10117 / 6149 / 3335 | 57.733 / 43.288 / 27.707 / 31.479 / 54.809 / 68.452 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154433_dealer-router-wss-paired-c1.txt` |
| .NET | 1243969 / 652944 / 239091 / 9479 / 5895 / 3079 | 59.091 / 39.267 / 34.010 / 33.676 / 42.891 / 75.113 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154448_dotnet-dealer-router-wss-paired-final.txt` |

throughput ratio는 `70.60% / 84.93% / 97.86% / 93.69% / 95.87% / 92.32%`, 산술평균은
`89.21%`다. latency ratio는 `1.024x / 0.907x / 1.227x / 1.070x / 0.783x / 1.097x`,
산술평균은 `1.018x`다. .NET routed one-way aggregate 기준을 충족해 통과로 판정한다.

### .NET Single DEALER_ROUTER_REQREP/wss

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 206395 / 156387 / 78725 / 4613 / 2796 / 1562 | 0.249 / 0.364 / 0.795 / 2.600 / 2.144 / 1.918 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155235_dealer-router-reqrep-wss-paired-c1.txt` |
| .NET | 127036 / 115325 / 65381 / 4313 / 2487 / 1517 | 0.312 / 0.388 / 0.892 / 2.747 / 2.377 / 1.950 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155250_dealer-router-reqrep-wss-paired-final.txt` |

throughput ratio는 `61.55% / 73.74% / 83.05% / 93.50% / 88.95% / 97.12%`, 산술평균은
`82.98%`다. latency ratio는 `1.253x / 1.066x / 1.122x / 1.057x / 1.109x / 1.017x`,
산술평균은 `1.104x`다. .NET socket request/reply aggregate 기준을 충족해 통과로 판정한다.

### .NET Single ROUTER_ROUTER/wss

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1752137 / 735231 / 230999 / 9861 / 6071 / 3463 | 48.749 / 36.335 / 38.287 / 73.953 / 42.437 / 67.473 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155438_router-router-wss-paired-c1.txt` |
| .NET | 1050212 / 640637 / 231238 / 9562 / 5764 / 3090 | 69.053 / 33.221 / 32.011 / 32.777 / 42.947 / 70.039 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155452_router-router-wss-paired-final.txt` |

throughput ratio는 `59.94% / 87.13% / 100.10% / 96.97% / 94.94% / 89.23%`, 산술평균은
`88.05%`다. latency ratio는 `1.417x / 0.914x / 0.836x / 0.443x / 1.012x / 1.038x`,
산술평균은 `0.943x`다. .NET routed one-way aggregate 기준을 충족해 통과로 판정한다.

### .NET Single ROUTER_ROUTER_REQREP/wss

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 204449 / 152615 / 76590 / 4616 / 2751 / 1659 | 0.245 / 0.375 / 0.816 / 2.599 / 2.179 / 1.805 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155632_router-router-reqrep-wss-paired-c1.txt` |
| .NET | 117535 / 103869 / 67185 / 4359 / 2744 / 1504 | 0.311 / 0.424 / 0.851 / 2.713 / 2.162 / 1.967 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155645_router-router-reqrep-wss-paired-final.txt` |

throughput ratio는 `57.49% / 68.06% / 87.72% / 94.43% / 99.75% / 90.66%`, 산술평균은
`83.02%`다. latency ratio는 `1.269x / 1.131x / 1.043x / 1.044x / 0.992x / 1.090x`,
산술평균은 `1.095x`다. .NET socket request/reply aggregate 기준을 충족해 통과로 판정한다.

### .NET Single PAIR/tls

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2002027 / 961213 / 317590 / 13380 / 7870 / 4190 | 41.730 / 0.297 / 0.638 / 14.609 / 24.699 / 45.601 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155818_pair-tls-paired-c1.txt` |
| .NET before | 1713235 / 747265 / 322010 / 12056 / 7088 / 3714 | 37.130 / 40.005 / 11.208 / 16.115 / 27.178 / 51.213 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155831_pair-tls-paired-final.txt` |
| .NET after | 1523006 / 791552 / 341356 / 11732 / 7119 / 3650 | 41.682 / 30.041 / 9.157 / 16.508 / 27.103 / 52.193 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_160225_pair-tls-own-after-clock.txt` |

before throughput ratio는 `85.575% / 77.742% / 101.392% / 90.105% / 90.064% / 88.640%`, 산술평균은 `88.919%`다. after throughput ratio는 `76.073% / 82.349% / 107.483% / 87.683% / 90.457% / 87.112%`, 산술평균은 `88.526%`다. before latency ratio는 `0.890x / 134.697x / 17.567x / 1.103x / 1.100x / 1.123x`, 산술평균은 `26.080x`다. after latency ratio는 `0.999x / 101.148x / 14.353x / 1.130x / 1.097x / 1.145x`, 산술평균은 `19.979x`다.

`EpochNs()`의 Stopwatch 기반 wall-epoch 변환 후 256B·1024B latency가 개선됐지만 latency aggregate는 미달이다. Sol 2차 리뷰에서 `MessageSocketSendOperation` 재사용과 private direct-send는 public 호출자 참조·mutable state·ownership 계약 위험으로 no-go 판정했다. 측정값 기준으로 `자체 개선 후 no-go·보류`한다. 다음 대상은 `.NET Single PUBSUB/tls`다.

### .NET Single PUBSUB/tls

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1341628 / 789037 / 302617 / 13200 / 7703 / 4274 | 51.635 / 0.585 / 0.648 / 14.801 / 25.150 / 44.820 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_161248_dotnet-pubsub-tls-paired-c1.txt` |
| .NET before | 916712 / 626971 / 329376 / 12590 / 7077 / 3777 | 57.247 / 31.743 / 12.801 / 15.467 / 27.285 / 50.491 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_161306_dotnet-pubsub-tls-paired-before.txt` |
| .NET after | 925530 / 628641 / 318470 / 12867 / 7027 / 3788 | 49.458 / 31.455 / 12.441 / 15.149 / 27.503 / 50.548 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_161727_dotnet-pubsub-tls-own-after-lock.txt` |

before throughput ratio는 `68.328% / 79.460% / 108.843% / 95.379% / 91.873% / 88.372%`, 산술평균은 `88.709%`다. after throughput ratio는 `68.986% / 79.672% / 105.239% / 97.477% / 91.224% / 88.629%`, 산술평균은 `88.538%`다. before latency ratio는 `1.109x / 54.262x / 19.755x / 1.045x / 1.085x / 1.127x`, 산술평균은 `13.064x`다. after latency ratio는 `0.958x / 53.769x / 19.199x / 1.024x / 1.094x / 1.128x`, 산술평균은 `12.862x`다.

`PublishMessageUnchecked`에서 topic validation과 native submit을 같은 `SubmitGate` 구간으로 합친 후 latency aggregate가 소폭 개선됐지만 기준에는 미달했다. Sol 2차 리뷰는 `PublisherSendOperation` pooling·private direct path·topic validation 시점 변경을 public builder 참조·ownership·error semantics 위험으로 no-go 판정했다. 측정값 기준으로 `자체 개선 후 no-go·보류`한다. 다음 대상은 `.NET Single DEALER_DEALER/tls`다.

### .NET Single DEALER_DEALER/tls

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1925835 / 1007677 / 322933 / 13223 / 7665 / 3974 | 39.023 / 0.277 / 0.628 / 14.746 / 25.287 / 48.233 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162033_dotnet-dealer-dealer-tls-paired-c1.txt` |
| .NET before | 1110563 / 823854 / 343776 / 12208 / 6386 / 2820 | 60.119 / 25.706 / 8.813 / 15.943 / 30.120 / 66.582 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162044_dotnet-dealer-dealer-tls-paired-before.txt` |
| .NET after | 1187777 / 839404 / 360634 / 12191 / 7015 / 3600 | 58.093 / 22.857 / 7.957 / 15.941 / 27.396 / 52.611 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162153_dotnet-dealer-dealer-tls-own-after-inline.txt` |

before throughput ratio는 `57.667% / 81.758% / 106.454% / 92.324% / 83.314% / 70.961%`, 산술평균은 `82.080%`다. after throughput ratio는 `61.676% / 83.301% / 111.675% / 92.195% / 91.520% / 90.589%`, 산술평균은 `88.493%`다. before latency ratio는 `1.541x / 92.801x / 14.033x / 1.081x / 1.191x / 1.380x`, 산술평균은 `18.671x`다. after latency ratio는 `1.489x / 82.516x / 12.670x / 1.081x / 1.083x / 1.091x`, 산술평균은 `16.655x`다.

`SendMessageUnchecked`에 AggressiveInlining을 적용한 후 throughput·latency aggregate가 모두 개선됐지만 latency 기준은 미달했다. Sol 2차 리뷰는 `MessageSocketSendOperation` pooling·singleton·private direct-send를 independent builder와 stale-reference·ownership 계약 위험으로 no-go 판정했다. 측정값 기준으로 `자체 개선 후 no-go·보류`한다. 다음 대상은 `.NET Single DEALER_ROUTER/tls`다.

### .NET Single DEALER_ROUTER/tls

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2153255 / 1032204 / 337869 / 13319 / 7944 / 4487 | 40.947 / 0.321 / 0.604 / 14.883 / 25.267 / 45.121 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162440_dotnet-dealer-router-tls-paired-c1.txt` |
| .NET final before | 1193408 / 843356 / 346456 / 12294 / 7432 / 3849 | 52.982 / 22.270 / 12.926 / 16.046 / 26.678 / 52.010 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162451_dotnet-dealer-router-tls-paired-before.txt` |
| .NET removed after | 1142369 / 857861 / 338651 / 12552 / 7304 / 4105 | 62.651 / 32.110 / 15.209 / 15.756 / 27.110 / 49.535 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162542_dotnet-dealer-router-tls-own-after-inline.txt` |

final before throughput ratio는 `55.423% / 81.704% / 102.542% / 92.304% / 93.555% / 85.781%`, 산술평균은 `85.218%`다. 제거한 after throughput ratio는 `53.053% / 83.110% / 100.231% / 94.241% / 91.944% / 91.487%`, 산술평균은 `85.678%`다. final before latency ratio는 `1.294x / 69.377x / 21.401x / 1.078x / 1.056x / 1.153x`, 산술평균은 `15.893x`다. 제거한 after latency ratio는 `1.530x / 100.031x / 25.180x / 1.059x / 1.073x / 1.098x`, 산술평균은 `21.662x`다.

`SendRoutedMessageUnchecked` AggressiveInlining 후보는 throughput aggregate를 0.46%p 높였지만 latency aggregate를 악화시켜 revert했다. Sol 2차 리뷰는 latency 회귀를 확인하고 routed builder pooling·private direct·추가 inlining을 no-go 판정했다. 최종은 before 측정값 기준으로 `자체 후보 제거 후 no-go·보류`한다. 다음 대상은 `.NET Single DEALER_ROUTER_REQREP/tls`다.

### .NET Single ROUTER_ROUTER/tls

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2118574 / 948552 / 319706 / 13192 / 7944 / 4105 | 56.231 / 0.461 / 0.636 / 15.091 / 25.070 / 49.120 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_163028_dotnet-router-router-tls-paired-c1.txt` |
| .NET final before | 727166 / 809610 / 358973 / 12563 / 7355 / 3888 | 101.286 / 36.500 / 10.955 / 15.539 / 26.373 / 48.895 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_163041_dotnet-router-router-tls-paired-before.txt` |
| .NET removed after | 706401 / 794696 / 340576 / 12084 / 7078 / 3612 | 123.369 / 39.316 / 8.349 / 16.145 / 27.309 / 52.566 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_163135_dotnet-router-router-tls-own-after-inline.txt` |

final before throughput ratio는 `34.323% / 85.352% / 112.282% / 95.232% / 92.586% / 94.714%`, 산술평균은 `85.748%`다. 제거한 after throughput ratio는 `33.343% / 83.780% / 106.528% / 91.601% / 89.099% / 87.990%`, 산술평균은 `82.057%`다. final before latency ratio는 `1.801x / 79.176x / 17.225x / 1.030x / 1.052x / 0.995x`, 산술평균은 `16.880x`다. 제거한 after latency ratio는 `2.194x / 85.284x / 13.127x / 1.070x / 1.089x / 1.070x`, 산술평균은 `17.306x`다.

`SendRoutedMessageUnchecked` AggressiveInlining 후보는 throughput과 latency aggregate를 모두 악화시켜 revert했다. Sol 2차 리뷰는 JIT 회귀를 확인하고 routed builder pooling·private direct·추가 inlining을 no-go 판정했다. 최종은 before 측정값 기준으로 `자체 후보 제거 후 no-go·보류`한다. 다음 대상은 `.NET Single ROUTER_ROUTER_REQREP/tls`다.

### .NET Single DEALER_ROUTER_REQREP/tls

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 213956 / 180666 / 139576 / 6502 / 3758 / 2039 | 0.203 / 0.254 / 0.432 / 1.844 / 1.594 / 1.468 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162838_dotnet-dealer-router-reqrep-tls-paired-c1.txt` |
| .NET | 134330 / 127068 / 124054 / 6227 / 3683 / 2003 | 0.272 / 0.294 / 0.362 / 1.906 / 1.610 / 1.481 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162850_dotnet-dealer-router-reqrep-tls-paired-before.txt` |

throughput ratio는 `62.784% / 70.333% / 88.879% / 95.771% / 98.004% / 98.234%`, 산술평균은 `85.668%`다. latency ratio는 `1.340x / 1.157x / 0.838x / 1.034x / 1.010x / 1.009x`, 산술평균은 `1.065x`다. .NET socket request/reply aggregate 기준을 충족해 통과로 판정한다. 추가 hotpath 변경 없이 다음 대상은 `.NET Single ROUTER_ROUTER/tls`다.

### .NET Single ROUTER_ROUTER_REQREP/tls

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 217251 / 181029 / 143541 / 6325 / 3852 / 2096 | 0.197 / 0.246 / 0.420 / 1.896 / 1.556 / 1.429 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_163419_dotnet-router-router-reqrep-tls-paired-c1.txt` |
| .NET | 130110 / 124273 / 115210 / 6265 / 3638 / 2034 | 0.276 / 0.294 / 0.367 / 1.895 / 1.634 / 1.461 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_163432_dotnet-router-router-reqrep-tls-paired-before.txt` |

throughput ratio는 `59.889% / 68.648% / 80.263% / 99.051% / 94.444% / 97.042%`, 산술평균은 `83.223%`다. latency ratio는 `1.401x / 1.195x / 0.874x / 0.999x / 1.050x / 1.022x`, 산술평균은 `1.090x`다. .NET socket request/reply aggregate 기준을 충족해 통과로 판정한다. 추가 hotpath 변경 없이 다음 대상은 `.NET Single PAIR/inproc`다.

### .NET Single PAIR/inproc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2737466 / 1860478 / 1980629 / 475404 / 181373 / 85715 | 0.013 / 0.166 / 0.172 / 0.006 / 0.012 / 0.019 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_164153_dotnet-pair-inproc-paired-c1.txt` |
| .NET before | 1698244 / 1177207 / 1188690 / 105648 / 153561 / 63932 | 0.103 / 0.210 / 0.232 / 0.026 / 0.016 / 0.027 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_164205_dotnet-pair-inproc-paired-before.txt` |
| .NET own after | 1942561 / 1005103 / 924283 / 110082 / 81021 / 66643 | 0.023 / 0.050 / 0.052 / 0.024 / 0.026 / 0.026 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_164513_dotnet-pair-inproc-own-after-epoch.txt` |

before throughput ratio는 `62.037% / 63.274% / 60.016% / 22.223% / 84.666% / 74.587%`, 산술평균은 `61.134%`다. before latency ratio는 `7.923x / 1.265x / 1.349x / 4.333x / 1.333x / 1.421x`, 산술평균은 `2.937x`다. 자체 Epoch nanosecond fast path after throughput ratio는 `70.962% / 54.024% / 46.666% / 23.155% / 44.671% / 77.750%`, 산술평균은 `52.871%`이고 latency ratio는 `1.769x / 0.301x / 0.302x / 4.000x / 2.167x / 1.368x`, 산술평균은 `1.651x`다. throughput aggregate가 악화되어 후보를 제거하고 before 측정값을 최종값으로 채택한다. .NET inproc 단순 one-way aggregate 기준으로 통과한다. 추가 contract-safe hotpath 후보는 no-go다. 다음 대상은 `.NET Single PUBSUB/inproc`다.

### .NET Single PUBSUB/inproc

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1346308 / 1198314 / 1201421 / 433968 / 183320 / 83774 | 0.018 / 0.047 / 0.107 / 0.007 / 0.012 / 0.021 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_164932_dotnet-pubsub-inproc-paired-c1.txt` |
| .NET | 1209927 / 858100 / 791610 / 295406 / 145611 / 63650 | 0.034 / 0.176 / 0.133 / 0.011 / 0.016 / 0.027 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_164952_dotnet-pubsub-inproc-paired-before.txt` |

throughput ratio는 `89.870% / 71.609% / 65.889% / 68.071% / 79.430% / 75.978%`, 산술평균은 `75.141%`다. latency ratio는 `1.889x / 3.745x / 1.243x / 1.571x / 1.333x / 1.286x`, 산술평균은 `1.845x`다. .NET inproc 단순 one-way aggregate 기준을 충족해 통과로 판정한다. 추가 hotpath 변경 없이 다음 대상은 `.NET Single DEALER_DEALER/inproc`다.
