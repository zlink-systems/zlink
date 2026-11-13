| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Source CPU µs/msg before → after | Target CPU µs/msg before → after |
|---|---:|---:|---:|---:|---:|---:|
| request-backpressure / zlink-cpp | 243,153.1 | 247,237.4 | +1.7% | — | 9.662 → 9.659 | 10.491 → 10.133 |
| request-backpressure / zlink-framework-cpp | 50,895.7 | 50,397.0 | -1.0% | 0.2093 → 0.2038 | 82.152 → 84.328 | 61.334 → 62.960 |
| send-saturation / zlink-cpp | 617,595.4 | 616,689.0 | -0.1% | — | 3.005 → 3.020 | 3.414 → 3.401 |
| send-saturation / zlink-framework-cpp | 173,869.5 | 174,878.8 | +0.6% | 0.2815 → 0.2836 | 10.268 → 10.118 | 14.661 → 14.459 |

| Pattern / implementation | Before range, msg/s | After range, msg/s |
|---|---:|---:|
| request-backpressure / zlink-cpp | 243,039.2–246,398.8 | 238,160.8–248,427.4 |
| request-backpressure / zlink-framework-cpp | 50,692.2–52,279.3 | 49,647.2–51,718.9 |
| send-saturation / zlink-cpp | 589,121.3–635,272.3 | 613,989.4–625,183.8 |
| send-saturation / zlink-framework-cpp | 172,263.4–177,685.4 | 172,472.5–175,242.0 |
