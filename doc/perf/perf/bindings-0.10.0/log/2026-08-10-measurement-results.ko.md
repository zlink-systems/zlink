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

### .NET Single DEALER_DEALER/inproc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2356980 / 1795347 / 1893613 / 489617 / 186321 / 84383 | 0.075 / 0.164 / 0.180 / 0.009 / 0.012 / 0.020 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165225_dotnet-dealer-dealer-inproc-paired-c1.txt` |
| .NET | 1401362 / 1064182 / 1034631 / 148685 / 178865 / 68888 | 0.125 / 0.282 / 0.323 / 0.020 / 0.013 / 0.025 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_165237_dotnet-dealer-dealer-inproc-paired-before.txt` |

throughput ratio는 `59.456% / 59.274% / 54.638% / 30.368% / 95.998% / 81.637%`, 산술평균은 `63.562%`다. latency ratio는 `1.667x / 1.720x / 1.794x / 2.222x / 1.083x / 1.250x`, 산술평균은 `1.623x`다. .NET inproc 단순 one-way aggregate 기준을 충족해 통과로 판정한다. 추가 hotpath 변경 없이 다음 대상은 `.NET Single DEALER_ROUTER/inproc`다.

### .NET Single DEALER_ROUTER/inproc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2221583 / 1699994 / 1966884 / 501256 / 196334 / 85813 | 0.085 / 0.183 / 0.180 / 0.006 / 0.012 / 0.020 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165429_dotnet-dealer-router-inproc-paired-c1.txt` |
| .NET | 1342474 / 1102788 / 1097378 / 309148 / 130588 / 66330 | 0.144 / 0.280 / 0.324 / 0.017 / 0.024 / 0.029 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_165440_dotnet-dealer-router-inproc-paired-before.txt` |

throughput ratio는 `60.429% / 64.870% / 55.793% / 61.675% / 66.513% / 77.296%`, 산술평균은 `64.429%`다. latency ratio는 `1.694x / 1.530x / 1.800x / 2.833x / 2.000x / 1.450x`, 산술평균은 `1.885x`다. .NET inproc routed one-way aggregate 기준을 충족해 통과로 판정한다. 추가 hotpath 변경 없이 다음 대상은 `.NET Single DEALER_ROUTER_REQREP/inproc`다.

### .NET Single DEALER_ROUTER_REQREP/inproc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 348493 / 317541 / 333373 / 140787 / 84081 / 47572 | 0.095 / 0.104 / 0.097 / 0.047 / 0.043 / 0.045 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165648_dotnet-dealer-router-reqrep-inproc-paired-c1.txt` |
| .NET before | 159941 / 157648 / 153670 / 108073 / 77826 / 45356 | 0.206 / 0.204 / 0.205 / 0.058 / 0.044 / 0.048 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_165700_dotnet-dealer-router-reqrep-inproc-paired-before.txt` |
| .NET own after inline | 167232 / 157741 / 151818 / 109419 / 77463 / 44346 | 0.191 / 0.204 / 0.212 / 0.057 / 0.045 / 0.049 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_165825_dotnet-dealer-router-reqrep-inproc-own-after-inline.txt` |
| .NET Sol after single-part | 160287 / 152125 / 159746 / 116343 / 80689 / 46382 | 0.203 / 0.212 / 0.202 / 0.054 / 0.043 / 0.048 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_170138_dotnet-dealer-router-reqrep-inproc-sol-after-single.txt` |

before throughput ratio는 `45.895% / 49.647% / 46.096% / 76.763% / 92.561% / 95.342%`, 산술평균은 `67.717%`다. before latency ratio는 `2.168x / 1.962x / 2.113x / 1.234x / 1.023x / 1.067x`, 산술평균은 `1.595x`다. 자체 inlining after throughput ratio는 `47.987% / 49.676% / 45.540% / 77.720% / 92.129% / 93.219%`, 산술평균은 `67.712%`다. 자체 inlining after latency ratio는 `2.011x / 1.962x / 2.186x / 1.213x / 1.047x / 1.089x`, 산술평균은 `1.584x`다. Sol single-part specialization after throughput ratio는 `45.994% / 47.907% / 47.918% / 82.638% / 95.966% / 97.499%`, 산술평균은 `69.654%`다. Sol after latency ratio는 `2.137x / 2.038x / 2.082x / 1.149x / 1.000x / 1.067x`, 산술평균은 `1.579x`다.

Sol single-part specialization은 단일 part 요청에서 임시 `IReadOnlyList` 생성을 제거하고 기존 ownership·timeout·callback·error 처리를 private 공통 경로로 유지했다. throughput과 latency가 before보다 개선됐지만 throughput aggregate 70%에는 0.346%p 미달해 최종 상태는 `보류`다. POSDDD 기준의 내부 책임 경계 개선이 있어 후보는 최종 코드에 유지하며, 추가 contract-safe 후보는 no-go다. .NET build와 contract test 결과는 build 성공, `149 passed / 0 failed / 0 skipped`다.

### .NET Single ROUTER_ROUTER_REQREP/inproc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 295094 / 306826 / 308693 / 128330 / 81310 / 46879 | 0.112 / 0.107 / 0.105 / 0.052 / 0.045 / 0.045 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_171326_dotnet-router-router-reqrep-inproc-paired-c1.txt` |
| .NET before | 142666 / 139310 / 145320 / 76354 / 83628 / 46975 | 0.228 / 0.226 / 0.217 / 0.084 / 0.040 / 0.046 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_171339_dotnet-router-router-reqrep-inproc-paired-before.txt` |
| .NET own after single-part | 148750 / 158713 / 145784 / 75413 / 79705 / 45700 | 0.211 / 0.198 / 0.215 / 0.084 / 0.042 / 0.046 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_171524_dotnet-router-router-reqrep-inproc-own-after-single.txt` |
| .NET Sol after submitter | 135345 / 149269 / 139200 / 96658 / 70257 / 47175 | 0.248 / 0.218 / 0.231 / 0.064 / 0.049 / 0.046 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_171859_dotnet-router-router-reqrep-inproc-sol-after-submit.txt` |

before throughput ratio는 `48.346% / 45.404% / 47.076% / 59.498% / 102.851% / 100.205%`, 산술평균은 `67.230%`다. before latency ratio는 `2.036x / 2.112x / 2.067x / 1.615x / 0.889x / 1.022x`, 산술평균은 `1.624x`다. 자체 single-part specialization after throughput ratio는 `50.408% / 51.727% / 47.226% / 58.765% / 98.026% / 97.485%`, 산술평균은 `67.273%`다. 자체 after latency ratio는 `1.884x / 1.850x / 2.048x / 1.615x / 0.933x / 1.022x`, 산술평균은 `1.559x`다. Sol submitter after throughput ratio는 `45.865% / 48.649% / 45.093% / 75.320% / 86.406% / 100.631%`, 산술평균은 `66.994%`다. Sol after latency ratio는 `2.214x / 2.037x / 2.200x / 1.231x / 1.089x / 1.022x`, 산술평균은 `1.632x`다.

자체 single-part specialization은 단일 part 요청에서 임시 목록 경로를 제거하고 Router의 요청 제출 책임을 private 공통 경로로 정리했다. throughput aggregate가 `67.230%`에서 `67.273%`로 개선되고 latency aggregate가 `1.624x`에서 `1.559x`로 개선되어 POSDDD 기준의 책임 경계 개선과 함께 최종 코드에 유지한다. Sol의 private direct submitter 후보는 throughput `66.994%`, latency `1.632x`로 자체 after보다 악화되어 제거했다. aggregate throughput 70%에는 미달하고 추가 contract-safe 후보가 없어 최종 상태는 `보류`다. .NET build와 contract test 결과는 build 성공, `149 passed / 0 failed / 0 skipped`다.

### .NET Single ROUTER_ROUTER_REQREP/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 256353 / 226815 / 231747 / 21861 / 14709 / 9040 | 0.150 / 0.173 / 0.192 / 0.544 / 0.404 / 0.328 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_172657_dotnet-router-router-reqrep-ipc-paired-c1.txt` |
| .NET | 130843 / 130885 / 125631 / 21052 / 14751 / 8780 | 0.260 / 0.258 / 0.264 / 0.557 / 0.398 / 0.333 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_172710_dotnet-router-router-reqrep-ipc-paired-before.txt` |

throughput ratio는 `51.040% / 57.706% / 54.210% / 96.299% / 100.286% / 97.124%`, 산술평균은 `76.111%`다. latency ratio는 `1.733x / 1.491x / 1.375x / 1.024x / 0.985x / 1.015x`, 산술평균은 `1.271x`다. 6개 size와 30개 result line이 모두 complete이며 .NET request/reply aggregate 기준을 충족해 최종 상태는 `통과`다. 현재 적용된 single-part 내부 경로와 public contract를 유지하고, 추가 hotpath 또는 POSDDD 구조 변경은 채택하지 않는다. 다음 대상은 아직 미측정인 `ROUTER_ROUTER / inproc`이다.

### .NET Single ROUTER_ROUTER/inproc

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2688652 / 1822075 / 2001808 / 483366 / 165454 / 85244 | 0.015 / 0.166 / 0.177 / 0.006 / 0.013 / 0.020 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_173203_dotnet-router-router-inproc-paired-c1.txt` |
| .NET before | 1354680 / 984394 / 1020907 / 111930 / 95780 / 57852 | 0.141 / 0.316 / 0.325 / 0.027 / 0.023 / 0.030 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_173216_dotnet-router-router-inproc-paired-before.txt` |
| .NET own native routing-id cache | 1112061 / 906648 / 914556 / 239681 / 94543 / 59649 | 0.074 / 0.252 / 0.361 / 0.014 / 0.023 / 0.028 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_173725_dotnet-router-router-inproc-own-after-native-rid.txt` |
| .NET Sol `Unsafe.SkipInit` | 1473140 / 1185362 / 1019612 / 113906 / 99686 / 54731 | 0.103 / 0.252 / 0.339 / 0.028 / 0.022 / 0.032 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_174425_dotnet-router-router-inproc-sol-after-skipinit.txt` |

before throughput ratio는 `50.385% / 54.026% / 50.999% / 23.156% / 57.889% / 67.866%`, 산술평균은 `50.720%`다. before latency ratio는 `9.400x / 1.904x / 1.836x / 4.500x / 1.769x / 1.500x`, 산술평균은 `3.485x`다. 자체 native routing-id cache after throughput ratio는 `41.361% / 49.759% / 45.686% / 49.586% / 57.142% / 69.974%`, 산술평균은 `52.251%`이며 latency ratio는 `4.933x / 1.518x / 2.040x / 2.333x / 1.769x / 1.400x`, 산술평균은 `2.332x`다. Sol review는 native cache의 동시 P/Invoke ref 사용은 안전하지만 public `RoutingId` struct layout과 생성 비용을 바꾸므로 제거하도록 판정했다.

Sol `ReceiveRouterParts` `Unsafe.SkipInit` after throughput ratio는 `54.791% / 65.056% / 50.935% / 23.565% / 60.250% / 64.205%`, 산술평균은 `53.134%`이며 latency ratio는 `6.867x / 1.518x / 1.915x / 4.667x / 1.692x / 1.600x`, 산술평균은 `3.043x`다. Sol 후보는 before보다 throughput과 latency를 모두 개선했지만 local `ROUTER_ROUTER / inproc` 목표 throughput 55%와 .NET latency 상한 3.0x에 미달한다. 추가 contract-safe 후보가 없어 `Unsafe.SkipInit`은 유지하고 최종 상태는 `보류`다. public contract·ownership·error semantics는 변경하지 않았으며 build는 0 warning/error, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Single ROUTER_ROUTER/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2224080 / 1323355 / 833920 / 41366 / 27163 / 16195 | 0.100 / 0.514 / 0.286 / 4.936 / 7.610 / 12.882 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_175017_dotnet-router-router-ipc-paired-c1.txt` |
| .NET | 1335168 / 939693 / 656589 / 37658 / 24481 / 14622 | 0.156 / 0.298 / 0.299 / 5.395 / 8.357 / 14.039 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_175032_dotnet-router-router-ipc-paired-before.txt` |

throughput ratio는 `60.032% / 71.008% / 78.735% / 91.036% / 90.126% / 90.287%`, 산술평균은 `80.204%`다. 평균 latency ratio는 `1.560x / 0.580x / 1.045x / 1.093x / 1.098x / 1.090x`, 산술평균은 `1.078x`다. 64B·256B·1024B throughput 개별 기준 미달은 측정 결과로 기록하고 aggregate 기준으로 통과한다. 추가 hotpath 또는 POSDDD 구조 변경은 채택하지 않았으며 public contract·ownership·error semantics는 변경하지 않았다.

### .NET Single PAIR/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2825899 / 1287412 / 790957 / 41608 / 26907 / 15344 | 0.080 / 0.208 / 0.264 / 4.876 / 7.626 / 13.420 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_175801_dotnet-pair-ipc-paired-c1.txt` |
| .NET | 1702707 / 1010821 / 702584 / 39199 / 25074 / 15460 | 0.115 / 0.197 / 0.271 / 5.191 / 8.134 / 13.171 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_175814_dotnet-pair-ipc-paired-before.txt` |

throughput ratio는 `60.254% / 78.516% / 88.827% / 94.210% / 93.188% / 100.756%`, 산술평균은 `85.958%`다. 평균 latency ratio는 `1.438x / 0.947x / 1.027x / 1.065x / 1.067x / 0.981x`, 산술평균은 `1.087x`다. 64B throughput 개별 기준 미달은 측정 결과로 기록하고 aggregate 기준으로 통과한다. 추가 hotpath 또는 POSDDD 구조 변경은 채택하지 않았으며 public contract·ownership·error semantics는 변경하지 않았다.

### .NET Single PUBSUB/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1491433 / 1052025 / 717917 / 40986 / 26200 / 15742 | 0.107 / 0.328 / 0.284 / 4.953 / 7.841 / 13.041 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_180020_dotnet-pubsub-ipc-paired-c1.txt` |
| .NET baseline | 980723 / 760483 / 578662 / 39775 / 25997 / 14668 | 0.095 / 0.154 / 0.300 / 5.136 / 7.913 / 13.928 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_180040_dotnet-pubsub-ipc-paired-before.txt` |
| .NET own after `AggressiveInlining` | 979569 / 761385 / 547533 / 37817 / 23619 / 14793 | 0.091 / 0.168 / 0.322 / 5.386 / 8.622 / 13.816 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_180327_dotnet-pubsub-ipc-own-after-inline.txt` |

