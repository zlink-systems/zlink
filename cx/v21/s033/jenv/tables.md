| 항목 | main Framework | prototype paced | binding paced |
|---|---|---|---|
| request / reply record B | 1,435 / 1,443 | 1,092 / 1,092 | 1,252 / 1,252 |
| source IO/0 CPU µs/request | 12.892 | 10.943 | 7.620 |
| source IO/2 CPU µs/request | 12.415 | 12.070 | 4.892 |
| target IO/0 CPU µs/request | 10.004 | 8.592 | 6.047 |
| target IO/1 CPU µs/request | 13.554 | 12.767 | 6.201 |
| source I/O 합계 CPU µs/request | 25.317 | 23.023 | 12.520 |
| target I/O 합계 CPU µs/request | 23.568 | 21.368 | 12.254 |
| source mesh CPU µs/request | 12.371 | 11.047 | 해당 thread 없음 |
| target mesh CPU µs/request | 10.699 | 9.727 | 해당 thread 없음 |
| caller µs | 12.642 | 10.679 | 8.778 |
| active 전체 req/s | 61,132.050 | 61,129.150 | 61,131.750 |
| perf 창 req/s | 58,498.153 | 61,166.253 | 61,131.136 |
| BPF 창 req/s | 57,269.174 | 59,163.060 | 50,517.298 |

| 실행 | thread | 합계 µs | TCP send | TCP receive | epoll | eventfd/wake | Core userspace | 기타 syscall | 표본 수 |
|---|---|---|---|---|---|---|---|---|---|
| main Framework | source IO/0 | 12.892 | 9.246 | 0.000 | 1.316 | 0.000 | 2.293 | 0.038 | 343 |
| main Framework | source IO/2 | 12.415 | 0.000 | 7.769 | 2.361 | 0.038 | 1.942 | 0.305 | 326 |
| main Framework | target IO/0 | 10.004 | 0.000 | 5.820 | 1.788 | 0.342 | 1.902 | 0.152 | 263 |
| main Framework | target IO/1 | 13.554 | 10.119 | 0.000 | 1.232 | 0.000 | 2.166 | 0.037 | 363 |
| prototype unpaced | source IO/0 | 9.826 | 7.254 | 0.000 | 1.088 | 0.000 | 1.418 | 0.066 | 298 |
| prototype unpaced | source IO/2 | 11.011 | 0.000 | 6.640 | 1.935 | 0.234 | 1.668 | 0.534 | 330 |
| prototype unpaced | target IO/0 | 7.734 | 0.000 | 4.520 | 1.339 | 0.402 | 1.306 | 0.167 | 231 |
| prototype unpaced | target IO/1 | 11.969 | 9.091 | 0.000 | 1.308 | 0.000 | 1.504 | 0.065 | 366 |
| prototype paced | source IO/0 | 10.943 | 8.153 | 0.000 | 1.087 | 0.000 | 1.667 | 0.036 | 302 |
| prototype paced | source IO/2 | 12.070 | 0.000 | 6.971 | 2.165 | 0.404 | 2.091 | 0.440 | 329 |
| prototype paced | target IO/0 | 8.592 | 0.000 | 5.089 | 1.549 | 0.295 | 1.475 | 0.184 | 233 |
| prototype paced | target IO/1 | 12.767 | 9.512 | 0.000 | 1.194 | 0.000 | 2.062 | 0.000 | 353 |
| binding paced | source IO/0 | 7.620 | 5.334 | 0.000 | 0.617 | 0.000 | 1.597 | 0.073 | 210 |
| binding paced | source IO/2 | 4.892 | 0.000 | 3.072 | 0.796 | 0.303 | 0.645 | 0.076 | 129 |
| binding paced | target IO/0 | 6.047 | 0.000 | 3.591 | 1.136 | 0.000 | 0.989 | 0.330 | 165 |
| binding paced | target IO/1 | 6.201 | 4.557 | 0.000 | 0.747 | 0.000 | 0.859 | 0.037 | 166 |

