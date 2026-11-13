# #4350 새 binding 기준값 (C2·C3 off)

조건: C2·C3 off, `SCHED_OTHER`, CPU split 0–5/8–13 + housekeeping 6–7,14–15, 셀마다 warm run 1회, guard v7. 각 counted run은 active 5초이며 source CPU 0–7, target CPU 8–15로 요청해 guard가 측정 트리를 자동 축소하도록 한다.

| 언어 | 셀 | msg/s (새 기준) | 이전 pre-cstate msg/s | raw |
|---|---|---:|---:|---|
| node | send-saturation-1024 | 113435.4 | 110892.6 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-send-saturation-1024/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/send-saturation-1024/counted/attempt1/run1/zlink-node-send-saturation-1024/results.json) |
| node | send-saturation-4096 | 86577.4 | 87063.2 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-send-saturation-4096/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/send-saturation-4096/counted/attempt1/run1/zlink-node-send-saturation-4096/results.json) |
| node | request-serial-1024 | 7323.4 | 7202 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-request-serial-1024/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/request-serial-1024/counted/attempt1/run1/zlink-node-request-serial-1024/results.json) |
| node | request-serial-4096 | 7612 | 6995.2 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-request-serial-4096/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/request-serial-4096/counted/attempt1/run1/zlink-node-request-serial-4096/results.json) |
| node | request-backpressure-1024 | 29873 | 30957.2 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-request-backpressure-1024/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/request-backpressure-1024/counted/attempt1/run1/zlink-node-request-backpressure-1024/results.json) |
| node | request-backpressure-4096 | 25836.4 | 22793.2 ([이전 raw1](/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/session/binding/run1/zlink-node-request-backpressure-4096/results.json)) | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/node/request-backpressure-4096/counted/attempt1/run1/zlink-node-request-backpressure-4096/results.json) |
| cpp | send-saturation-1024 | 507627.265715 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/send-saturation-1024/counted/attempt1/run1/zlink-cpp-send-saturation-1024/results.json) |
| cpp | send-saturation-4096 | 225263.045846 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/send-saturation-4096/counted/attempt1/run1/zlink-cpp-send-saturation-4096/results.json) |
| cpp | request-serial-1024 | 12294.253556 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/request-serial-1024/counted/attempt1/run1/zlink-cpp-request-serial-1024/results.json) |
| cpp | request-serial-4096 | 11071.664813 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/request-serial-4096/counted/attempt1/run1/zlink-cpp-request-serial-4096/results.json) |
| cpp | request-backpressure-1024 | 115785.570527 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/request-backpressure-1024/counted/attempt1/run1/zlink-cpp-request-backpressure-1024/results.json) |
| cpp | request-backpressure-4096 | 125560.587017 | — | [raw](/root/jobs/fx/cx/v21/s033/binding-rebase/raw/cpp/request-backpressure-4096/counted/attempt1/run1/zlink-cpp-request-backpressure-4096/results.json) |