baseline throughput ratio는 `65.757% / 72.288% / 80.603% / 97.045% / 99.225% / 93.177%`, 산술평균은 `84.683%`다. baseline 평균 latency ratio는 `0.888x / 0.470x / 1.056x / 1.037x / 1.009x / 1.068x`, 산술평균은 `0.921x`다. 자체 `PublishSingleCoreUnlocked` `AggressiveInlining` after throughput ratio는 `65.680% / 72.373% / 76.267% / 92.268% / 90.149% / 93.972%`, 산술평균은 `81.785%`이며 평균 latency ratio는 `0.850x / 0.512x / 1.134x / 1.087x / 1.100x / 1.059x`, 산술평균은 `0.957x`다. 자체 후보는 throughput·latency aggregate가 모두 악화되어 제거했다. Sol은 topic cache hit·단일 `SubmitGate`·allocation 없는 native submitter가 이미 적용되어 추가 inlining·builder pooling/reuse를 no-go 판정했다. throughput aggregate 85%에 미달해 최종 상태는 `보류`이며 public contract·ownership·error semantics는 변경하지 않았다.

### .NET Single DEALER_DEALER/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2411528 / 1319972 / 902662 / 41271 / 26294 / 15545 | 0.107 / 0.204 / 0.234 / 4.919 / 7.809 / 13.180 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_180750_dotnet-dealer-dealer-ipc-paired-c1.txt` |
| .NET baseline | 1058458 / 999507 / 746153 / 37053 / 23973 / 14836 | 0.783 / 0.212 / 0.271 / 5.449 / 8.486 / 13.716 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_180805_dotnet-dealer-dealer-ipc-paired-before.txt` |
| .NET own after `SendSingleCore` `AggressiveInlining` | 1275735 / 1036979 / 718509 / 36082 / 24545 / 14591 | 0.676 / 0.222 / 0.282 / 5.603 / 8.322 / 13.970 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_180953_dotnet-dealer-dealer-ipc-own-after-inline.txt` |

baseline throughput ratio는 `43.892% / 75.722% / 82.661% / 89.780% / 91.173% / 95.439%`, 산술평균은 `79.778%`다. baseline 평균 latency ratio는 `7.318x / 1.039x / 1.158x / 1.108x / 1.087x / 1.041x`, 산술평균은 `2.125x`다. 자체 `SendSingleCore` `AggressiveInlining` after throughput ratio는 `52.902% / 78.561% / 79.599% / 87.427% / 93.348% / 93.863%`, 산술평균은 `80.950%`이며 평균 latency ratio는 `6.318x / 1.088x / 1.205x / 1.139x / 1.066x / 1.060x`, 산술평균은 `1.979x`다. 자체 후보는 두 aggregate를 개선해 유지한다. Sol은 추가 inlining·Message pool·builder reuse를 no-go 판정했다. throughput aggregate 85%에 미달해 최종 상태는 `보류`이며 build는 성공, contract test는 `149 passed / 0 failed / 0 skipped`다. public contract·ownership·error semantics는 변경하지 않았다.

### .NET Single DEALER_ROUTER/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2571763 / 1353246 / 883394 / 41831 / 27369 / 16153 | 0.200 / 0.204 / 0.245 / 4.891 / 7.545 / 12.923 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_181357_dotnet-dealer-router-ipc-paired-c1.txt` |
| .NET baseline | 1528621 / 1006250 / 671981 / 35985 / 23711 / 15264 | 0.246 / 0.257 / 0.300 / 5.670 / 8.664 / 13.662 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_181415_dotnet-dealer-router-ipc-paired-before.txt` |
| .NET own after `SendRoutedMessageUnchecked` `AggressiveInlining` | 1565258 / 1014132 / 672384 / 37270 / 24335 / 14878 | 0.418 / 0.216 / 0.307 / 5.479 / 8.514 / 13.929 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_181515_dotnet-dealer-router-ipc-own-after-routed-inline.txt` |
| .NET Sol A/B `ReceiveRouterParts` `Unsafe.SkipInit` off | 1483380 / 961427 / 673312 / 39577 / 24348 / 14621 | 0.449 / 0.279 / 0.320 / 5.179 / 8.463 / 14.229 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_181717_dotnet-dealer-router-ipc-sol-skipinit-off.txt` |

baseline throughput ratio는 `59.439% / 74.358% / 76.068% / 86.025% / 86.635% / 94.496%`, 산술평균은 `79.503%`다. baseline 평균 latency ratio는 `1.230x / 1.260x / 1.224x / 1.159x / 1.148x / 1.057x`, 산술평균은 `1.180x`다. 자체 routed inlining after throughput ratio는 `60.863% / 74.941% / 76.114% / 89.097% / 88.914% / 92.107%`, 산술평균은 `80.339%`이며 평균 latency ratio는 `2.090x / 1.059x / 1.253x / 1.120x / 1.128x / 1.078x`, 산술평균은 `1.288x`다. Sol `ReceiveRouterParts` `Unsafe.SkipInit` off A/B throughput ratio는 `57.679% / 71.046% / 76.219% / 94.612% / 88.962% / 90.516%`, 산술평균 `79.839%`, 평균 latency ratio `1.367x`다. SkipInit을 유지한 최종 after보다 throughput·latency가 모두 악화되어 후보를 유지한다. 최종 throughput aggregate 85% 미달로 `보류`하며 public contract·ownership·error semantics는 변경하지 않았다. build는 성공, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Single DEALER_ROUTER_REQREP/ipc

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 257368 / 220382 / 231910 / 21106 / 14277 / 8640 | 0.149 / 0.182 / 0.194 / 0.563 / 0.416 / 0.343 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_182021_dotnet-dealer-router-reqrep-ipc-paired-c1.txt` |
| .NET | 135557 / 139417 / 136828 / 20547 / 14019 / 8669 | 0.273 / 0.249 / 0.239 / 0.571 / 0.418 / 0.337 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_182036_dotnet-dealer-router-reqrep-ipc-paired-before.txt` |

throughput ratio는 `52.670% / 63.262% / 59.000% / 97.351% / 98.193% / 100.336%`, 산술평균은 `78.469%`다. latency ratio는 `1.832x / 1.368x / 1.232x / 1.014x / 1.005x / 0.983x`, 산술평균은 `1.239x`다. 6개 size와 30개 result line이 모두 complete이며 .NET request/reply aggregate 기준을 충족해 최종 상태는 `통과`다. 추가 hotpath·POSDDD 구조 변경은 채택하지 않았고 public contract·ownership·error semantics는 변경하지 않았다.

### .NET Multi MULTI_DEALER_DEALER/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C와 .NET의 stop-token cleanup 경계가 동일한 상태에서 6개 size와 30개 result line을 모두 complete로 측정했다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2759596 / 1510617 / 726799 / 371826 / 95081 / 52851 | 0.181 / 0.531 / 404.765 / 297.811 / 204.628 / 290.186 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_182810_dotnet-multi-dealer-dealer-tcp-paired-c1.txt` |
| .NET baseline | 1142885 / 1013355 / 900652 / 285299 / 78073 / 41642 | 3.919 / 0.337 / 3.524 / 378.780 / 190.741 / 321.269 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_184232_dotnet-multi-dealer-dealer-tcp-paired-parity-final.txt` |
| .NET own after `Message`·단일 part buffer 경계 inlining | 1144046 / 1062310 / 871859 / 281899 / 87539 / 45522 | 1.590 / 0.714 / 3.893 / 369.523 / 195.066 / 289.593 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_184721_dotnet-multi-dealer-dealer-tcp-own-after-inline.txt` |
| .NET Sol guard inlining A/B | 1163151 / 1081574 / 876890 / 271899 / 89598 / 44627 | 1.944 / 0.386 / 3.680 / 391.555 / 182.160 / 312.482 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_184914_dotnet-multi-dealer-dealer-tcp-sol-after-guard-inline.txt` |

baseline throughput ratio는 `41.415% / 67.082% / 123.920% / 76.729% / 82.112% / 78.791%`, 산술평균은 `78.342%`이며 평균 latency ratio는 `21.652x / 0.635x / 0.009x / 1.272x / 0.932x / 1.107x`, 산술평균은 `4.268x`다. 자체 after ratio는 `41.457% / 70.323% / 119.959% / 75.815% / 92.068% / 86.133%`, 산술평균은 `80.959%`이며 평균 latency ratio는 `8.785x / 1.345x / 0.010x / 1.241x / 0.953x / 0.998x`, 산술평균은 `2.222x`다. Sol guard inlining A/B는 throughput 산술평균 `81.033%`로 자체 after 대비 `+0.074%p`에 그쳤고 latency 산술평균은 `2.460x`로 악화되어 제거했다. throughput aggregate 기준 `85%`에 미달하므로 최종 상태는 `보류`다.

POSDDD 평가: 단일/2-part와 multipart 표현의 변경 지식을 내부 `OperationMessageBuffer`가 소유하고 호출부와 public contract에는 노출하지 않는다. 기존 ownership·error semantics·public interface를 유지하면서 throughput과 latency aggregate가 모두 개선된 자체 후보를 채택했다. 별도 구조 변경 후보는 추가 복잡성 대비 분명한 이득이 없어 만들지 않았다. build는 0 warning/error, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_SENDSEND/tcp (runner MULTI_DEALER_ROUTER)

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C semantic pattern은 `MULTI_DEALER_ROUTER_SENDSEND`이고 .NET runner pattern은 `MULTI_DEALER_ROUTER`다. C relay의 connection-ready count gate를 사용하지 않고 HWM 정책을 READY 전에 확정하는 parity correction 후 C/.NET 모두 30/30 complete로 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 182915 / 183683 / 171372 / 165034 / 37001 / 22304 | 0.251 / 0.251 / 0.267 / 0.279 / 0.979 / 1.600 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_185640_dotnet-multi-dealer-router-sendsend-tcp-paired-c1.txt` |
| .NET baseline | 149304 / 153868 / 143990 / 141450 / 47569 / 28832 | 0.294 / 0.286 / 0.302 / 0.313 / 0.990 / 1.685 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_190615_dotnet-multi-dealer-router-sendsend-tcp-paired-parity-baseline.txt` |
| .NET own after `Received.Send` 단일-part 경계 inlining A/B | 150215 / 152566 / 142212 / 139483 / 42520 / 29278 | 0.293 / 0.287 / 0.307 / 0.316 / 1.101 / 1.661 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_190902_dotnet-multi-dealer-router-sendsend-tcp-own-after-received-inline.txt` |

baseline throughput ratio는 `81.625% / 83.768% / 84.022% / 85.710% / 128.561% / 129.268%`, 산술평균은 `98.826%`이며 평균 latency ratio는 `1.171x / 1.139x / 1.131x / 1.122x / 1.011x / 1.053x`, 산술평균은 `1.105x`다. 자체 A/B throughput ratio는 `82.123% / 83.059% / 82.984% / 84.518% / 114.916% / 131.268%`, 산술평균은 `96.478%`이며 평균 latency ratio는 `1.167x / 1.143x / 1.150x / 1.133x / 1.125x / 1.038x`, 산술평균은 `1.126x`다. 자체 후보는 throughput·latency aggregate가 모두 악화되어 제거했다. Sol 2차 review는 baseline이 이미 기준을 충족하고 추가 contract-safe hotpath 후보가 없어 no-go로 판정했다.

