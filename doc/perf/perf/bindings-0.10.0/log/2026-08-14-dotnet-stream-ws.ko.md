# .NET STREAM WS 측정 결과

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `ws` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 .NET 1회, 병렬 실행 없음 |

| Pattern | 64B | 256B | 1024B | 4096B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---|
| `MULTI_STREAM` | 89.04% | 89.79% | 91.84% | 97.40% | 92.02% | 중앙값 통과 |
