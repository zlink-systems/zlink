# .NET ROUTER/ROUTER REQREP WSS 측정 결과

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `wss` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096, 65536, 131072 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 .NET 1회, 병렬 실행 없음 |

| Pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_ROUTER_ROUTER_REQREP` | 70.09% | 60.04% | 66.95% | 77.76% | 100.75% | 241.03% | 102.77% | 중앙값 통과 |
