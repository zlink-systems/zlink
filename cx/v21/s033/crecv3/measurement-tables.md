| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Caller µs/msg before → after | Source µs/msg before → after | Target µs/msg before → after |
|---|---:|---:|---:|---:|---:|---:|---:|
| request-backpressure / zlink-cpp | 250,627.3 | 243,153.1 | -3.0% | — | 4.057 → 4.231 | 9.735 → 9.662 | 9.802 → 10.491 |
| request-backpressure / zlink-framework-cpp | 51,940.2 | 50,895.7 | -2.0% | 0.207 → 0.209 | 17.585 → 17.887 | 81.215 → 82.152 | 59.341 → 61.334 |
| send-saturation / zlink-cpp | 612,012.0 | 617,595.4 | +0.9% | — | 1.416 → 1.418 | 3.024 → 3.005 | 3.360 → 3.414 |
| send-saturation / zlink-framework-cpp | 175,907.9 | 173,869.5 | -1.2% | 0.287 → 0.282 | 4.817 → 4.904 | 10.067 → 10.268 | 14.625 → 14.661 |

| Allocation scope | Before | After |
|---|---:|---:|
| Source caller heap calls/request | 17.251161 | 17.261617 |
| Entire source heap calls/request | 34.136117 | 34.012763 |
| Entire target heap calls/request | 41.328927 | 41.177539 |