POSDDD 평가: C relay와 .NET relay의 시작 책임을 동일하게 정렬하고, Router server가 connection-ready 개수를 직접 관리하던 잘못된 lifecycle 지식을 제거했다. 이 변경은 throughput 개선과 무관하게 cross-language 변경 증폭과 timeout 경계를 줄이며, public interface·ownership·error semantics를 변경하지 않으므로 채택했다. 최종 상태는 `통과`이며 build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_REQREP/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C와 .NET의 server lifecycle을 activity-driven setup으로 맞추고, HWM을 READY 전에 확정했다. reply backpressure는 C와 동일하게 explicit EAGAIN 후 50ms POLLOUT slice로 재시도했으며, retry template과 server 수명 polling 목록을 사용했다. C와 .NET 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 99335 / 100320 / 101207 / 92840 / 24335 / 16056 | 0.347 / 0.350 / 0.343 / 0.372 / 1.015 / 1.553 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_191447_dotnet-multi-dealer-router-reqrep-tcp-paired-c1.txt` |
| .NET parity baseline | 61775 / 64372 / 54351 / 61490 / 24763 / 17635 | 0.482 / 0.457 / 0.540 / 0.492 / 1.570 / 2.571 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_192735_dotnet-multi-dealer-router-reqrep-tcp-paired-parity-template-baseline.txt` |
| .NET own after request/reply builder `AggressiveInlining` | 61614 / 66480 / 62384 / 60992 / 25204 / 18258 | 0.470 / 0.448 / 0.477 / 0.483 / 1.613 / 2.481 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_192922_dotnet-multi-dealer-router-reqrep-tcp-own-after-request-reply-inline.txt` |

parity baseline throughput ratio는 `62.189% / 64.167% / 53.703% / 66.232% / 101.759% / 109.834%`, 산술평균은 `76.314%`다. 평균 latency ratio는 `1.389x / 1.306x / 1.574x / 1.323x / 1.547x / 1.656x`, 산술평균은 `1.466x`다. 자체 builder inlining after throughput ratio는 `62.026% / 66.268% / 61.640% / 65.696% / 103.571% / 113.714%`, 산술평균은 `78.819%`이며 평균 latency ratio는 `1.354x / 1.280x / 1.391x / 1.298x / 1.589x / 1.598x`, 산술평균은 `1.418x`다. .NET socket request/reply aggregate 목표 `70%`를 충족하므로 최종 상태는 `통과`다. 개별 size ratio는 측정값으로 기록한다.

POSDDD 평가: C와 .NET의 setup·retry 책임을 같은 계층에서 처리하도록 정렬하고, retry 시도별 message 수명과 server 수명 polling을 harness 내부에 숨겼다. public interface·ownership·error semantics는 변경하지 않았다. 자체 builder inlining은 두 aggregate를 개선해 채택했고, Sol 2차 review는 추가 binding/harness 구조 변경을 no-go 판정했다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_SENDSEND/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C semantic pattern은 `MULTI_ROUTER_ROUTER_SENDSEND`이고 .NET runner에 등록된 대응 pattern은 `MULTI_ROUTER_ROUTER`다. C와 .NET의 routed echo 의미를 맞추기 위해 .NET Router client에 C와 같은 `CONNECT_ROUTING_ID=SERVER`와 `client_<index>` routing id를 설정했다. 두 report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 182672 / 179557 / 160163 / 164776 / 34962 / 21293 | 0.255 / 0.258 / 0.287 / 0.280 / 1.008 / 1.632 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_193520_dotnet-multi-router-router-sendsend-tcp-paired-c1.txt` |
| .NET runner `MULTI_ROUTER_ROUTER` | 108609 / 74146 / 134991 / 115370 / 45849 / 26149 | 0.393 / 0.562 / 0.322 / 0.377 / 1.010 / 1.856 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_193856_dotnet-multi-router-router-sendsend-tcp-paired-before-connect-rid.txt` |

throughput ratio는 `59.456% / 41.294% / 84.284% / 70.016% / 131.140% / 122.806%`, 산술평균은 `84.832%`다. 평균 latency ratio는 `1.541x / 2.178x / 1.122x / 1.346x / 1.002x / 1.137x`, 산술평균은 `1.388x`다. .NET multi routed echo aggregate 목표 `70%`와 latency 기준을 충족해 최종 상태는 `통과`다. 개별 size ratio는 측정값으로 기록한다.

POSDDD 평가: routed peer identity를 암묵적인 connection 상태에 맡기지 않고 client connection setup 책임으로 명시했다. 이 parity correction은 public contract·ownership·error semantics를 변경하지 않으며 aggregate가 이미 목표를 충족하므로 추가 binding hotpath pass와 Sol review 기반 두 번째 개선 pass는 수행하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_REQREP/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C semantic pattern과 .NET runner pattern은 모두 `MULTI_ROUTER_ROUTER_REQREP`다. C와 .NET의 Router client identity를 맞추기 위해 `client_<index>` routing id와 `ConnectRoutingId=SERVER`를 Connect 전에 설정했다. 두 report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 90448 / 92255 / 84528 / 81417 / 20501 / 15162 | 0.365 / 0.371 / 0.386 / 0.408 / 1.255 / 1.631 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_194652_dotnet-multi-router-router-reqrep-tcp-paired-c1.txt` |
| .NET runner `MULTI_ROUTER_ROUTER_REQREP` | 54380 / 52205 / 54758 / 49850 / 25790 / 15520 | 0.518 / 0.547 / 0.490 / 0.559 / 1.419 / 2.915 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_194708_dotnet-multi-router-router-reqrep-tcp-paired-before-connect-rid.txt` |

throughput ratio는 `60.123% / 56.588% / 64.781% / 61.228% / 125.799% / 102.361%`, 산술평균은 `78.480%`다. 평균 latency ratio는 `1.419x / 1.474x / 1.269x / 1.370x / 1.131x / 1.787x`, 산술평균은 `1.409x`다. .NET socket request/reply aggregate 목표 `70%`와 latency 기준을 충족해 최종 상태는 `통과`다. 개별 size ratio는 측정값으로 기록한다.

POSDDD 평가: routed peer identity를 connection readiness의 암묵적인 전제에 두지 않고 Router client의 연결 설정 책임으로 명시했다. public contract·ownership·error semantics는 변경하지 않았고, aggregate가 목표를 충족하므로 추가 binding hotpath pass와 Sol review 기반 두 번째 개선 pass는 수행하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_SENDSEND/tcp 재검토

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C report는 기존 동일 조건 paired report를 사용하고, .NET after만 routed send builder 내부 AggressiveInlining 후보 적용 후 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 182672 / 179557 / 160163 / 164776 / 34962 / 21293 | 0.255 / 0.258 / 0.287 / 0.280 / 1.008 / 1.632 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_193520_dotnet-multi-router-router-sendsend-tcp-paired-c1.txt` |
| .NET 기존 paired 결과 | 108609 / 74146 / 134991 / 115370 / 45849 / 26149 | 0.393 / 0.562 / 0.322 / 0.377 / 1.010 / 1.856 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_193856_dotnet-multi-router-router-sendsend-tcp-paired-before-connect-rid.txt` |
| .NET routed builder inlining after | 145283 / 148474 / 141657 / 139041 / 46964 / 28993 | 0.303 / 0.295 / 0.310 / 0.318 / 0.993 / 1.674 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_201438_dotnet-multi-router-router-sendsend-tcp-revisit-routed-inline.txt` |

기존 .NET throughput ratio는 `59.456% / 41.294% / 84.284% / 70.016% / 131.140% / 122.806%`, 산술평균 `84.832%`, 평균 latency ratio `1.388x`다. routed builder `Message`·`Flags`·`Submit` 내부 AggressiveInlining after ratio는 `79.532% / 82.689% / 88.446% / 84.382% / 134.329% / 136.162%`, 산술평균 `100.923%`, 평균 latency ratio `1.188x / 1.143x / 1.080x / 1.136x / 0.985x / 1.026x`, 산술평균 `1.093x`다. throughput·latency aggregate가 모두 개선되어 후보를 채택했다. public contract·ownership·error semantics는 변경하지 않았으며 build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_REQREP/tcp 재검토

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C report는 기존 동일 조건 paired report를 사용하고, .NET after만 sealed `RouterPeerRequestOperation` override 내부 AggressiveInlining 후보 적용 후 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 90448 / 92255 / 84528 / 81417 / 20501 / 15162 | 0.365 / 0.371 / 0.386 / 0.408 / 1.255 / 1.631 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_194652_dotnet-multi-router-router-reqrep-tcp-paired-c1.txt` |
| .NET 기존 paired 결과 | 54380 / 52205 / 54758 / 49850 / 25790 / 15520 | 0.518 / 0.547 / 0.490 / 0.559 / 1.419 / 2.915 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_194708_dotnet-multi-router-router-reqrep-tcp-paired-before-connect-rid.txt` |
| .NET request override inlining after | 61284 / 57966 / 57475 / 54904 / 25191 / 17045 | 0.458 / 0.499 / 0.486 / 0.529 / 1.502 / 2.587 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_201638_dotnet-multi-router-router-reqrep-tcp-revisit-request-inline.txt` |

기존 .NET throughput ratio는 `60.123% / 56.588% / 64.781% / 61.228% / 125.799% / 102.361%`, 산술평균 `78.480%`, 평균 latency ratio `1.409x`다. request override inlining after ratio는 `67.756% / 62.832% / 67.995% / 67.436% / 122.877% / 112.419%`, 산술평균 `83.553%`, 평균 latency ratio `1.255x / 1.345x / 1.259x / 1.297x / 1.197x / 1.586x`, 산술평균 `1.323x`다. throughput·latency aggregate가 모두 개선되어 후보를 채택했다. public contract·ownership·error semantics는 변경하지 않았으며 build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_PUBSUB/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `100`, duration `1s`, runs `1`, server/client I/O threads `4/4`, auto-HWM, connect-ready timeout `10000ms`, monitor-HWM `4096000`. C와 .NET 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 1578456 / 1516658 / 1115962 / 538284 / 102952 / 56610 | 346.451 / 385.566 / 309.695 / 255.565 / 106.523 / 95.833 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_195045_dotnet-multi-pubsub-tcp-paired-c1.txt` |
| .NET initial baseline | 886490 / 939682 / 761242 / 420365 / 85465 / 48766 | 423.387 / 408.899 / 423.025 / 316.981 / 107.931 / 105.926 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_195100_dotnet-multi-pubsub-tcp-paired-before.txt` |
| .NET own after `Publish` EAGAIN 후 PollOut 대기 | 896449 / 895251 / 882633 / 421806 / 89387 / 47371 | 426.581 / 416.587 / 411.942 / 327.109 / 111.389 / 107.967 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_195518_dotnet-multi-pubsub-tcp-own-after-publish-poll.txt` |
| .NET lifecycle parity rebaseline | 956353 / 919808 / 861707 / 459917 / 109815 / 60586 | 421.442 / 417.280 / 390.531 / 312.956 / 99.327 / 85.491 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_195930_dotnet-multi-pubsub-tcp-rebaseline-lifecycle.txt` |
| .NET Sol `Unsafe.SkipInit` A/B | 938519 / 1020783 / 943060 / 460975 / 96428 / 46873 | 415.460 / 424.304 / 394.469 / 305.968 / 91.003 / 105.737 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_200045_dotnet-multi-pubsub-tcp-sol-after-skipinit.txt` |

initial baseline throughput ratio는 `56.162% / 61.957% / 68.214% / 78.094% / 83.014% / 86.144%`, 산술평균은 `72.264%`이며 평균 latency ratio는 `1.222x / 1.061x / 1.275x / 1.240x / 1.013x / 1.105x`, 산술평균은 `1.168x`다. 자체 PollOut after throughput ratio는 `56.793% / 59.028% / 79.092% / 78.361% / 86.824% / 83.680%`, 산술평균은 `73.963%`이며 평균 latency ratio는 `1.231x / 1.080x / 1.330x / 1.280x / 1.046x / 1.126x`, 산술평균은 `1.182x`다.

lifecycle parity rebaseline throughput ratio는 `60.588% / 60.647% / 77.217% / 85.441% / 106.666% / 107.023%`, 산술평균은 `82.930%`이며 평균 latency ratio는 `1.216x / 1.082x / 1.261x / 1.225x / 0.932x / 0.892x`, 산술평균은 `1.101x`다. C와 .NET의 active deadline·단일 stop token 전송 책임을 맞춘 lifecycle 변경은 public contract·ownership·error semantics와 측정 의미를 유지하므로 채택했다. Sol `Unsafe.SkipInit` A/B throughput ratio는 `59.458% / 67.305% / 84.506% / 85.638% / 93.663% / 82.800%`, 산술평균은 `78.895%`이며 평균 latency ratio 산술평균은 `1.121x`로 lifecycle rebaseline보다 악화되어 제거했다. 최종 aggregate throughput `82.930%`가 .NET simple one-way 목표 `85%`에 미달하므로 `보류`다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_STREAM/tcp

조건: Core `v0.10.1` release package, Release, `tcp`, clients `7000`, duration `1s`, runs `1`, msg sizes `64/256/1024/65536B`, connect concurrency `1024`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 4개 size와 20개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 296575 / 296562 / 275822 / 30296 | 23.562 / 23.553 / 25.284 / 216.987 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_202308_dotnet-multi-stream-tcp-paired-c-7000.txt` |
| .NET baseline | 203676 / 235307 / 204704 / 23541 | 34.595 / 29.733 / 34.364 / 305.146 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_202329_dotnet-multi-stream-tcp-paired-dotnet-7000.txt` |
| .NET own after `Message.Allocate` 직접 작성 | 195329 / 207115 / 204690 / 28915 | 36.209 / 34.102 / 34.544 / 239.599 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_202735_dotnet-multi-stream-tcp-own-after-message-allocate.txt` |
| .NET Sol after EAGAIN 시점 pending wrapper 생성 | 194507 / 222908 / 201877 / 30446 | 36.638 / 31.571 / 35.098 / 223.131 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_203035_dotnet-multi-stream-tcp-sol-after-lazy-pending.txt` |

baseline throughput ratio는 `68.676% / 79.345% / 74.216% / 77.703%`, 산술평균은 `74.985%`이며 평균 latency ratio는 `1.468x / 1.262x / 1.359x / 1.406x`, 산술평균은 `1.374x`다. 자체 `Message.Allocate` 직접 작성 after ratio는 `65.862% / 69.839% / 74.211% / 95.442%`, 산술평균은 `76.338%`, 평균 latency ratio는 `1.537x / 1.448x / 1.366x / 1.104x`, 산술평균은 `1.364x`다. Sol pending wrapper 지연 after ratio는 `65.584% / 75.164% / 73.191% / 100.495%`, 산술평균은 `78.609%`, 평균 latency ratio는 `1.555x / 1.340x / 1.388x / 1.028x`, 산술평균은 `1.328x`다. 두 개선을 적용해도 .NET simple one-way 최소 기준 `85%`에 미달하므로 최종 상태는 `보류`다. public contract·ownership·error semantics는 변경하지 않았다. build는 `0 warning / 0 error`다.

### .NET Multi MULTI_DEALER_DEALER/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 2624033 / 1447344 / 942974 / 438265 / 50201 / 30387 | 4.966 / 0.467 / 14.091 / 39.460 / 295.334 / 399.860 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_203400_dotnet-multi-dealer-dealer-ws-paired-c1.txt` |
| .NET parity baseline | 1074092 / 868551 / 783286 / 402810 / 49440 / 25446 | 71.792 / 0.754 / 3.215 / 37.252 / 289.870 / 378.593 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_203649_dotnet-multi-dealer-dealer-ws-paired-parity-baseline.txt` |
| .NET own 상태 검증 inlining A/B | 1095884 / 815198 / 754132 / 372469 / 51344 / 24222 | 51.878 / 1.637 / 2.516 / 46.886 / 268.204 / 372.416 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_204049_dotnet-multi-dealer-dealer-ws-own-inline-validation.txt` |

