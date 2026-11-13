| 언어 | 패턴 | 크기 | Framework | binding | F÷B | 이전 F÷B | ≥0.6 |
|---|---|---:|---:|---:|---:|---:|:---:|
| cpp | request-backpressure | 1024 | 36,315 | 220,467 | 0.165 | 0.138 | X |
| cpp | request-backpressure | 4096 | 35,570 | 140,737 | 0.253 | 0.221 | X |
| cpp | request-serial | 1024 | 6,537 | 12,614 | 0.518 | 0.431 | X |
| cpp | request-serial | 4096 | 6,459 | 11,548 | 0.559 | 0.510 | X |
| cpp | send-saturation | 1024 | 163,273 | 542,278 | 0.301 | 0.298 | X |
| cpp | send-saturation | 4096 | 128,943 | 226,752 | 0.569 | 0.452 | X |
| dotnet | request-backpressure | 1024 | 29,571 | 58,990 | 0.501 | 0.508 | X |
| dotnet | request-backpressure | 4096 | 23,943 | 49,454 | 0.484 | 0.495 | X |
| dotnet | request-serial | 1024 | 5,825 | 11,303 | 0.515 | 0.537 | X |
| dotnet | request-serial | 4096 | 5,557 | 11,320 | 0.491 | 0.527 | X |
| dotnet | send-saturation | 1024 | 178,561 | 230,659 | 0.774 | 0.779 | O |
| dotnet | send-saturation | 4096 | 130,583 | 156,018 | 0.837 | 0.819 | O |
| java | request-backpressure | 1024 | 57,145 | 124,104 | 0.460 | 0.465 | X |
| java | request-backpressure | 4096 | 55,700 | 90,010 | 0.619 | 0.639 | O |
| java | request-serial | 1024 | 8,651 | 14,862 | 0.582 | 0.572 | X |
| java | request-serial | 4096 | 8,125 | 14,037 | 0.579 | 0.658 | X |
| java | send-saturation | 1024 | 114,094 | 311,460 | 0.366 | 0.362 | X |
| java | send-saturation | 4096 | 107,953 | 170,736 | 0.632 | 0.627 | O |
| node | request-backpressure | 1024 | 17,306 | 27,912 | 0.620 | - | O |
| node | request-backpressure | 4096 | 17,280 | 24,737 | 0.699 | - | O |
| node | request-serial | 1024 | 5,491 | 9,078 | 0.605 | - | O |
| node | request-serial | 4096 | 4,875 | 8,117 | 0.601 | - | O |
| node | send-saturation | 1024 | 77,539 | 116,868 | 0.663 | - | O |
| node | send-saturation | 4096 | 54,892 | 91,921 | 0.597 | - | X |

cells=24 pass=9 carried=0
