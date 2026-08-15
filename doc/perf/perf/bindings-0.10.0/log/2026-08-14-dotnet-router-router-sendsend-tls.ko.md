# .NET ROUTER/ROUTER SENDSEND TLS 측정 결과

## 측정 조건

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `tls` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096, 65536, 131072 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 .NET 1회, 병렬 실행 없음 |

## C 대비 throughput 비율

| Pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_ROUTER_ROUTER_SENDSEND` | 86.25% | 89.03% | 85.67% | 86.08% | 109.41% | 107.39% | 93.97% | 중앙값 통과 |