parity baseline throughput ratio는 `40.933% / 60.010% / 83.065% / 91.910% / 98.484% / 83.740%`, 산술평균은 `76.357%`이며 평균 latency ratio는 `14.457x / 1.615x / 0.228x / 0.944x / 0.981x / 0.947x`, 산술평균은 `3.195x`다. 자체 상태 검증 inlining A/B throughput ratio는 `41.763% / 56.324% / 79.974% / 84.987% / 102.277% / 79.712%`, 산술평균은 `74.173%`, 평균 latency ratio 산술평균은 `2.860x`다. throughput aggregate가 기준선보다 `2.184%p` 낮아 후보를 제거했다. Sol review는 매 send의 `MessageSocketSendOperation` allocation을 제거하려면 builder pooling·public direct-send·private 우회가 필요하며 public builder·stale reference·ownership 계약과 충돌한다고 판정했다. 최종 상태는 .NET simple one-way 최소 기준 `85%` 미달로 `보류`다. public contract·ownership·error semantics는 변경하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_SENDSEND/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern은 `MULTI_DEALER_ROUTER_SENDSEND`, .NET runner pattern은 `MULTI_DEALER_ROUTER`다. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 165767 / 172435 / 167196 / 142810 / 31377 / 16525 | 0.282 / 0.272 / 0.280 / 0.330 / 1.498 / 2.931 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_204848_dotnet-multi-dealer-router-sendsend-ws-paired-c1.txt` |
| .NET parity baseline | 133891 / 138603 / 140633 / 115913 / 34883 / 16881 | 0.332 / 0.321 / 0.321 / 0.392 / 1.400 / 2.929 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_204921_dotnet-multi-dealer-router-sendsend-ws-paired-baseline.txt` |
| .NET own Received send-operation inlining A/B | 128273 / 137622 / 140525 / 120480 / 35587 / 16682 | 0.347 / 0.324 / 0.320 / 0.375 / 1.369 / 2.963 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_205216_dotnet-multi-dealer-router-sendsend-ws-own-received-send-inline.txt` |
| .NET retry timestamp·sequence parity correction | 140168 / 144616 / 135052 / 123137 / 33363 / 17351 | 0.318 / 0.310 / 0.330 / 0.369 / 1.458 / 2.844 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_205820_dotnet-multi-dealer-router-sendsend-ws-sol-parity-timestamp.txt` |

baseline throughput ratio는 `80.771% / 80.380% / 84.113% / 81.166% / 111.174% / 102.154%`, 산술평균은 `89.960%`이며 평균 latency ratio는 `1.177x / 1.180x / 1.146x / 1.188x / 0.935x / 0.999x`, 산술평균은 `1.104x`다. 자체 inlining A/B throughput ratio는 `77.382% / 79.811% / 84.048% / 84.364% / 113.417% / 100.950%`, 산술평균은 `89.995%`이며 평균 latency ratio 산술평균은 `1.104x`로 효과가 없어 제거했다. Sol review는 추가 binding hotpath 후보를 no-go로 판정하고, C가 blocked retry 때마다 timestamp를 기록하고 성공한 send에서만 sequence를 증가시키는 의미를 확인했다. .NET도 같은 의미로 보정한 뒤 throughput ratio는 `84.557% / 83.867% / 80.775% / 86.224% / 106.329% / 104.998%`, 산술평균은 `91.125%`, 평균 latency ratio는 `1.128x / 1.140x / 1.179x / 1.118x / 0.973x / 0.970x`, 산술평균은 `1.085x`다. 최종 상태는 `통과`다. public contract·ownership·error semantics는 변경하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_SENDSEND/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern은 `MULTI_ROUTER_ROUTER_SENDSEND`, .NET runner pattern은 `ROUTER_ROUTER`다. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 169366 / 175446 / 167910 / 141536 / 32029 / 16333 | 0.279 / 0.269 / 0.282 / 0.338 / 1.470 / 2.964 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_211803_dotnet-multi-router-router-sendsend-ws-paired-c1.txt` |
| .NET parity baseline | 135478 / 143582 / 137091 / 103284 / 32663 / 17332 | 0.331 / 0.313 / 0.327 / 0.436 / 1.490 / 2.849 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_211826_dotnet-multi-router-router-sendsend-ws-paired-baseline.txt` |
| .NET own slot type-cache·sequence parity | 135322 / 142510 / 137586 / 122663 / 34232 / 17307 | 0.332 / 0.314 / 0.326 / 0.374 / 1.420 / 2.848 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_212252_dotnet-multi-router-router-sendsend-ws-own-slot-parity.txt` |
| .NET Sol retry timestamp parity | 138150 / 143081 / 137264 / 120034 / 34547 / 16806 | 0.326 / 0.314 / 0.328 / 0.380 / 1.403 / 2.941 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_212529_dotnet-multi-router-router-sendsend-ws-sol-timestamp-parity.txt` |

baseline throughput ratio는 `79.991% / 81.838% / 81.646% / 72.974% / 101.979% / 106.116%`, 산술평균 `87.424%`이며 평균 latency ratio는 `1.186x / 1.164x / 1.160x / 1.290x / 1.014x / 0.961x`, 산술평균 `1.129x`다. 자체 slot type-cache·sequence parity 후보는 `79.899% / 81.227% / 81.940% / 86.666% / 106.878% / 105.963%`, 산술평균 `90.429%`이며 평균 latency ratio `1.190x / 1.167x / 1.156x / 1.107x / 0.966x / 0.961x`, 산술평균 `1.091x`다. Sol retry timestamp parity 후 최종 ratio는 `81.569% / 81.553% / 81.749% / 84.808% / 107.862% / 102.896%`, 산술평균 `90.073%`이며 평균 latency ratio `1.168x / 1.167x / 1.163x / 1.124x / 0.954x / 0.992x`, 산술평균 `1.095x`다. own 후보는 throughput·latency aggregate를 개선했고, Sol 후보는 C와 blocked retry stamping 의미를 일치시킨다. 최종 상태는 `통과`다. builder pooling·private direct-send·builder 재사용은 public builder/ownership 경계와 충돌해 no-go로 판정했다. public contract·ownership·error semantics는 변경하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_REQREP/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern은 `MULTI_DEALER_ROUTER_REQREP`, .NET runner pattern은 `DEALER_ROUTER_REQREP`다. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 67776 / 67220 / 66849 / 59434 / 17279 / 11283 | 0.519 / 0.520 / 0.524 / 0.606 / 2.011 / 3.017 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_210445_dotnet-multi-dealer-router-reqrep-ws-paired-c1.txt` |
| .NET parity baseline | 42772 / 41707 / 54595 / 48767 / 19618 / 10024 | 0.777 / 0.767 / 0.615 / 0.708 / 2.003 / 4.419 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_210500_dotnet-multi-dealer-router-reqrep-ws-paired-baseline.txt` |
| .NET own slot callback cache | 63724 / 63839 / 59318 / 59156 / 23281 / 14431 | 0.494 / 0.501 / 0.536 / 0.576 / 1.780 / 3.114 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_210703_dotnet-multi-dealer-router-reqrep-ws-own-cached-callback.txt` |
| .NET Sol single-part value-type submitter | 63308 / 66790 / 60482 / 59783 / 21562 / 14533 | 0.506 / 0.484 / 0.519 / 0.554 / 1.886 / 3.137 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_211110_dotnet-multi-dealer-router-reqrep-ws-sol-value-submit.txt` |

baseline throughput ratio는 `63.108% / 62.046% / 81.669% / 82.052% / 113.537% / 88.842%`, 산술평균은 `81.876%`이며 평균 latency ratio는 `1.497x / 1.475x / 1.174x / 1.168x / 0.996x / 1.465x`, 산술평균은 `1.296x`다. 자체 callback cache throughput ratio는 `94.021% / 94.970% / 88.734% / 99.532% / 134.736% / 127.900%`, 산술평균은 `106.649%`이며 평균 latency ratio는 `0.952x / 0.963x / 1.023x / 0.950x / 0.885x / 1.032x`, 산술평균은 `0.968x`다. Sol single-part value-type submitter after throughput ratio는 `93.408% / 99.360% / 90.476% / 100.587% / 124.787% / 128.804%`, 산술평균은 `106.237%`이며 평균 latency ratio는 `0.975x / 0.931x / 0.990x / 0.914x / 0.938x / 1.040x`, 산술평균은 `0.965x`다. own callback cache와 Sol value-type submitter 모두 baseline보다 개선되어 유지한다. Sol review는 callback reuse의 single outstanding·drain·GCHandle 조건과 value-type submitter의 Message move/실패 복원·timeout·backpressure·callback lifetime 보존을 확인했으며, 추가 구조 변경은 no-go로 판정했다. 최종 상태는 `통과`다. public contract·ownership·error semantics는 변경하지 않았다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_REQREP/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern과 .NET runner pattern은 `MULTI_ROUTER_ROUTER_REQREP`다. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 91194 / 87013 / 84478 / 78526 / 21668 / 14446 | 0.387 / 0.403 / 0.410 / 0.456 / 1.463 / 2.434 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_213113_dotnet-multi-router-router-reqrep-ws-paired-c1.txt` |
| .NET parity baseline | 54048 / 53052 / 58731 / 56180 / 23270 / 14269 | 0.549 / 0.555 / 0.509 / 0.580 / 1.728 / 3.183 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_213128_dotnet-multi-router-router-reqrep-ws-paired-baseline.txt` |
| .NET own `RouterRequestSinglePartSubmitter` | 53590 / 49184 / 57604 / 50608 / 24431 / 13032 | 0.587 / 0.648 / 0.510 / 0.621 / 1.602 / 3.433 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_213448_dotnet-multi-router-router-reqrep-ws-own-router-submit.txt` |
| .NET Sol timeout 조회 1회화 | 61338 / 65231 / 56265 / 56819 / 24546 / 14652 | 0.508 / 0.500 / 0.591 / 0.588 / 1.704 / 3.089 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_214143_dotnet-multi-router-router-reqrep-ws-sol-timeout-hoist.txt` |

baseline throughput ratio는 `59.267% / 60.970% / 69.522% / 71.543% / 107.393% / 98.775%`, 산술평균은 `77.912%`이며 평균 latency ratio는 `1.419x / 1.377x / 1.241x / 1.272x / 1.181x / 1.308x`, 산술평균은 `1.300x`다. 자체 `RouterRequestSinglePartSubmitter` 후보 throughput ratio는 `58.765% / 56.525% / 68.188% / 64.447% / 112.752% / 90.212%`, 산술평균은 `75.148%`이며 평균 latency ratio는 `1.517x / 1.608x / 1.244x / 1.362x / 1.095x / 1.410x`, 산술평균은 `1.373x`다. 자체 후보는 baseline보다 악화되어 제거했다.

Sol timeout 조회 1회화 후 throughput ratio는 `67.261% / 74.967% / 66.603% / 72.357% / 113.282% / 101.426%`, 산술평균은 `82.649%`이며 평균 latency ratio는 `1.313x / 1.241x / 1.441x / 1.289x / 1.165x / 1.269x`, 산술평균은 `1.286x`다. run setup에서 `ResolveReqRepTimeout()`을 한 번만 호출하고 client request마다 같은 timeout을 재사용했다. timeout 값·오류 의미·ownership·callback 수명과 public contract는 변경하지 않았다. 최종 상태는 `통과`다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_PUBSUB/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|----------------------------|--------------------------|--------|
| C 기준 | 1001753 / 1126822 / 980444 / 479074 / 52467 / 35855 | 410.941 / 402.939 / 346.237 / 180.906 / 175.677 / 135.117 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_214820_dotnet-multi-pubsub-ws-paired-c1.txt` |
| .NET parity baseline | 706359 / 841749 / 884609 / 417997 / 76484 / 38529 | 462.977 / 486.903 / 456.074 / 272.077 / 129.257 / 124.138 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_214832_dotnet-multi-pubsub-ws-paired-baseline.txt` |
| .NET own capability bypass | 682396 / 869345 / 817276 / 343030 / 72610 / 37447 | 463.254 / 481.065 / 454.811 / 299.538 / 133.734 / 124.199 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_215034_dotnet-multi-pubsub-ws-own-publish-unchecked.txt` |
| .NET Sol retry payload parity | 717345 / 872209 / 788463 / 422390 / 78942 / 35934 | 466.247 / 474.282 / 439.796 / 343.868 / 128.779 / 132.989 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_215358_dotnet-multi-pubsub-ws-sol-retry-payload.txt` |

baseline throughput ratio는 `70.512% / 74.701% / 90.225% / 87.251% / 145.775% / 107.458%`, 산술평균은 `95.987%`이며 평균 latency ratio는 `1.127x / 1.208x / 1.317x / 1.504x / 0.736x / 0.919x`, 산술평균은 `1.135x`다. 자체 `PublishNoWaitUnchecked` capability bypass 후보 throughput ratio는 `68.120% / 77.150% / 83.358% / 71.603% / 138.392% / 104.440%`, 산술평균은 `90.510%`이며 평균 latency ratio는 `1.127x / 1.194x / 1.314x / 1.656x / 0.761x / 0.919x`, 산술평균은 `1.162x`다. capability bypass 후보는 throughput·latency aggregate가 모두 악화되어 제거했다.

Sol retry payload parity 후 throughput ratio는 `71.609% / 77.404% / 80.419% / 88.168% / 150.460% / 100.220%`, 산술평균은 `94.713%`이며 평균 latency ratio는 `1.135x / 1.177x / 1.270x / 1.901x / 0.733x / 0.984x`, 산술평균은 `1.200x`다. logical message마다 metric header의 timestamp와 sequence를 한 번만 기록하고, `DontWait` 실패 뒤 같은 `Message`를 `POLLOUT`까지 재시도하도록 C publish 의미를 맞췄다. 이 parity correction은 수치가 baseline보다 낮아도 측정 의미를 회복하므로 채택했다. public contract·ownership·error semantics는 변경하지 않았으며, 64B·256B 개별 throughput 미달은 aggregate 평균 판정에 영향을 주지 않는 결과 기록이다. 최종 상태는 `통과`다. build는 `0 warning / 0 error`, contract test는 `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_STREAM/ws

조건: Core `v0.10.1` release package, Release, `ws`, clients `7000`, duration `1s`, runs `1`, msg sizes `64/256/1024/65536B`, connect concurrency `1024`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 4개 size와 20개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 216670 / 225385 / 207274 / 7011 | 32.058 / 30.782 / 33.521 / 1328.706 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_215924_dotnet-multi-stream-ws-paired-c-7000.txt` |
| .NET baseline | 151905 / 138734 / 146420 / 7000 | 46.441 / 50.576 / 47.978 / 1599.743 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_215947_dotnet-multi-stream-ws-paired-baseline-7000.txt` |
| .NET 자체 routing-id direct | 159713 / 140975 / 156880 / 7005 | 43.948 / 49.194 / 44.898 / 1342.721 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_220312_dotnet-multi-stream-ws-own-routingid-cache-7000.txt` |
| .NET Sol 기존 Message pool | 165082 / 168775 / 153407 / 7000 | 42.573 / 41.599 / 45.941 / 1517.836 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_220629_dotnet-multi-stream-ws-sol-message-pool-7000.txt` |
| .NET Sol post-helper 후보(원복) | 183215 / 177129 / 130204 / 7000 | 38.317 / 39.933 / 54.053 / 1658.615 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_220900_dotnet-multi-stream-ws-sol-post-helper-7000.txt` |

