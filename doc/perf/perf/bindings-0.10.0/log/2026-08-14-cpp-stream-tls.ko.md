# C++ STREAM TLS 측정 결과

## 측정 조건

| 항목 | 값 |
|---|---|
| Core runtime | release `0.10.1` |
| transport | `tls` |
| client 수 | 100 |
| message size | 64, 256, 1024, 4096 bytes |
| duration | size별 2초 |
| 실행 순서 | C 1회 후 C++ 1회, 병렬 실행 없음 |

## C 대비 throughput 비율

| Pattern | 64B | 256B | 1024B | 4096B | 산술평균 | 판정 |
|---|---:|---:|---:|---:|---:|---|
| `MULTI_STREAM` | 100.66% | 97.56% | 97.86% | 98.46% | 98.63% | 중앙값 통과 |
