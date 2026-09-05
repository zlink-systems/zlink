# Node Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 13:53~14:00 KST, pattern마다 C 직후 Node(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core `349040d3e6`(Release lib 10:48 빌드; 이후 Core 수정은 cold path), Node 24.19, 시작 load 1.4~4.9(직전 dev ctest 잔여).
- C tag `p1node`: `perf_c_multi_linux_20260905_135*_p1node.txt`; Node `perf_node_multi_linux_20260905_13{5524,5649,5755,5859}_p1node.txt`(`bindings/node/perf/results/multi/report/`).
- 목표(§2.1 Node): 단순 one-way 35%/60%, request/reply 30%/60%; latency 5.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 15.8% (160.7/1017.4) | 17.5% (166.7/955.0) | 20.4% (169.7/832.4) | 39.9% (126.0/315.6) | 44.2% (26.8/60.7) | **27.6%** | 204x(큐 깊이) | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | 7.6% (11.0/145.3) | 6.2% (10.1/163.5) | 5.3% (8.3/155.3) | 9.7% (11.9/122.9) | 64.9% (14.4/22.2) | **18.7%** | 42x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 5.5% (9.7/176.4) | 5.2% (7.5/144.4) | 6.0% (8.4/139.6) | 13.0% (14.8/113.7) | 59.8% (12.9/21.5) | **17.9%** | 55x | `미달` |
| `MULTI_PUBSUB` | 19.0% (111.7/588.1) | 21.2% (164.8/778.0) | 20.1% (170.7/849.2) | 22.6% (146.2/646.5) | 60.1% (41.1/68.4) | **28.6%** | 0.91x | `미달` |

(괄호 Node/C Kmsg/s 또는 Kops/s.) REQREP가 8~15k ops/s로 고정(Java before와 같은 형태: 요청당 고정 지연·직렬화), one-way도 계획서 §2.1의 Node 과거 p10(33.6%)보다 낮음 → 0.17.0 포트 회귀 의심. 자체 pass 1 job(astra) 14:05 KST 시작.

## 재판정 — Core `a40cb46335` + pass 1c(`8ec82c4be7`), quiet 3-run 20:18~20:33 KST (`p1node-r3q`, PUBSUB은 체인 중단으로 미측정)

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency |
|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 24.2% | 22.0% | 21.8% | 42.0% | 56.4% | **33.3%** | 큐 |
| `MULTI_DEALER_ROUTER_REQREP` | 8.0% | 13.5% | 12.5% | 14.1% | 69.3% | **23.5%** | 48x |
| `MULTI_ROUTER_ROUTER_REQREP` | 11.0% | 15.6% | 15.0% | 19.7% | 60.4% | **24.4%** | 54x |

(Node/C Kmsg/s: DD 241.7/998.4, 206.6/938.5, 184.6/845.8, 133.5/317.5, 35.5/63.0; DR 16.6/206.9 … 15.7/22.7; RR 19.4/176.1 … 12.8/21.3.) Core 토큰 수정으로 64K REQREP는 69/60%로 회복했으나 작은 크기의 ~50 ms 고정 지연은 그대로 → Node 고유 원인(클라이언트 completion 대기) pass 1d 필요.
