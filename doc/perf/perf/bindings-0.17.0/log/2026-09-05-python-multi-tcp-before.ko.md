# Python Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 19:32~19:38 KST, pattern마다 C 직후 Python(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core `a40cb46335` Release lib 19:25, venv `~/project/zlink-work/c016/python-venv`(in-place extension). 시작 load 1분 3~6(직전 LTO 빌드 잔여).
- C tag `p1python`: `perf_c_multi_linux_20260905_193*_p1python.txt`; Python `perf_python_multi_linux_20260905_193{2xx..}.txt`.
- 목표(§2.1 Python): 단순 one-way 35%/60%, request/reply 30%/60%; latency 5.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 1.6% (15.2/922.3) | 1.7% (15.4/889.2) | 2.2% (15.3/688.4) | 4.8% (15.2/319.2) | 22.6% (13.4/59.2) | **6.6%** | 0.09x | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | 7.8% (13.2/169.4) | 7.6% (13.0/170.3) | 8.2% (12.9/157.8) | 9.6% (12.5/130.6) | 37.9% (9.4/24.8) | **14.2%** | 5.99x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 8.7% (11.9/137.1) | 8.7% (11.8/136.1) | 8.9% (11.4/128.4) | 9.9% (11.3/113.7) | 40.7% (8.7/21.3) | **15.4%** | 6.80x | `미달` |
| `MULTI_PUBSUB` | 21.7% (141.1/651.6) | 17.2% (134.6/780.4) | 13.7% (123.9/903.0) | 18.0% (124.7/691.1) | 60.5% (43.4/71.6) | **26.2%** | 1.09x | `미달` |

(괄호 Python/C Kmsg/s 또는 Kops/s.) DD가 size와 무관하게 **15k msg/s 고정**(메시지당 ~65 µs의 Python 측 고정 비용 — 제출 경로가 GIL 아래 직렬화되는 형태), REQREP 12~13k ops/s 고정, PUBSUB만 크기에 따라 변함. 자체 pass 1 job(astra) 19:45 KST 시작.