| mesh source JFR 분류 µs (표본) | main Framework | prototype unpaced | prototype paced |
|---|---|---|---|
| registry take | 1.211 (65/664) | 0.000 (0/704) | 0.000 (0/552) |
| reply decode | 1.025 (55/664) | 0.000 (0/704) | 0.000 (0/552) |
| multipart view decode | 0.578 (31/664) | 0.000 (0/704) | 0.000 (0/552) |
| completion handoff | 2.869 (154/664) | 3.278 (223/704) | 3.462 (173/552) |
| other | 6.689 (359/664) | 7.071 (481/704) | 7.585 (379/552) |

| 실행 | thread | syscall | 호출/request | 성공 B/호출 | EAGAIN/request | <2 / 2–4 / 4–8 / ≥8 KiB 비율 |
|---|---|---|---|---|---|---|
| main Framework | source IO/0 | sendto | 0.507 | 2,793.754 | 0.000 | 48.2%/25.6%/24.7%/1.5% |
| main Framework | source IO/2 | recvfrom | 0.700 | 2,728.512 | 0.178 | 50.3%/26.3%/20.7%/2.7% |
| main Framework | target IO/0 | recvfrom | 0.721 | 2,917.964 | 0.235 | 45.8%/25.6%/25.3%/3.3% |
| main Framework | target IO/1 | sendto | 0.588 | 2,424.511 | 0.000 | 56.0%/25.1%/18.8%/0.2% |
| prototype unpaced | source IO/0 | sendto | 0.455 | 2,378.580 | 0.000 | 49.1%/35.8%/12.9%/2.2% |
| prototype unpaced | source IO/2 | recvfrom | 0.730 | 2,064.678 | 0.206 | 51.0%/40.6%/8.3%/0.1% |
| prototype unpaced | target IO/0 | recvfrom | 0.588 | 2,549.942 | 0.164 | 43.6%/37.5%/15.9%/3.0% |
| prototype unpaced | target IO/1 | sendto | 0.563 | 1,921.761 | 0.000 | 56.1%/37.5%/6.4%/0.0% |
| prototype paced | source IO/0 | sendto | 0.548 | 1,973.283 | 0.000 | 54.7%/37.0%/7.9%/0.4% |
| prototype paced | source IO/2 | recvfrom | 0.795 | 1,907.453 | 0.228 | 55.7%/37.7%/6.5%/0.1% |
| prototype paced | target IO/0 | recvfrom | 0.725 | 2,136.706 | 0.219 | 49.4%/39.5%/10.3%/0.8% |
| prototype paced | target IO/1 | sendto | 0.614 | 1,761.212 | 0.000 | 61.3%/34.1%/4.6%/0.0% |
| binding paced | source IO/0 | sendto | 0.929 | 1,322.737 | 0.000 | 94.5%/5.5%/0.0%/0.0% |
| binding paced | source IO/2 | recvfrom | 1.049 | 1,596.614 | 0.280 | 77.2%/22.1%/0.5%/0.2% |
| binding paced | target IO/0 | recvfrom | 1.109 | 1,404.325 | 0.234 | 88.6%/11.3%/0.1%/0.0% |
| binding paced | target IO/1 | sendto | 0.791 | 1,553.738 | 0.000 | 79.9%/19.6%/0.5%/0.0% |

