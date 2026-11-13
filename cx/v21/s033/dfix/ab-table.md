| Pattern / bytes | Round | A main msg/s | B fix msg/s | A / binding | B / binding |
|---|---:|---:|---:|---:|---:|
| request-backpressure / 1024 | 1 | 28,836.0 | 29,859.2 | 0.489 | 0.506 |
| request-backpressure / 1024 | 2 | 29,441.4 | 29,608.2 | 0.499 | 0.502 |
| request-backpressure / 1024 | 3 | 29,785.4 | 29,533.2 | 0.505 | 0.501 |
| request-backpressure / 1024 | 4 | 29,319.4 | 29,732.4 | 0.497 | 0.504 |
| **request-backpressure / 1024** | **median** | **29,380.4** | **29,670.3** | **0.498** | **0.503** |
| request-backpressure / 4096 | 1 | 24,202.0 | 24,388.0 | 0.489 | 0.493 |
| request-backpressure / 4096 | 2 | 24,093.4 | 24,346.8 | 0.487 | 0.492 |
| request-backpressure / 4096 | 3 | 23,723.0 | 24,377.4 | 0.480 | 0.493 |
| request-backpressure / 4096 | 4 | 24,171.8 | 20,772.8 | 0.489 | 0.420 |
| **request-backpressure / 4096** | **median** | **24,132.6** | **24,362.1** | **0.488** | **0.493** |
| request-serial / 1024 | 1 | 5,807.6 | 5,815.6 | 0.514 | 0.515 |
| request-serial / 1024 | 2 | 5,665.0 | 5,802.8 | 0.501 | 0.513 |
| request-serial / 1024 | 3 | 6,014.4 | 6,044.8 | 0.532 | 0.535 |
| request-serial / 1024 | 4 | 4,299.6 | 5,529.2 | 0.380 | 0.489 |
| **request-serial / 1024** | **median** | **5,736.3** | **5,809.2** | **0.508** | **0.514** |
| request-serial / 4096 | 1 | 5,713.8 | 5,750.2 | 0.505 | 0.508 |
| request-serial / 4096 | 2 | 5,608.0 | 5,543.4 | 0.495 | 0.490 |
| request-serial / 4096 | 3 | 5,600.8 | 5,735.0 | 0.495 | 0.507 |
| request-serial / 4096 | 4 | 5,438.8 | 5,412.2 | 0.480 | 0.478 |
| **request-serial / 4096** | **median** | **5,604.4** | **5,639.2** | **0.495** | **0.498** |

| Cell | B versus A median | B / binding ≥ 0.6 |
|---|---:|---|
| request-backpressure / 1024 | +0.99% | no |
| request-backpressure / 4096 | +0.95% | no |
| request-serial / 1024 | +1.27% | no |
| request-serial / 4096 | +0.62% | no |

Minimum sampled MemAvailable during the 32 queued/run intervals: 20.07 GiB (limit: 12 GiB; no aborts).
