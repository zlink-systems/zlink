Current stage profile, request-backpressure 1024 (main ee11390257, condition B). Full report: kairos job `s033/cprof`.

The source application submitter thread is the critical resource: 23.4 µs/message at 0.97 core (binding 3.7 µs).

| Stage, submitter µs/message | Framework | binding |
|---|---:|---:|
| allocation | 7.56 | 0.92 |
| Framework bookkeeping (registry, admission, deadline, dispatch) | 5.06 | 0 |
| binding/Core call | 5.00 | 2.12 |
| codec | 2.63 | 0.13 |
| hand-off and wake | 1.47 | 0.20 |
| benchmark driver | 0.91 | 0.38 |
| task/coroutine/result state | 0.78 | 0 |

- Heap calls per message: submitter 49.3 vs 12.4; both processes 114.6 vs 20.7. Context switches: 3.77 vs 0.20 per message.
- Reaching 0.6 needs the effective interval to fall from 27.5 to 7.6 µs/message. Allocation alone is 42% of that gap.
- Whole-process CPU is a second limit. The source process uses 108 µs/message and the target 83, against 60 µs/message available per process at 0.6 with 8 CPUs. Core IO threads make about 7× the binding's send/recv syscalls per message.
- Candidate R from the earlier analysis was already delivered by #3600. W (service wire written into the final native message) is the largest remaining named candidate, at 2.4 µs/message at most.

Next, in parallel: an allocation-site census of the submitter and both processes, and the W implementation with an A/B measurement.
