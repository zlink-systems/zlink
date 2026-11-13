| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Source CPU µs/msg before → after | Target CPU µs/msg before → after |
|---|---:|---:|---:|---:|---:|---:|
| request-backpressure / zlink-cpp | 243,153.1 | 249,541.3 | +2.6% | — | 9.662 → 9.759 | 10.491 → 10.126 |
| request-backpressure / zlink-framework-cpp | 50,895.7 | 51,693.7 | +1.6% | 0.2093 → 0.2072 | 82.152 → 79.442 | 61.334 → 59.836 |
| send-saturation / zlink-cpp | 617,595.4 | 617,320.1 | -0.0% | — | 3.005 → 3.032 | 3.414 → 3.342 |
| send-saturation / zlink-framework-cpp | 173,869.5 | 172,856.5 | -0.6% | 0.2815 → 0.2800 | 10.268 → 10.129 | 14.661 → 14.818 |

| Pattern / implementation | Before range, msg/s | After range, msg/s |
|---|---:|---:|
| request-backpressure / zlink-cpp | 243,039.2–246,398.8 | 223,787.0–250,849.6 |
| request-backpressure / zlink-framework-cpp | 50,692.2–52,279.3 | 51,245.5–51,844.5 |
| send-saturation / zlink-cpp | 589,121.3–635,272.3 | 606,615.2–625,203.5 |
| send-saturation / zlink-framework-cpp | 172,263.4–177,685.4 | 167,555.1–175,351.7 |
