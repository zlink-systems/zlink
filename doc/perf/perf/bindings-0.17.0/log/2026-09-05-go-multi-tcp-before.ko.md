# Go Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 17:46~17:51 KST, pattern마다 C 직후 Go(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core lib 16:51 빌드(`349040d3e6`+A 커밋), Go 러너 `ZLINK_GO_NATIVE_DIR`/`CGO_LDFLAGS`/`LD_LIBRARY_PATH`를 `core/build/lib`로 지정(패키지 native 디렉터리 없음). **주의: Core 수정 job의 빌드·테스트(load 3~7)와 겹쳐 C 기준값도 평소보다 낮음(예: DR_REQREP C 112k vs 145~200k)** → 비율은 참고, 판정은 quiet 재짝지음으로.
- C tag `p1go`: `perf_c_multi_linux_20260905_174*_p1go.txt`; Go `bindings/go/perf/results/multi/report/` 같은 시각 report.
- 목표(§2.1 Go): 단순 one-way 55%/65%, request/reply 40%/53%; latency 3.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 20.2% (202.9/1003.8) | 24.0% (190.2/793.1) | 22.1% (106.5/481.3) | 50.3% (82.8/164.8) | 40.9% (18.4/45.1) | **31.5%** | 43x(큐) | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | 1.4% (1.5/112.5) | 1.2% (1.4/120.0) | 1.6% (1.5/97.4) | 1.6% (1.4/92.7) | 9.0% (1.2/13.3) | **2.9%** | 0.11x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 1.4% (1.4/97.0) | 1.3% (1.3/99.4) | 1.4% (1.2/90.2) | 1.7% (1.3/73.7) | 8.0% (1.1/13.3) | **2.8%** | 0.20x | `미달` |
| `MULTI_PUBSUB` | 31.0% (148.4/479.4) | 29.0% (150.8/519.8) | 33.9% (189.7/559.2) | 32.4% (158.5/489.0) | 110.0% (52.2/47.5) | **47.3%** | 1.73x | `미달` |

(괄호 Go/C Kmsg/s 또는 Kops/s.) REQREP가 **1.1~1.5k ops/s**로 고정(요청당 ~70 ms 고정 지연 — Java/Node before와 같은 형태이며 훨씬 심함), one-way도 과거 p10(59.4%)의 절반. 자체 pass 1 job(astra) 17:55 KST 시작.