| 실행 | thread | kernel TCP cycles/B 추정 | kernel TCP cycles/request 추정 | TCP/kernel 표본 |
|---|---|---|---|---|
| main Framework | source IO/0 | 14.211 | 20,392.886 | 275/364 |
| main Framework | source IO/2 | 9.711 | 14,013.138 | 193/334 |
| main Framework | target IO/0 | 5.771 | 8,281.571 | 138/282 |
| main Framework | target IO/1 | 16.243 | 23,438.293 | 279/380 |
| prototype unpaced | source IO/0 | 17.146 | 18,723.729 | 241/321 |
| prototype unpaced | source IO/2 | 12.346 | 13,481.972 | 193/350 |
| prototype unpaced | target IO/0 | 8.006 | 8,742.010 | 113/246 |
| prototype unpaced | target IO/1 | 21.555 | 23,537.753 | 290/385 |
| prototype paced | source IO/0 | 17.781 | 19,416.325 | 217/305 |
| prototype paced | source IO/2 | 12.926 | 14,115.186 | 188/344 |
| prototype paced | target IO/0 | 10.092 | 11,020.787 | 117/248 |
| prototype paced | target IO/1 | 20.055 | 21,899.777 | 272/376 |
| binding paced | source IO/0 | 12.071 | 15,113.021 | 176/259 |
| binding paced | source IO/2 | 4.282 | 5,360.689 | 40/114 |
| binding paced | target IO/0 | 7.402 | 9,267.076 | 118/211 |
| binding paced | target IO/1 | 9.007 | 11,277.343 | 92/158 |

| 실행 | process | local → remote port | TCP_NODELAY | SO_SNDBUF B | SO_RCVBUF 관측 범위 B | sk_userlocks |
|---|---|---|---|---|---|---|
| main Framework | target | 5254 → 32896 | 1 | 3677184 | 6291456–6291456 | 4 |
| main Framework | target | 5254 → 32904 | 1 | 3677184 | 131072–131072 | 4 |
| main Framework | source | 32896 → 5254 | 1 | 3677184 | 131072–131072 | 0 |
| main Framework | source | 32904 → 5254 | 1 | 3677184 | 6291456–6291456 | 0 |
| prototype paced | target | 5254 → 53028 | 1 | 4194304 | 6291456–6291456 | 4 |
| prototype paced | target | 5254 → 53034 | 1 | 4194304 | 131072–131072 | 4 |
| prototype paced | source | 53028 → 5254 | 1 | 4194304 | 131072–131072 | 0 |
| prototype paced | source | 53034 → 5254 | 1 | 4194304 | 4318117–4318117 | 0 |
| binding paced | target | 5247 → 39594 | 1 | 4194304 | 4611526–4611526 | 4 |
| binding paced | target | 5247 → 39598 | 1 | 4194304 | 131072–131072 | 4 |
| binding paced | source | 39594 → 5247 | 1 | 4194304 | 131072–131072 | 0 |
| binding paced | source | 39598 → 5247 | 1 | 4194304 | 5669054–5669054 | 0 |

| 실행 | 수신 process | native part B | allocator 범위 안 | 범위 밖 | 범위 밖 비율 | storage type |
|---|---|---|---|---|---|---|
| main Framework | target | 13 | 322439 | 419 | 0.13% | 101 |
| main Framework | target | 1398 | 277386 | 45471 | 14.08% | 105 |
| main Framework | source | 13 | 2 | 0 | 0.00% | 101 |
| main Framework | source | 21 | 321903 | 818 | 0.25% | 101 |
| main Framework | source | 1398 | 268217 | 54505 | 16.89% | 105 |
| prototype paced | target | 13 | 2 | 0 | 0.00% | 101 |
| prototype paced | target | 41 | 316940 | 1191 | 0.37% | 105 |
| prototype paced | target | 1027 | 309747 | 8384 | 2.64% | 105 |
| prototype paced | source | 13 | 2 | 0 | 0.00% | 101 |
| prototype paced | source | 41 | 317615 | 1198 | 0.38% | 105 |
| prototype paced | source | 1027 | 307307 | 11506 | 3.61% | 105 |
| binding paced | target | 201 | 317403 | 90 | 0.03% | 105 |
| binding paced | target | 1027 | 316997 | 496 | 0.16% | 105 |
| binding paced | source | 201 | 317279 | 211 | 0.07% | 105 |
| binding paced | source | 1027 | 316476 | 1014 | 0.32% | 105 |

