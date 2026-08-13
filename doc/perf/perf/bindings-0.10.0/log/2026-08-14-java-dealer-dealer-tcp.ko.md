# Java dealer/dealer TCP 측정 결과

## 측정 조건

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `tcp` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096, 65536, 131072 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 Java 1회, 병렬 실행 없음 |

## C 대비 throughput 비율

| Pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_DEALER_DEALER` | 57.89% | 82.00% | 85.54% | 70.81% | 90.54% | 90.94% | 79.62% | 최소 통과, 중앙값 미달 |
