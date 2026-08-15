# C++ router/router request/reply TLS 측정 결과

## 측정 조건

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `tls` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096, 65536, 131072 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 C++ 1회, 병렬 실행 없음 |

## C 대비 throughput 비율

| Pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_ROUTER_ROUTER_REQREP` | 91.47% | 89.59% | 87.74% | 93.83% | 96.38% | 105.16% | 94.03% | 중앙값 통과 |
