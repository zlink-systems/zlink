# Rust Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 19:28~19:31 KST, pattern마다 C 직후 Rust(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core `a40cb46335`(D-B120 토큰 수정 포함) Release lib 19:25 빌드, `CARGO_TARGET_DIR` 별도. 시작 load 1분 2.8~5(직전 LTO 빌드의 5/15분 평균 잔여).
- C tag `p1rust`: `perf_c_multi_linux_20260905_192*_p1rust.txt`/`193*`; Rust `bindings/rust/perf/results/multi/report/` 같은 시각.
- 목표(§2.1 Rust): 단순 one-way 85%/95%, request/reply 70%/85%; latency 2.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 43.6% (494.7/1134.4) | 46.7% (498.5/1066.7) | 47.8% (411.0/859.7) | 76.7% (205.3/267.8) | 100.1% (56.6/56.5) | **63.0%** | 34x(큐) | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | 67.8% (108.6/160.1) | 63.5% (101.9/160.5) | 70.0% (104.7/149.6) | 65.8% (92.8/140.9) | 62.4% (14.7/23.5) | **65.9%** | 1.43x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 67.3% (97.4/144.8) | 66.0% (90.2/136.7) | 66.1% (89.2/135.0) | 69.3% (81.0/116.8) | 72.3% (15.9/22.1) | **68.2%** | 1.54x | `미달` |
| `MULTI_PUBSUB` | 67.7% (443.9/655.3) | 71.1% (588.6/828.3) | 84.6% (719.7/850.3) | 87.0% (584.1/671.2) | 105.4% (67.5/64.0) | **83.2%** | 0.96x | `미달` |

(괄호 Rust/C Kmsg/s 또는 Kops/s.) REQREP는 bindings 중 가장 높음(~100k ops/s, C의 2/3). DD 작은 메시지 고정 비용(44~48%)이 주 과제. 자체 pass 1 job(astra) 19:40 KST 시작.
