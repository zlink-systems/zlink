# Java Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 11:44~11:49 KST, pattern마다 C 직후 Java(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core `0c39ed2e52` Release lib, JDK 22, 시작 load 1.7~4.7(.NET pass 2 리뷰 job 동시 — 코드 읽기 단계).
- C tag `p1java`: `perf_c_multi_linux_20260905_11{4449,4559,4705,4814}_p1java.txt`; Java `perf_java_multi_linux_20260905_11{4527,4633,4742,4848}.txt`(`bindings/java/perf/results/multi/report/`).
- 목표(§2.1 Java): 단순 one-way 70%/90%, request/reply 50%/70%; latency 3.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 55.9% (583.6/1043.8) | 55.8% (561.9/1007.6) | 61.9% (559.6/903.6) | 62.2% (227.1/365.3) | **18.0%** (14.0/77.7) | **50.8%** | 0.30x | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | **5.0%** (9.6/192.8) | 8.8% (14.8/168.2) | 8.6% (15.0/174.4) | 10.6% (14.9/141.3) | 42.7% (9.6/22.6) | **15.1%** | 2.32x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 10.0% (14.1/141.5) | 10.8% (14.3/132.2) | 11.4% (14.1/123.5) | 13.3% (14.5/109.4) | 74.6% (10.0/13.5) | **24.0%** | 2.09x | `미달` |
| `MULTI_PUBSUB` | 74.1% (458.1/618.3) | 69.4% (520.6/749.6) | 74.6% (591.8/792.9) | 74.8% (509.8/681.7) | 112.3% (70.7/62.9) | **81.0%** | 1.04x | `미달` |

(괄호 Java/C Kmsg/s 또는 Kops/s.) REQREP가 10~15k ops/s로 고정된 듯 낮고(요청당 고정 지연·직렬화 의심), DD 64 KiB가 18%(큰 메시지 복사 경로), 작은 메시지 고정 비용 ~45%. 자체 pass 1 job(astra, `doc/plan/c016-worklog/briefs/java-perf-pass1.prompt`) 11:52 KST 시작.
