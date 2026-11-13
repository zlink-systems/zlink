# #4350 새 binding 기준값 (C2·C3 off)

조건: C2·C3 off, `SCHED_OTHER`, CPU split 0–5/8–13 + housekeeping 6–7,14–15, 셀마다 warm run 1회, guard v7. 각 counted run은 active 5초이며 source CPU 0–7, target CPU 8–15로 요청해 guard가 측정 트리를 자동 축소하도록 한다.

| 언어 | 셀 | msg/s (새 기준) | 이전 pre-cstate msg/s | raw |
|---|---|---:|---:|---|