baseline throughput ratio는 `70.109% / 61.554% / 70.641% / 99.843%`, 산술평균은 `75.537%`이며 평균 latency ratio는 `1.449x / 1.643x / 1.431x / 1.204x`, 산술평균은 `1.432x`다. 자체 routing-id direct 변환 후 ratio는 `73.713% / 62.549% / 75.687% / 99.914%`, 산술평균은 `77.966%`, 평균 latency ratio는 `1.371x / 1.598x / 1.339x / 1.011x`, 산술평균은 `1.330x`다. Sol 기존 Message pool 적용 후 최종 ratio는 `76.191% / 74.883% / 74.012% / 99.843%`, 산술평균은 `81.232%`, 평균 latency ratio는 `1.328x / 1.351x / 1.371x / 1.142x`, 산술평균은 `1.298x`다. post-helper 후보는 throughput 산술평균 `81.452%`로 소폭 높았지만 latency 산술평균이 `1.338x`로 악화되고 1024B throughput이 `15.13%` 낮아 원복했다. 최종 aggregate는 .NET simple one-way 목표 `85%`에 미달하며 추가 contract-safe 후보가 없어 `보류`다. public contract·ownership·error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_STREAM/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `7000`, duration `1s`, runs `1`, msg sizes `64/256/1024/65536B`, connect concurrency `1024`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 4개 size와 20개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 135837 / 89683 / 105897 / 7000 | 51.369 / 76.215 / 65.225 / 2552.476 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_221414_dotnet-multi-stream-wss-paired-c-7000.txt` |
| .NET baseline | 93019 / 83266 / 92453 / 7000 | 76.402 / 84.258 / 76.483 / 2063.079 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_221513_dotnet-multi-stream-wss-paired-baseline-7000.txt` |
| .NET 자체 StreamSendOperation 후보(원복) | 81096 / 96343 / 76268 / 7000 | 85.822 / 73.094 / 94.565 / 2431.833 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_221651_dotnet-multi-stream-wss-own-stream-submit-inline-7000.txt` |

baseline throughput ratio는 `68.478% / 92.845% / 87.305% / 100.000%`, 산술평균은 `87.157%`이며 평균 latency ratio는 `1.487x / 1.106x / 1.173x / 0.808x`, 산술평균은 `1.143x`다. 자체 StreamSendOperation nullable routing-id 제거·AggressiveInlining 후보 ratio는 `59.701% / 107.426% / 72.021% / 100.000%`, 산술평균 `84.787%`, 평균 latency ratio `1.671x / 0.959x / 1.450x / 0.953x`, 산술평균 `1.258x`로 baseline보다 악화되어 원복했다. Sol review는 native routing-id direct 변환·기존 Message pool·lazy pending을 유지하고, packet framing copy는 C wire 의미상 필요하며 public builder 제거는 ownership·stale-reference 경계를 위반하므로 추가 후보 no-go로 판정했다. 최종 aggregate가 simple one-way 목표 `85%`를 충족해 `통과`다. public contract·ownership·error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_STREAM/tls

조건: Core `v0.10.1` release package, Release, `tls`, clients `7000`, duration `1s`, runs `1`, msg sizes `64/256/1024/65536B`, connect concurrency `1024`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 4개 size와 20개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 170040 / 124963 / 121079 / 7859 | 41.507 / 55.063 / 57.531 / 1051.875 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_222055_dotnet-multi-stream-tls-paired-c-7000.txt` |
| .NET baseline | 133840 / 123314 / 127770 / 7009 | 53.206 / 57.214 / 55.580 / 1347.747 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_222326_dotnet-multi-stream-tls-paired-baseline-7000.txt` |
| .NET own dead `StreamSendOperation` 제거 | 117052 / 108696 / 97635 / 8323 | 60.661 / 64.693 / 71.649 / 987.388 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_222621_dotnet-multi-stream-tls-own-dead-stream-op-7000.txt` |

baseline throughput ratio는 `78.711% / 98.680% / 105.526% / 89.184%`, 산술평균은 `93.025%`이며 평균 latency ratio는 `1.282x / 1.039x / 0.966x / 1.281x`, 산술평균은 `1.142x`다. dead `StreamSendOperation` 제거 후 own ratio는 `68.838% / 86.983% / 80.637% / 105.904%`, 산술평균 `85.590%`, 평균 latency ratio `1.461x / 1.175x / 1.245x / 0.939x`, 산술평균 `1.205x`다. 실제 STREAM send 경로가 `RoutedSendOperation` 하나를 사용하고 삭제 대상에 생성·참조가 없으므로, 이 변경은 성능 후보가 아닌 POSDDD 구조 정리로 채택했다. Sol review는 public contract·builder lifetime·ownership·error semantics 영향이 없음을 확인하고 TLS 암호화·framing 비용 외 추가 contract-safe hotpath 후보를 no-go로 판정했다. 최종 aggregate는 simple one-way 목표 `85%`를 충족해 `통과`다. public contract·ownership·error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_DEALER/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C와 .NET report 모두 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2560279 / 1194612 / 712186 / 230046 / 21801 / 13108 | 10.091 / 2.039 / 24.844 / 76.799 / 444.495 / 446.802 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_223240_dotnet-multi-dealer-dealer-wss-paired-c-100.txt` |
| .NET baseline | 1136480 / 778892 / 563668 / 208618 / 17377 / 10888 | 24.366 / 0.564 / 17.374 / 80.792 / 363.480 / 404.671 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_223301_dotnet-multi-dealer-dealer-wss-paired-baseline-100.txt` |
| .NET own send-side pool 제거 | 1230060 / 828505 / 574846 / 212234 / 23684 / 13665 | 16.088 / 0.616 / 20.489 / 79.038 / 332.654 / 404.541 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_223505_dotnet-multi-dealer-dealer-wss-own-unpooled-allocate-100.txt` |

baseline throughput ratio는 `44.389% / 65.200% / 79.146% / 90.685% / 79.707% / 83.064%`, 산술평균은 `73.699%`이며 평균 latency ratio는 `2.415x / 0.277x / 0.699x / 1.052x / 0.818x / 0.906x`, 산술평균은 `1.028x`다. send-side `Message.Allocate` pool 제거 후 ratio는 `48.044% / 69.353% / 80.716% / 92.257% / 108.637% / 104.249%`, 산술평균 `83.876%`, 평균 latency ratio `1.594x / 0.302x / 0.825x / 1.029x / 0.748x / 0.905x`, 산술평균 `0.901x`다. pool 제거는 throughput·latency를 모두 개선했지만 aggregate 목표 `85%`에 `1.124%p` 미달했다. Sol review는 receive 경로 pool 유지와 send-side unpooled allocation을 public contract·ownership·error semantics를 보존하는 변경으로 GO했으며, `MessageSocketSendOperation` builder allocation 제거·reuse·pooling은 independent builder와 stale-reference 경계 때문에 no-go로 판정했다. 추가 contract-safe 후보가 없어 최종 상태는 `보류`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_SENDSEND/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern은 `MULTI_DEALER_ROUTER_SENDSEND`, .NET runner pattern은 `MULTI_DEALER_ROUTER`이며 routed echo 의미로 paired 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 132763 / 135665 / 127099 / 95300 / 13001 / 6864 | 0.361 / 0.353 / 0.376 / 0.508 / 3.803 / 7.163 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_224002_dotnet-multi-dealer-router-sendsend-wss-paired-c-100.txt` |
| .NET baseline | 70603 / 114011 / 107522 / 80697 / 13296 / 7272 | 0.662 / 0.403 / 0.426 / 0.578 / 3.717 / 6.795 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_224033_dotnet-multi-dealer-router-sendsend-wss-paired-baseline-100.txt` |
| .NET own combined validation inlining | 105346 / 112911 / 106311 / 80738 / 13114 / 7188 | 0.434 / 0.405 / 0.432 / 0.579 / 3.766 / 6.841 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_224325_dotnet-multi-dealer-router-sendsend-wss-own-received-inline-100.txt` |

baseline throughput ratio는 `53.180% / 84.039% / 84.597% / 84.677% / 102.269% / 105.944%`, 산술평균은 `85.784%`이며 평균 latency ratio는 `1.834x / 1.142x / 1.133x / 1.138x / 0.977x / 0.949x`, 산술평균은 `1.195x`다. 자체 개선은 `MessageSocketSendOperation`의 private `EnsureReady`/`EnsureNotSubmitted`와 `ReceivedSendOperationImpl`의 `Message`/`Flags`/`Submit`/`EnsureReady`/`EnsureNotSubmitted`에 AggressiveInlining을 적용했다. own throughput ratio는 `79.349% / 83.228% / 83.644% / 84.720% / 100.869% / 104.720%`, 산술평균은 `89.422%`이며 평균 latency ratio는 `1.202x / 1.147x / 1.149x / 1.140x / 0.990x / 0.955x`, 산술평균은 `1.097x`다. Sol review는 두 내부 inlining을 GO하고 추가 public builder allocation 제거 후보를 no-go로 판정했다. public contract·independent builder·ownership·error order는 변경하지 않았으며 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_REQREP/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern과 .NET runner pattern은 `MULTI_DEALER_ROUTER_REQREP`이며 socket request/reply 의미로 paired 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 90349 / 90053 / 83026 / 65584 / 11453 / 6463 | 0.446 / 0.440 / 0.482 / 0.627 / 4.139 / 7.471 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_225239_dotnet-dealer-router-reqrep-wss-paired-c-100.txt` |
| .NET baseline | 48337 / 60157 / 54625 / 45497 / 9541 / 6275 | 0.762 / 0.604 / 0.666 / 0.845 / 5.005 / 7.782 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_225259_dotnet-dealer-router-reqrep-wss-paired-baseline-100.txt` |
| .NET own DealerRequestOperation helper inlining | 55486 / 59052 / 54434 / 43784 / 11778 / 5902 | 0.679 / 0.619 / 0.656 / 0.874 / 4.076 / 8.268 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_225548_dotnet-dealer-router-reqrep-wss-own-dealer-operation-inline-100.txt` |

baseline throughput ratio는 `53.500% / 66.802% / 65.793% / 69.372% / 83.306% / 97.091%`, 산술평균은 `72.644%`이며 평균 latency ratio는 `1.709x / 1.373x / 1.382x / 1.348x / 1.209x / 1.042x`, 산술평균은 `1.344x`다. 자체 개선은 private `DealerRequestOperation.AddMessage`/`EnsureReady`/`EnsureNotSubmitted`에 AggressiveInlining을 적용했다. own throughput ratio는 `61.413% / 65.575% / 65.563% / 66.760% / 102.838% / 91.320%`, 산술평균은 `75.578%`이며 평균 latency ratio는 `1.522x / 1.407x / 1.361x / 1.394x / 0.985x / 1.107x`, 산술평균은 `1.296x`다. Sol review는 변경 유지를 GO하고 callback completion/GCHandle·EAGAIN reply copy 최적화 후보를 no-go로 판정했다. public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았으며 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_SENDSEND/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern은 `MULTI_ROUTER_ROUTER_SENDSEND`, .NET runner pattern은 `MULTI_ROUTER_ROUTER`이며 routed echo 의미로 paired 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 127986 / 124283 / 129625 / 92983 / 12034 / 7386 | 0.377 / 0.387 / 0.372 / 0.523 / 4.106 / 6.687 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_230056_dotnet-router-router-sendsend-wss-paired-c-100.txt` |
| .NET baseline | 104411 / 113865 / 106619 / 80248 / 13249 / 6784 | 0.440 / 0.404 / 0.431 / 0.579 / 3.722 / 7.267 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_230117_dotnet-router-router-sendsend-wss-paired-baseline-100.txt` |
| .NET own RouterRequestOperation helper inlining (원복) | 101453 / 112255 / 108302 / 83088 / 12834 / 6969 | 0.452 / 0.408 / 0.425 / 0.559 / 3.845 / 7.070 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_230235_dotnet-router-router-sendsend-wss-own-router-operation-inline-100.txt` |

baseline throughput ratio는 `81.580% / 91.618% / 82.252% / 86.304% / 110.096% / 91.849%`, 산술평균은 `90.617%`이며 평균 latency ratio는 `1.167x / 1.044x / 1.159x / 1.107x / 0.906x / 1.087x`, 산술평균은 `1.078x`다. 자체 개선은 private `RouterRequestOperation.AddMessage`/`EnsureReady`/`EnsureNotSubmitted`에 AggressiveInlining을 적용했다. own throughput ratio는 `79.269% / 90.322% / 83.550% / 89.358% / 106.648% / 94.354%`, 산술평균은 `90.584%`이며 평균 latency ratio는 `1.199x / 1.054x / 1.142x / 1.069x / 0.936x / 1.057x`, 산술평균은 `1.076x`다. own throughput이 baseline보다 소폭 낮아 후보는 원복했다. Sol review는 추가 builder/callback/native submit 경계 후보를 no-go로 판정했다. public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았으며 baseline 기준 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_DEALER/tls

