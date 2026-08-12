# Java routed echo harness parity 측정 결과

## 측정 조건

- Core runtime: release `0.10.1`
- transport: `tcp`
- clients: `100`
- active duration: `1` second
- message sizes: `64, 256, 1024, 4096, 65536, 131072` bytes
- auto-HWM profile: `balanced`
- connect concurrency: `128`
- connect-ready timeout: `10000` ms
- monitor HWM: `4096000`
- 실행 순서: 각 비교에서 C를 먼저, Java를 다음에 단독 실행

Java multi runner의 기본 connect-ready timeout과 monitor HWM을 C runner와 같게
맞춘 뒤 측정했다. routed echo client loop도 C와 같이 `poll(-1)`로 ready socket을
기다린 뒤 `DONT_WAIT` recv를 drain한다.

## 결과

| Pattern | C throughput (Kops/s) | Java throughput (Kops/s) | Java/C ratio | 산술평균 |
|---|---:|---:|---:|---:|
| `MULTI_DEALER_ROUTER_SENDSEND` | 187.282 / 180.220 / 173.219 / 163.783 / 35.531 / 21.839 | 85.430 / 102.748 / 105.592 / 95.732 / 29.134 / 14.357 | 45.6% / 57.0% / 61.0% / 58.5% / 82.0% / 65.7% | 61.6% |
| `MULTI_ROUTER_ROUTER_SENDSEND` | 191.447 / 193.749 / 183.427 / 173.251 / 39.816 / 22.360 | 95.764 / 98.989 / 100.206 / 95.511 / 35.570 / 20.158 | 50.0% / 51.1% / 54.6% / 55.1% / 89.3% / 90.2% | 65.0% |

`MULTI_DEALER_ROUTER_SENDSEND`의 최신 report는 다음 경로에 있다.

- C: `/tmp/zlink-java-inventory-c/multi/report/perf_c_multi_linux_20260812_212452.txt`
- Java: `/tmp/zlink-java-inventory-java/multi/report/perf_java_multi_linux_20260812_212516.txt`

`MULTI_ROUTER_ROUTER_SENDSEND`은 routed echo `poll(-1)` 변경 후 같은 조건으로
측정했다.

- C: `/tmp/zlink-java-rr-poller-c/multi/report/perf_c_multi_linux_20260812_212156.txt`
- Java: `/tmp/zlink-java-rr-poller-java/multi/report/perf_java_multi_linux_20260812_212219.txt`
