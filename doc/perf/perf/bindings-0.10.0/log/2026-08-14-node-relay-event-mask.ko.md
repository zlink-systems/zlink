# Node relay event-mask 측정 결과

## 측정 조건

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `ws` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096, 65536, 131072 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 Node 1회, 병렬 실행 없음 |

## C 대비 throughput 비율

| Node pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_DEALER_ROUTER_SENDSEND` | 65.54% | 62.45% | 62.44% | 61.80% | 73.49% | 78.32% | 67.34% | 중앙값 통과 |
| `MULTI_ROUTER_ROUTER_SENDSEND` | 55.39% | 48.30% | 47.89% | 51.73% | 73.68% | 72.97% | 58.33% | 최소 통과, 중앙값 미달 |