조건: Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C와 .NET report는 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1934452 / 1256523 / 840810 / 354615 / 33719 / 17955 | 84.092 / 0.688 / 19.943 / 49.293 / 380.722 / 462.950 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_232836_dotnet-dealer-dealer-tls-paired-c-100.txt` |
| .NET baseline | 992653 / 770942 / 731305 / 310912 / 34695 / 14603 | 79.952 / 0.555 / 8.116 / 52.685 / 330.201 / 411.720 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_232858_dotnet-dealer-dealer-tls-paired-baseline-100.txt` |
| .NET own Message.MoveTo inlining | 1167865 / 859366 / 745359 / 313796 / 35142 / 18941 | 43.650 / 0.413 / 1.303 / 48.534 / 307.825 / 380.782 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_233108_dotnet-dealer-dealer-tls-own-moveto-inline-100.txt` |

baseline throughput ratio는 `51.314% / 61.355% / 86.976% / 87.676% / 102.895% / 81.331%`, 산술평균 `78.591%`이며 평균 latency ratio는 `0.951x / 0.807x / 0.407x / 1.069x / 0.867x / 0.889x`, 산술평균 `0.832x`다. `Message.MoveTo` AggressiveInlining 후 own throughput ratio는 `60.372% / 68.392% / 88.648% / 88.489% / 104.220% / 105.492%`, 산술평균 `85.935%`, 평균 latency ratio는 `0.519x / 0.600x / 0.065x / 0.985x / 0.809x / 0.823x`, 산술평균 `0.633x`다. Sol review는 `EnsureValid → destination 초기화 → native move → source 무효화`와 실패 시 `RestoreFrom` 동작이 동일함을 확인하고 변경을 GO했다. `SinglePartSubmit`·native submitter는 이미 inline이며 `RestoreFrom` 및 추가 pool/ownership 전이 후보는 no-go다. public contract·ownership·error semantics는 변경하지 않았다. 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_SENDSEND/tls

조건: C semantic pattern `MULTI_DEALER_ROUTER_SENDSEND`, .NET runner pattern `MULTI_DEALER_ROUTER`, Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C와 .NET report는 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 145689 / 134066 / 143556 / 119283 / 17848 / 9284 | 0.323 / 0.349 / 0.331 / 0.402 / 2.756 / 5.277 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_233427_dotnet-dealer-router-sendsend-tls-paired-c-100.txt` |
| .NET baseline | 112245 / 123930 / 124972 / 102199 / 17751 / 9735 | 0.400 / 0.365 / 0.365 / 0.450 / 2.782 / 5.079 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_233453_dotnet-dealer-router-sendsend-tls-paired-baseline-100.txt` |
| .NET own Received.Send inlining | 127557 / 125612 / 119795 / 104284 / 17850 / 9779 | 0.355 / 0.359 / 0.378 / 0.441 / 2.766 / 5.046 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_233648_dotnet-dealer-router-sendsend-tls-own-received-send-inline-100.txt` |

baseline throughput ratio는 `77.044% / 92.440% / 87.055% / 85.678% / 99.457% / 104.858%`, 산술평균 `91.088%`이며 평균 latency ratio는 `1.238x / 1.046x / 1.103x / 1.119x / 1.009x / 0.962x`, 산술평균 `1.080x`다. `Received.Send()` AggressiveInlining 후 own throughput ratio는 `87.554% / 93.694% / 83.448% / 87.426% / 100.011% / 105.332%`, 산술평균 `92.911%`, 평균 latency ratio는 `1.099x / 1.029x / 1.142x / 1.097x / 1.004x / 0.956x`, 산술평균 `1.054x`다. Sol review는 독립적인 `ReceivedSendOperationImpl` 생성과 builder lifetime이 유지됨을 확인하고 변경을 GO했다. `Received.SendCore`/`SendReceivedSingle` 확대와 builder 재사용·private bypass 후보는 no-go다. public contract·ownership·error semantics는 변경하지 않았다. 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_DEALER_ROUTER_REQREP/tls

조건: Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C와 .NET report는 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 89411 / 85492 / 89105 / 77963 / 14626 / 8788 | 0.426 / 0.437 / 0.431 / 0.497 / 3.005 / 5.389 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_234008_dotnet-dealer-router-reqrep-tls-paired-c-100.txt` |
| .NET baseline | 59792 / 60658 / 58928 / 50257 / 14525 / 7799 | 0.591 / 0.586 / 0.589 / 0.710 / 3.200 / 6.197 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_234029_dotnet-dealer-router-reqrep-tls-paired-baseline-100.txt` |
| .NET own DealerSocket.Request inlining | 61758 / 44314 / 59438 / 54144 / 14544 / 7991 | 0.574 / 0.858 / 0.588 / 0.664 / 3.198 / 6.044 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_234209_dotnet-dealer-router-reqrep-tls-own-dealer-request-inline-100.txt` |

baseline throughput ratio는 `66.873% / 70.952% / 66.133% / 64.463% / 99.309% / 88.746%`, 산술평균 `76.079%`이며 평균 latency ratio는 `1.387x / 1.341x / 1.367x / 1.429x / 1.065x / 1.150x`, 산술평균 `1.290x`다. `DealerSocket.Request()` AggressiveInlining 후 own throughput ratio는 `69.072% / 51.834% / 66.706% / 69.448% / 99.439% / 90.931%`, 산술평균 `74.572%`, 평균 latency ratio는 `1.347x / 1.963x / 1.364x / 1.336x / 1.064x / 1.122x`, 산술평균 `1.366x`다. throughput·latency 모두 악화되어 후보는 원복했다. Sol review는 slot callback 재사용·timeout 사전 계산·single-part submitter·기존 helper inlining을 확인하고 callback completion/GCHandle·builder 재사용·pooling 후보를 no-go로 판정했다. socket request/reply 기준과 latency 조건을 충족해 최종 상태는 `통과`다. public contract·ownership·error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_SENDSEND/tls

조건: C semantic pattern `MULTI_ROUTER_ROUTER_SENDSEND`, .NET runner pattern `MULTI_ROUTER_ROUTER`, Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C와 .NET report는 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 135859 / 127631 / 143510 / 121079 / 17160 / 8084 | 0.351 / 0.371 / 0.332 / 0.397 / 2.873 / 6.039 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_234535_dotnet-router-router-sendsend-tls-paired-c-100.txt` |
| .NET baseline | 127337 / 108276 / 119977 / 102685 / 15946 / 9234 | 0.357 / 0.418 / 0.379 / 0.452 / 3.069 / 5.352 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_234555_dotnet-router-router-sendsend-tls-paired-baseline-100.txt` |
| .NET own `RoutedSendOperation` validation inlining | 116434 / 119034 / 120271 / 101072 / 17744 / 8361 | 0.388 / 0.378 / 0.378 / 0.453 / 2.780 / 5.771 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_235137_dotnet-router-router-sendsend-tls-own-routed-validation-inline-100.txt` |
| .NET own `Send(RoutingId)` factory inlining | 125869 / 121947 / 83567 / 101084 / 18350 / 9498 | 0.360 / 0.370 / 0.533 / 0.457 / 2.672 / 5.200 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_235403_dotnet-router-router-sendsend-tls-own-routed-send-factory-inline-100.txt` |

baseline throughput ratio는 `93.727% / 84.835% / 83.602% / 84.808% / 92.925% / 114.226%`, 산술평균 `92.354%`이며 평균 latency ratio는 `1.017x / 1.127x / 1.142x / 1.139x / 1.068x / 0.886x`, 산술평균 `1.063x`다. 자체 `RoutedSendOperation.EnsureReady`/`EnsureNotSubmitted` inlining 후 own throughput ratio는 `85.702% / 93.264% / 83.807% / 83.476% / 103.403% / 103.427%`, 산술평균 `92.180%`, 평균 latency ratio `1.055x`로 throughput이 낮아 원복했다. Sol 2차 `RoutedMessageSocketBase.Send(RoutingId)` inlining 후 own throughput ratio는 `92.647% / 95.547% / 58.231% / 83.486% / 106.935% / 117.491%`, 산술평균 `92.389%`, 평균 latency ratio `1.095x`로 1024B throughput과 latency가 악화되어 원복했다. Sol review는 추가 builder allocation·pooling·private bypass 후보를 no-go로 판정했다. baseline을 최종 채택하며 public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았다. 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_ROUTER_ROUTER_REQREP/tls

조건: C semantic pattern `ROUTER_ROUTER_REQREP`, .NET runner pattern `MULTI_ROUTER_ROUTER`, Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. 전체 size를 한 프로세스에서 실행한 C full sweep는 `malloc_consolidate(): unaligned fastbin chunk detected`로 partial report만 남아 공식 비교에서 제외했다. 이후 6개 size를 각각 별도 프로세스로 실행했고 각 report가 complete다. perf는 한 번에 하나만 실행했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 88255 / 93158 / 90547 / 78228 / 15195 / 8988 | 0.423 / 0.406 / 0.418 / 0.488 / 2.918 / 5.235 | 64B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000254_dotnet-router-router-reqrep-tls-c-64-repro-100.txt`; 256B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000228_dotnet-router-router-reqrep-tls-c-256-repro-100.txt`; 1024B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000304_dotnet-router-router-reqrep-tls-c-1024-repro-100.txt`; 4096B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000312_dotnet-router-router-reqrep-tls-c-4096-repro-100.txt`; 65536B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000320_dotnet-router-router-reqrep-tls-c-65536-repro-100.txt`; 131072B `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_000328_dotnet-router-router-reqrep-tls-c-131072-repro-100.txt` |
| .NET baseline | 56770 / 63753 / 57566 / 52050 / 14578 / 8628 | 0.620 / 0.538 / 0.575 / 0.682 / 3.174 / 5.602 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260811_000346_dotnet-router-router-reqrep-tls-paired-baseline-100.txt` |
| .NET own `RouterSocket.Request(RoutingId)` inlining | 57747 / 41003 / 55113 / 51892 / 13469 / 7544 | 0.583 / 0.670 / 0.590 / 0.703 / 3.437 / 6.345 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260811_000530_dotnet-router-router-reqrep-tls-own-router-request-factory-inline-100.txt` |

baseline throughput ratio는 `65.432% / 68.435% / 63.576% / 66.536% / 95.939% / 95.995%`, 산술평균 `75.801%`이며 평균 latency ratio는 `1.466x / 1.325x / 1.376x / 1.398x / 1.088x / 1.070x`, 산술평균 `1.287x`다. `RouterSocket.Request(RoutingId)` AggressiveInlining 후 own throughput ratio는 `65.432% / 44.014% / 60.867% / 66.334% / 88.641% / 83.934%`, 산술평균 `68.204%`, 평균 latency ratio `1.378x`다. throughput·latency가 모두 악화되어 후보는 원복했다. Sol review `SOL-DOTNET-MULTI-ROUTER-ROUTER-REQREP-TLS-20260811`는 후보를 NO-GO하고 추가 final candidate 없이 baseline 종료를 승인했다. callback completion/GCHandle·reply copy·기존 builder/submitter 경계는 request/reply ownership과 retry 의미상 유지해야 하므로 추가 변경하지 않았다. 최종 상태는 `통과`다. public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Multi MULTI_PUBSUB/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C full sweep는 64·256·1024·4096·65536B result line이 완료됐고, 131072B는 같은 옵션의 별도 retry report가 완료됐다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1120257 / 1362746 / 869358 / 227519 / 22509 / 13514 | 406.557 / 373.086 / 215.317 / 139.114 / 136.275 / 198.317 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_231111_dotnet-pubsub-wss-paired-c-100.txt`; 131072B retry: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_231146_dotnet-pubsub-wss-paired-c-131072-retry-100.txt` |
| .NET baseline | 687083 / 796389 / 522902 / 247372 / 26889 / 12816 | 472.199 / 474.273 / 280.965 / 193.136 / 178.720 / 195.703 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_231157_dotnet-pubsub-wss-paired-baseline-100.txt` |
| .NET own Publisher validation inlining | 675719 / 847922 / 584235 / 210325 / 17722 / 9784 | 465.554 / 463.949 / 318.339 / 167.659 / 134.135 / 203.589 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_231301_dotnet-pubsub-wss-own-publisher-validation-inline-100.txt` |

baseline throughput ratio는 `61.333% / 58.440% / 60.148% / 108.726% / 119.459% / 94.835%`, 산술평균 `83.823%`이며 평균 latency ratio는 `1.161x / 1.271x / 1.305x / 1.388x / 1.311x / 0.987x`, 산술평균 `1.237x`다. 자체 `PublisherSendOperation.EnsureReady`/`EnsureNotSubmitted` AggressiveInlining 후 throughput ratio는 `60.318% / 62.222% / 67.203% / 92.443% / 78.733% / 72.399%`, 산술평균 `72.220%`, 평균 latency ratio는 `1.145x / 1.244x / 1.478x / 1.205x / 0.984x / 1.027x`, 산술평균 `1.181x`다. throughput 악화로 자체 후보는 원복했다. Sol review는 topic cache·기존 Message pool 유지와 builder allocation 제거·submit 경계 후보 no-go를 확인했다. 추가 contract-safe 후보가 없어 aggregate throughput 목표 `85%` 미달 상태를 `보류`한다. public contract·ownership·error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### .NET Single 누락 개선 검토 재측정 (2026-08-11)

조건: Core `v0.10.1` release package, Release, `duration=1s`, `runs=1`, msg sizes `64/256/1024/65536/131072/262144B`, transport/pattern 단일 실행. 아래 ratio는 각 C report 대비 현재 .NET report의 throughput ratio이며, latency는 평균 latency ratio다. `RoutedSendOperation` validation inlining A/B는 `ws/DEALER_ROUTER`에서 throughput `85.020% → 80.271%`, latency `1.150x → 1.187x`로 악화되어 원복했다. Sol review submission `019fec52-3729-7462-bb04-9a60d002c5c6`는 Message one-way·PUBSUB·routed one-way 추가 후보를 NO-GO로 판정했다.

