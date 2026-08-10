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
| .NET baseline | 1821.704 / 932.936 / 551.677 / 36.991 / 24.980 / 15.081 | 0.085 / 0.241 / 4.369 / 5.470 / 8.184 / 13.617 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_133947_dotnet-pair-tcp-dotnet-full.txt` |
| .NET final | 1892.295 / 919.669 / 603.699 / 39.236 / 24.924 / 14.992 | 0.171 / 0.240 / 0.326 / 5.189 / 8.229 / 13.713 | `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_134835_dotnet-pair-tcp-dotnet-flags-none-ab.txt` |

Final throughput ratio is `77.14% / 72.75% / 85.99% / 93.55% / 94.56% / 94.23%`, arithmetic mean `86.37%`. Final average latency ratio is `0.163x / 1.048x / 1.116x / 1.072x / 1.060x / 1.062x`, arithmetic mean `0.920x`; the target passes.
