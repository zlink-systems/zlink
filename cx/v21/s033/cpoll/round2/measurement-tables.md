| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Source CPU µs/msg before → after | Target CPU µs/msg before → after |
|---|---:|---:|---:|---:|---:|---:|
| request-backpressure / zlink-cpp | 243,153.1 | 248,921.8 | +2.4% | — | 9.662 → 9.579 | 10.491 → 10.025 |
| request-backpressure / zlink-framework-cpp | 50,895.7 | 50,621.5 | -0.5% | 0.2093 → 0.2034 | 82.152 → 79.833 | 61.334 → 61.817 |
| send-saturation / zlink-cpp | 617,595.4 | 613,270.3 | -0.7% | — | 3.005 → 3.060 | 3.414 → 3.390 |
| send-saturation / zlink-framework-cpp | 173,869.5 | 176,444.4 | +1.5% | 0.2815 → 0.2877 | 10.268 → 9.891 | 14.661 → 14.237 |

| Pattern / implementation | Before range, msg/s | After range, msg/s |
|---|---:|---:|
| request-backpressure / zlink-cpp | 243,039.2–246,398.8 | 247,106.4–256,282.3 |
| request-backpressure / zlink-framework-cpp | 50,692.2–52,279.3 | 50,612.6–50,800.0 |
| send-saturation / zlink-cpp | 589,121.3–635,272.3 | 607,922.9–627,275.5 |
| send-saturation / zlink-framework-cpp | 172,263.4–177,685.4 | 174,833.8–180,443.2 |