| 대상 | C report | .NET re-review report | throughput ratio (64/256/1024/65536/131072/262144) | 평균 | latency 평균 | 결과 |
|------|----------|-----------------------|------------------------------------------------------|------|--------------|------|
| `tcp/DEALER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135529_dotnet-dealer-router-tcp-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004138_re-review-current-tcp-dealer-router.txt` | 60.627% / 78.990% / 81.023% / 82.980% / 89.303% / 84.702% | 79.604% | 0.834x | 통과·routed one-way 추가 후보 NO-GO |
| `tcp/DEALER_ROUTER_REQREP` | 기존 계획의 C 공식 경로는 현재 results 디렉터리에서 확인되지 않음 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004934_re-review-sol-reply-inline-tcp-dealer-router-reqrep.txt` | 공식 ratio 67.990% / 68.490% / 73.790% / 111.350% / 108.830% / 109.940% 유지 | 90.060% | 1.012x | 통과·reply helper inlining 유지·이번 C paired ratio는 재계산하지 않음 |
| `tcp/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145246_router-router-reqrep-tcp-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004941_re-review-sol-reply-inline-tcp-router-router-reqrep.txt` | 51.595% / 54.617% / 52.272% / 89.814% / 91.298% / 90.336% | 71.655% | 1.318x | 통과·reply helper inlining 유지 |
| `ws/PAIR` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145939_dotnet-pair-ws-c-paired.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004200_re-review-current-ws-pair.txt` | 51.764% / 62.300% / 88.308% / 84.687% / 86.602% / 84.177% | 76.306% | 1.262x | 통과·Message one-way 추가 후보 NO-GO |
| `ws/DEALER_DEALER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151412_dotnet-dealer-dealer-ws-c-paired.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004207_re-review-current-ws-dealer-dealer.txt` | 56.648% / 58.111% / 92.701% / 85.595% / 90.457% / 92.108% | 79.270% | 1.168x | 통과·Message one-way 추가 후보 NO-GO |
| `ws/DEALER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151702_dotnet-dealer-router-ws-c-paired.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004214_re-review-current-ws-dealer-router.txt` | 57.911% / 65.688% / 80.399% / 78.931% / 85.853% / 90.541% | 76.554% | 1.135x | 통과·routed one-way 추가 후보 NO-GO |
| `ws/ROUTER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_152401_router-router-ws-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004222_re-review-current-ws-router-router.txt` | 31.532% / 57.644% / 82.269% / 81.447% / 87.462% / 89.942% | 71.716% | 1.489x | 통과·routed one-way 추가 후보 NO-GO |
| `ws/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153631_router-router-reqrep-ws-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004948_re-review-sol-reply-inline-ws-router-router-reqrep.txt` | 54.860% / 62.189% / 83.325% / 89.742% / 93.523% / 93.013% | 79.442% | 1.130x | 통과·reply helper inlining 유지 |
| `wss/PAIR` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153917_pair-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004251_re-review-current-wss-pair.txt` | 63.726% / 76.040% / 86.112% / 82.282% / 86.624% / 87.515% | 80.383% | 1.173x | 통과·Message one-way 추가 후보 NO-GO |
| `wss/PUBSUB` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154100_pubsub-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004258_re-review-current-wss-pubsub.txt` | 67.800% / 76.430% / 89.546% / 89.132% / 88.794% / 84.881% | 82.764% | 1.126x | 통과·PUBSUB 추가 후보 NO-GO |
| `wss/DEALER_DEALER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154250_dealer-dealer-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004336_re-review-current-wss-dealer-dealer.txt` | 65.341% / 82.034% / 96.684% / 92.572% / 90.722% / 81.965% | 84.886% | 1.209x | 통과·Message one-way 추가 후보 NO-GO |
| `wss/DEALER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154433_dealer-router-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004343_re-review-current-wss-dealer-router.txt` | 70.576% / 80.426% / 93.474% / 89.888% / 89.624% / 88.456% | 85.407% | 1.138x | 통과·routed one-way 추가 후보 NO-GO |
| `wss/DEALER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155235_dealer-router-reqrep-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004955_re-review-sol-reply-inline-wss-dealer-router-reqrep.txt` | 51.118% / 68.329% / 82.023% / 86.321% / 90.773% / 90.205% | 78.128% | 1.211x | 통과·reply helper inlining 유지 |
| `wss/ROUTER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155438_router-router-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004358_re-review-current-wss-router-router.txt` | 59.075% / 79.615% / 95.796% / 91.563% / 89.244% / 80.595% | 82.648% | 1.075x | 통과·routed one-way 추가 후보 NO-GO |
| `wss/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155632_router-router-reqrep-wss-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005002_re-review-sol-reply-inline-wss-router-router-reqrep.txt` | 54.911% / 63.598% / 75.747% / 87.890% / 88.949% / 84.690% | 75.964% | 1.228x | 통과·reply helper inlining 유지 |
| `tls/DEALER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162838_dotnet-dealer-router-reqrep-tls-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005009_re-review-sol-reply-inline-tls-dealer-router-reqrep.txt` | 47.128% / 54.181% / 73.020% / 84.313% / 80.867% / 91.712% | 71.870% | 1.331x | 통과·reply helper inlining 유지 |
| `tls/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_163419_dotnet-router-router-reqrep-tls-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005017_re-review-sol-reply-inline-tls-router-router-reqrep.txt` | 46.319% / 54.054% / 57.232% / 84.000% / 87.747% / 88.979% | 69.722% | 1.361x | 통과·reply helper inlining 유지 |
| `inproc/PUBSUB` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_164932_dotnet-pubsub-inproc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004427_re-review-current-inproc-pubsub.txt` | 77.209% / 64.472% / 60.314% / 22.868% / 64.066% / 71.015% | 59.991% | 2.233x | 통과·PUBSUB 추가 후보 NO-GO |
| `inproc/DEALER_DEALER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165225_dotnet-dealer-dealer-inproc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004440_re-review-current-inproc-dealer-dealer.txt` | 52.191% / 57.963% / 54.223% / 21.799% / 53.170% / 72.979% | 52.054% | 2.011x | 통과·Message one-way 추가 후보 NO-GO |
| `inproc/DEALER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165429_dotnet-dealer-router-inproc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004447_re-review-current-inproc-dealer-router.txt` | 71.144% / 58.374% / 54.689% / 54.104% / 43.740% / 65.840% | 57.982% | 1.871x | 통과·routed one-way 추가 후보 NO-GO |
| `inproc/DEALER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_165648_dotnet-dealer-router-reqrep-inproc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005039_re-review-sol-reply-inline-inproc-dealer-router-reqrep.txt` | 41.608% / 48.275% / 42.730% / 75.327% / 92.268% / 88.987% | 64.866% | 1.698x | 보류·reply helper inlining 유지·request/reply aggregate 70% 미달 |
| `inproc/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_171326_dotnet-router-router-reqrep-inproc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005046_re-review-sol-reply-inline-inproc-router-router-reqrep.txt` | 46.194% / 45.348% / 43.092% / 75.376% / 89.927% / 91.203% | 65.190% | 1.672x | 보류·reply helper inlining 유지·request/reply aggregate 70% 미달 |
| `ipc/PAIR` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_175801_dotnet-pair-ipc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004455_re-review-current-ipc-pair.txt` | 43.584% / 58.984% / 67.081% / 79.946% / 81.291% / 86.646% | 69.589% | 1.656x | 통과·Message one-way 추가 후보 NO-GO |
| `ipc/DEALER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_182021_dotnet-dealer-router-reqrep-ipc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004856_re-review-sol-reply-inline-ipc.txt` | 53.134% / 61.358% / 57.179% / 97.854% / 94.137% / 99.132% | 77.132% | 1.254x | 통과·reply helper inlining 유지 |
| `ipc/ROUTER_ROUTER` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_175017_dotnet-router-router-ipc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_004509_re-review-current-ipc-router-router.txt` | 44.286% / 62.191% / 69.691% / 78.376% / 82.782% / 84.452% | 70.296% | 1.493x | 통과·routed one-way 추가 후보 NO-GO |
| `ipc/ROUTER_ROUTER_REQREP` | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_172657_dotnet-router-router-reqrep-ipc-paired-c1.txt` | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260811_005024_re-review-sol-reply-inline-ipc-router-router-reqrep.txt` | 38.069% / 49.941% / 46.453% / 79.557% / 78.156% / 88.285% | 63.410% | 1.568x | 통과·reply helper inlining 유지 |

`ReceivedReplyOperationImpl.EnsureReady`/`EnsureNotSubmitted` A/B는 `ipc/DEALER_ROUTER_REQREP`에서 current baseline 대비 throughput `123.346%`, latency `0.804x`였고, Sol review GO 후 유지했다. 이 변경은 `submitted 검사 → non-empty 검사 → reply submit → 성공 후 submitted 표시` 순서를 유지하며 public contract, Message ownership, retry/error semantics를 변경하지 않는다. Release build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`, samples `7 passed / 0 failed`다.

### .NET Multi MULTI_PUBSUB/tls

조건: Core `v0.10.1` release package, Release, `tls`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`다. C와 .NET report는 6개 size와 30개 result line이 complete다.

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1091620 / 1310878 / 1033471 / 380915 / 28742 / 17505 | 409.054 / 406.597 / 345.450 / 196.042 / 168.965 / 161.046 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_001824_dotnet-pubsub-tls-paired-c-100.txt` |
| .NET baseline | 698915 / 906496 / 664782 / 222515 / 25074 / 8518 | 457.623 / 469.661 / 414.368 / 335.520 / 166.168 / 235.984 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260811_001838_dotnet-pubsub-tls-paired-baseline-100.txt` |
| .NET own `PublisherSendOperation` validation inlining | 692775 / 870673 / 805416 / 362331 / 38455 / 17651 | 469.667 / 480.440 / 401.232 / 261.812 / 134.683 / 128.185 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260811_001957_dotnet-pubsub-tls-own-publisher-validation-inline-100.txt` |

baseline throughput ratio는 `64.025% / 69.152% / 64.325% / 58.416% / 87.238% / 48.660%`, 산술평균 `65.303%`이며 평균 latency ratio는 `1.119x / 1.155x / 1.200x / 1.711x / 0.983x / 1.465x`, 산술평균 `1.272x`다. `PublisherSendOperation.EnsureReady`/`EnsureNotSubmitted` AggressiveInlining 후 own throughput ratio는 `63.463% / 66.419% / 77.933% / 95.121% / 133.794% / 100.834%`, 산술평균 `89.594%`, 평균 latency ratio `1.070x`다. baseline 대비 throughput과 latency가 개선되어 후보를 유지하고 aggregate throughput 기준을 통과한다. Sol review `SOL-DOTNET-MULTI-PUBSUB-TLS-20260811`는 GO하고 `EnsureNotSubmitted → non-empty 검사 → submitted 표시 → publish` 순서, topic validation, Message ownership과 retry 의미가 유지됨을 확인했다. 추가 builder/public submit 경계 후보는 NO-GO다. 최종 상태는 `통과`다. public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.


### .NET Multi MULTI_ROUTER_ROUTER_REQREP/wss

조건: Core `v0.10.1` release package, Release, `wss`, clients `100`, duration `1s`, runs `1`, msg sizes `64/256/1024/4096/65536/131072B`, connect concurrency `128`, connect-ready timeout `10000ms`, monitor-HWM `4096000`, auto-HWM `balanced`, server/client I/O threads `4/4`. C semantic pattern과 .NET runner pattern은 `MULTI_ROUTER_ROUTER_REQREP`이며 socket request/reply 의미로 paired 측정했다.

