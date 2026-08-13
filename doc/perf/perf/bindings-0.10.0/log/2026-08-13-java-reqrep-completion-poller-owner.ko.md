# Java request/reply completion poller 결과

| Transport | Java / C 산술평균 | 판정 |
|---|---:|---|
| tls | 84.11% | 중앙값 통과 |
| ws | 72.03% | 중앙값 통과 |
| wss | 68.10% | 최소 통과, 중앙값 미달 |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_REQREP`, balanced auto-HWM