| 구분 | size별 throughput (Kops/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 82119 / 66736 / 83959 / 66375 / 11297 / 6967 | 0.478 / 0.576 / 0.470 / 0.617 / 4.190 / 6.964 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_230625_dotnet-router-router-reqrep-wss-paired-c-100.txt` |
| .NET baseline | 51374 / 55216 / 44441 / 47554 / 11823 / 5743 | 0.711 / 0.632 / 0.627 / 0.798 / 4.026 / 8.415 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_230645_dotnet-router-router-reqrep-wss-paired-baseline-100.txt` |
| .NET own RouterRequestOperation helper inlining | 57803 / 55918 / 56454 / 47068 / 11961 / 6128 | 0.603 / 0.623 / 0.629 / 0.798 / 3.966 / 7.946 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260810_230807_dotnet-router-router-reqrep-wss-own-router-operation-inline-100.txt` |

baseline throughput ratio는 `62.560% / 82.738% / 52.932% / 71.644% / 104.656% / 82.432%`, 산술평균은 `76.160%`이며 평균 latency ratio는 `1.487x / 1.097x / 1.334x / 1.293x / 0.961x / 1.208x`, 산술평균은 `1.230x`다. 자체 개선은 private `RouterRequestOperation.AddMessage`/`EnsureReady`/`EnsureNotSubmitted`에 AggressiveInlining을 적용했다. own throughput ratio는 `70.389% / 83.790% / 67.240% / 70.912% / 105.878% / 87.958%`, 산술평균은 `81.028%`이며 평균 latency ratio는 `1.262x / 1.082x / 1.338x / 1.293x / 0.947x / 1.141x`, 산술평균은 `1.177x`다. baseline 대비 throughput과 latency가 개선됐다. Sol review는 변경 유지를 GO하고 추가 builder/callback/native submit 경계 후보를 no-go로 판정했다. public contract·builder state·Message ownership·exception/error semantics는 변경하지 않았으며 최종 상태는 `통과`다. build `0 warning / 0 error`, contract test `149 passed / 0 failed / 0 skipped`다.

### Python Single PAIR/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2396.815 / 1211.775 / 666.060 / 39.687 / 23.774 / 15.644 | 0.104 / 0.239 / 0.311 / 5.122 / 8.605 / 13.190 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_010532_python-pair-tcp-native-builder-c.txt` |
| Python baseline | 120.962 / 118.698 / 113.169 / 39.257 / 20.468 / 13.010 | 0.117 / 0.096 / 0.123 / 3.385 / 10.309 / 20.178 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_010229_python-pair-tcp-paired-baseline.txt` |
| Python final | 132.816 / 126.421 / 125.324 / 36.295 / 22.118 / 13.654 | 0.107 / 0.112 / 0.129 / 0.675 / 8.214 / 17.708 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_010548_python-pair-tcp-native-builder-after.txt` |

final throughput ratio는 `5.541% / 10.433% / 18.816% / 91.453% / 93.034% / 87.279%`, 산술평균 `51.093%`이며 평균 latency ratio 산술평균은 `0.724x`다. 단순 one-way 최소 기준 `35%`를 통과한다. baseline throughput ratio 산술평균은 `48.824%`였고, final은 이를 초과한다.

### Python Single PUBSUB/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1534.761 / 970.271 / 589.128 / 38.404 / 25.369 / 13.430 | 14.985 / 0.254 / 0.339 / 5.304 / 8.080 / 15.160 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_011037_python-pubsub-tcp-publisher-op-c.txt` |
| Python baseline | 110.280 / 110.713 / 118.244 / 34.624 / 21.033 / 13.597 | 0.109 / 0.109 / 0.149 / 1.030 / 5.718 / 17.163 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_010804_python-pubsub-tcp-baseline.txt` |
| Python final | 121.183 / 131.444 / 118.667 / 36.950 / 21.791 / 13.484 | 0.164 / 0.166 / 0.188 / 1.364 / 7.148 / 16.532 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011110_python-pubsub-tcp-publisher-op-after.txt` |

final throughput ratio는 `7.895% / 13.547% / 20.143% / 96.214% / 85.896% / 100.402%`, 산술평균 `54.016%`이며 평균 latency ratio 산술평균은 `0.575x`다. 단순 one-way 최소 기준 `35%`를 통과한다. baseline throughput ratio 산술평균은 `49.055%`였고, final은 이를 초과한다.

### Python Single DEALER_DEALER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2519.274 / 1245.609 / 647.889 / 38.928 / 23.716 / 14.679 | 2.453 / 0.230 / 0.624 / 5.223 / 8.613 / 14.031 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_010946_python-dealer-dealer-tcp-paired-c.txt` |
| Python final | 129.526 / 132.554 / 127.227 / 34.084 / 19.719 / 8.195 | 0.126 / 0.142 / 0.148 / 5.358 / 10.561 / 27.318 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011003_python-dealer-dealer-tcp-baseline.txt` |

throughput ratio는 `5.141% / 10.641% / 19.637% / 87.558% / 83.146% / 55.828%`, 산술평균 `43.658%`이며 평균 latency ratio 산술평균은 `0.851x`다. 단순 one-way 최소 기준 `35%`를 통과한다.

### Python Single DEALER_ROUTER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2569.233 / 1248.673 / 762.251 / 40.211 / 25.362 / 16.061 | 15.508 / 0.211 / 0.273 / 5.068 / 8.160 / 12.950 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_011554_python-dealer-router-tcp-router-owner-c.txt` |
| Python baseline | 17.651 / 18.189 / 17.094 / 3.250 / 2.153 / 0.642 | 4.535 / 13.854 / 14.829 / 167.103 / 197.077 / 394.577 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011342_python-dealer-router-tcp-baseline.txt` |
| Python final | 128.860 / 117.833 / 121.766 / 32.814 / 20.113 / 8.741 | 0.128 / 0.131 / 0.134 / 1.989 / 10.833 / 25.789 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011614_python-dealer-router-tcp-router-owner-after.txt` |

final throughput ratio는 `5.016% / 9.437% / 15.975% / 81.605% / 79.304% / 54.424%`, 산술평균 `40.960%`이며 평균 latency ratio 산술평균은 `0.805x`다. routed one-way 최소 기준 `33%`를 통과한다. baseline throughput ratio 산술평균은 `4.188%`였다.

### Python Single ROUTER_ROUTER/tcp

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 2343.665 / 1247.780 / 669.192 / 39.831 / 26.393 / 17.188 | 0.169 / 0.256 / 0.334 / 5.120 / 7.838 / 12.221 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_011732_python-router-router-tcp-paired-c.txt` |
| Python final | 123.092 / 117.575 / 110.237 / 39.547 / 20.062 / 9.398 | 0.142 / 0.121 / 0.168 / 0.653 / 10.912 / 24.919 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011749_python-router-router-tcp-baseline.txt` |

throughput ratio는 `5.252% / 9.423% / 16.473% / 99.287% / 76.013% / 54.678%`, 산술평균 `43.521%`이며 평균 latency ratio 산술평균은 `0.896x`다. routed one-way 최소 기준 `33%`를 통과한다.

### Python Single PAIR/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1407.346 / 1012.526 / 412.278 / 22.526 / 14.981 / 9.491 | 46.655 / 27.941 / 21.860 / 13.552 / 13.336 / 20.962 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_011932_python-pair-ws-paired-c.txt` |
| Python final | 134.864 / 131.846 / 135.625 / 23.441 / 15.715 / 8.842 | 0.121 / 0.117 / 0.164 / 15.197 / 16.616 / 28.067 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_011949_python-pair-ws-current.txt` |

throughput ratio는 `9.583% / 13.021% / 32.896% / 104.062% / 104.900% / 93.162%`, 산술평균 `59.604%`이며 평균 latency ratio 산술평균은 `0.620x`다. 단순 one-way 최소 기준 `35%`를 통과한다.

### Python Single PUBSUB/ws

| 구분 | size별 throughput (Kmsg/s) | size별 평균 latency (ms) | report |
|------|-----------------------------|---------------------------|--------|
| C 기준 | 1167.614 / 846.436 / 409.612 / 25.604 / 16.782 / 11.353 | 48.285 / 35.844 / 21.743 / 12.281 / 15.457 / 17.758 | `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_012029_python-pubsub-ws-paired-c.txt` |
| Python final | 127.529 / 124.142 / 133.239 / 25.054 / 12.930 / 11.320 | 0.116 / 0.138 / 0.140 / 14.678 / 22.480 / 20.200 | `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_012101_python-pubsub-ws-current.txt` |

throughput ratio는 `10.922% / 14.666% / 32.528% / 97.852% / 77.047% / 99.709%`, 산술평균 `55.454%`이며 평균 latency ratio 산술평균은 `0.633x`다. 단순 one-way 최소 기준 `35%`를 통과한다.

### Python Single native send builder factory closure 제거

조건: Core `v0.10.1` release package, duration `1s`, runs `1`, msg sizes `64/256/1024/65536/131072/262144B`, auto-HWM `balanced`다. 각 대상은 C 종료 후 Python을 단독 실행했다.

| 대상 | size별 throughput ratio | 산술평균 | latency 산술평균 | 이전 Python 결과 대비 64/256/1024B 절대 throughput 변화 | report |
|------|---------------------------|----------|----------------------|---------------------------------------------------------|--------|
| `PAIR/tcp` | 5.544% / 12.325% / 22.646% / 110.749% / 79.856% / 62.292% | 48.902% | 0.908x | +13.6% / +17.2% / +20.1% | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_012330_python-pair-tcp-no-factory-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_012347_python-pair-tcp-no-factory-after.txt` |
| `DEALER_DEALER/tcp` | 5.847% / 11.668% / 19.421% / 88.298% / 70.731% / 55.573% | 41.923% | 1.039x | +19.4% / +11.8% / +12.9% | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_012517_python-dealer-dealer-tcp-no-factory-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_012533_python-dealer-dealer-tcp-no-factory-after.txt` |
| `DEALER_ROUTER/tcp` | 5.946% / 11.440% / 17.847% / 90.581% / 74.029% / 64.112% | 43.992% | 0.861x | +9.6% / +21.7% / +9.9% | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_012404_python-dealer-router-tcp-no-factory-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_012421_python-dealer-router-tcp-no-factory-after.txt` |
| `ROUTER_ROUTER/tcp` | 6.493% / 10.303% / 18.687% / 97.009% / 78.447% / 60.432% | 45.228% | 1.028x | +14.4% / +6.5% / +15.0% | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_012540_python-router-router-tcp-no-factory-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_012557_python-router-router-tcp-no-factory-after.txt` |

네 대상 모두 해당 aggregate 최소 기준을 통과했다. Sol review `SOL-PYTHON-NATIVE-SEND-BUILDER-20260811`은 변경 유지를 GO로 판정했다. unittest discovery는 `52 passed`이며 `test_lifecycle_contract` 1개 module은 환경에 `pytest`가 없어 import되지 않았다. 별도로 Core alignment test `12 passed`, source/native contract 함수 `12 passed`를 확인했다. public interface, builder state, `Message` ownership, caller-provided `Received`, request/reply routing metadata와 error 의미는 변경하지 않았다.

### Python Single PUBSUB/tcp `submit_single` 후보

| 구분 | size별 throughput ratio | 산술평균 | latency 산술평균 | report |
|------|---------------------------|----------|----------------------|--------|
| `submit_single` A/B | 7.918% / 12.174% / 20.297% / 99.430% / 81.702% / 66.987% | 48.085% | 0.570x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013057_python-pubsub-tcp-submit-single-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013127_python-pubsub-tcp-submit-single-after.txt` |

기존 publisher closure 제거 결과 대비 Python 절대 throughput 변화는 `-0.3% / -7.0% / -2.7% / +4.7% / -4.6% / -22.9%`다. 개선되지 않아 `submit_single` 후보는 원복했고, Sol review가 GO한 기존 publisher closure 제거만 유지한다.

### Python Single ws 추가 완료 대상

조건: Core `v0.10.1` release package, duration `1s`, runs `1`, msg sizes `64/256/1024/65536/131072/262144B`, auto-HWM `balanced`다. 각 대상은 C 종료 후 Python을 단독 실행했다.

| 대상 | size별 throughput ratio | 산술평균 | latency 산술평균 | 판정 | report |
|------|---------------------------|----------|----------------------|------|--------|
| `DEALER_DEALER/ws` | 11.278% / 15.751% / 33.933% / 83.377% / 70.642% / 74.524% | 48.251% | 0.762x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013400_python-dealer-dealer-ws-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013419_python-dealer-dealer-ws-current.txt` |
| `DEALER_ROUTER/ws` | 7.798% / 14.247% / 33.196% / 91.588% / 79.042% / 68.867% | 49.123% | 0.726x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013430_python-dealer-router-ws-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013448_python-dealer-router-ws-current.txt` |
| `ROUTER_ROUTER/ws` | 7.706% / 12.925% / 31.079% / 88.123% / 81.651% / 62.900% | 47.398% | 0.749x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013501_python-router-router-ws-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013519_python-router-router-ws-current.txt` |

세 대상 모두 shared native send 또는 routed receive owner 개선과 Sol review 범위에 포함되며 public interface·ownership·error 의미는 변경하지 않았다.

### Python Single wss 완료 대상

조건: Core `v0.10.1` release package, duration `1s`, runs `1`, msg sizes `64/256/1024/65536/131072/262144B`, auto-HWM `balanced`다. 각 대상은 C 종료 후 Python을 단독 실행했다.

| 대상 | size별 throughput ratio | 산술평균 | latency 산술평균 | 판정 | report |
|------|---------------------------|----------|----------------------|------|--------|
| `PAIR/wss` | 10.150% / 23.635% / 67.171% / 99.193% / 93.009% / 80.433% | 62.265% | 0.608x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013615_python-pair-wss-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013635_python-pair-wss-current.txt` |
| `PUBSUB/wss` | 13.027% / 22.708% / 60.810% / 93.419% / 83.032% / 84.186% | 59.530% | 0.588x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013642_python-pubsub-wss-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013711_python-pubsub-wss-current.txt` |
| `DEALER_DEALER/wss` | 9.458% / 22.180% / 74.055% / 95.528% / 85.007% / 79.577% | 60.968% | 0.603x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013717_python-dealer-dealer-wss-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013736_python-dealer-dealer-wss-current.txt` |
| `DEALER_ROUTER/wss` | 9.344% / 21.590% / 66.300% / 95.843% / 84.235% / 72.811% | 58.354% | 0.622x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013739_python-dealer-router-wss-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013757_python-dealer-router-wss-current.txt` |
| `ROUTER_ROUTER/wss` | 8.831% / 21.268% / 67.498% / 91.861% / 84.527% / 74.011% | 57.999% | 0.606x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013803_python-router-router-wss-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_013821_python-router-router-wss-current.txt` |

다섯 대상 모두 shared native send, publisher 또는 routed receive owner 개선과 Sol review 범위에 포함되며 public interface·ownership·error 의미는 변경하지 않았다.

### Python Single tls 완료 대상

조건: Core `v0.10.1` release package, duration `1s`, runs `1`, msg sizes `64/256/1024/65536/131072/262144B`, auto-HWM `balanced`다. 각 대상은 C 종료 후 Python을 단독 실행했다.

| 대상 | size별 throughput ratio | 산술평균 | latency 산술평균 | 판정 | report |
|------|---------------------------|----------|----------------------|------|--------|
| `PAIR/tls` | 7.172% / 17.858% / 48.400% / 93.853% / 82.336% / 71.797% | 53.570% | 0.820x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_013944_python-pair-tls-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_014002_python-pair-tls-current.txt` |
| `PUBSUB/tls` | 10.804% / 17.845% / 47.281% / 106.598% / 92.356% / 79.967% | 59.142% | 0.668x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_014005_python-pubsub-tls-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_014034_python-pubsub-tls-current.txt` |
| `DEALER_DEALER/tls` | 8.666% / 16.343% / 49.406% / 93.973% / 81.678% / 76.292% | 54.393% | 0.707x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_014042_python-dealer-dealer-tls-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_014100_python-dealer-dealer-tls-current.txt` |
| `DEALER_ROUTER/tls` | 7.407% / 14.933% / 47.113% / 86.694% / 77.591% / 73.245% | 51.164% | 0.738x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_014105_python-dealer-router-tls-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_014122_python-dealer-router-tls-current.txt` |
| `ROUTER_ROUTER/tls` | 6.798% / 14.518% / 45.411% / 89.530% / 77.828% / 71.894% | 50.996% | 0.790x | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_014128_python-router-router-tls-paired-c.txt`; Python: `/home/hep7hep7/project/zlink/bindings/python/perf/results/single/report/perf_python_single_linux_20260811_014145_python-router-router-tls-current.txt` |

다섯 대상 모두 shared native send, publisher 또는 routed receive owner 개선과 Sol review 범위에 포함되며 public interface·ownership·error 의미는 변경하지 않았다.
