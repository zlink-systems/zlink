# Core 1.24.0 release gate 결과 보고서

기준 B1은 `core/v1.23.0` 커밋 `79567d379162e67476ba13e18cfe0f69925e63c9`, 후보 C는 `0005a4d6c07160bbf661a6f7aea3d8093880514e`이다. B2는 동일한 B1 commit과 Core runtime을 다시 측정한 두 번째 기준이다. 양쪽 라이브러리의 `zlink_version` 반환 문자열은 모두 `1.23.0`이다. B1 SHA-256은 `c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a`, C SHA-256은 `2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64`이다. 출처: `build-versions.txt`, `core-sha256.tsv`, `codex.log`의 `zlink_version` 결과.

Harness 42개 중 40개는 JOB0 SHA와 일치했다. `perf_stream_client` baseline 및 candidate executable은 JOB0 SHA와 달랐으며 각각 `0a8766b512ba27088160a6d6c6d8383b905c78c52c68e5948d50f4411f92fe14`, `aae931fe02b68ab23c0b24204f6cfe5e4c096c9ff70685015f6629e67541a16b`이다. 이 executable은 Core 의존성이 없고 나머지 실행 파일은 모두 기대한 prefix의 RUNPATH와 ldd 경로를 사용했다. 상세: `harness-verification.tsv`.

## 단계 시각

| 단계 | 시작 UTC | 종료 UTC | 근거 |
|---|---|---|---|
| 1.23 harness configure/build | 2026-11-09 13:56:46Z | 2026-11-09 13:58:48Z | `build-preflight.jsonl`, `logs/harness-1.23-configure.log`, `logs/harness-1.23-build.log` |
| 1.24 harness configure/build | 2026-11-09 13:58:48Z | 2026-11-09 13:59:25Z | `build-preflight.jsonl`, `logs/harness-1.24-configure.log`, `logs/harness-1.24-build.log` |
| B1-single | 2026-11-09T08:39:31.111671846Z | 2026-11-09T09:28:25.971052380Z | `results/B1-single.meta.json` |
| B1-multi | 2026-11-09T09:29:26.459863747Z | 2026-11-09T10:37:59.895266252Z | `results/B1-multi.meta.json` |
| C-single | 2026-11-09T10:40:30.598894016Z | 2026-11-09T11:29:25.581137039Z | `results/C-single.meta.json` |
| C-multi | 2026-11-09T11:30:56.134234503Z | 2026-11-09T12:39:18.656254106Z | `results/C-multi.meta.json` |
| B2-single | 2026-11-09T14:00:36.959411235Z | 2026-11-09T14:49:31.897353954Z | `results/B2-single.meta.json` |
| B2-multi | 2026-11-09T14:51:02.466668239Z | 2026-11-09T15:59:37.378569011Z | `results/B2-multi.meta.json` |
| Runs-5 확인 58개 group | 2026-11-09T16:01:04.155908104Z | 2026-11-10T12:26:13.458692385Z | 개별 B1→C→B2 stage 시각은 `confirm/times.tsv`; group 목록은 `confirm/groups.tsv` |

`gate-*-corrected.txt`는 B1·C·B2 원시 결과가 모두 완성된 뒤 comparator를 다시 실행해 만든 결과다. 초기 gate 파일의 missing baseline/candidate 및 누락된 multi 입력 판정을 완성 결과로 교체했다.

Runs-5 확인 중 첫 pause는 2026-11-10 06:14:09Z에 `multi ROUTER_ROUTER_REQREP / ws`의 C stage 뒤에 발생했고, 재개 후 그 그룹 전체를 B1→C→B2로 다시 측정했다. 두 번째 pause는 08:00:25Z에 `multi ROUTER_ROUTER_SENDSEND / tls` 완료 직후 발생했으며, 재개 직후 측정한 그룹은 `multi ROUTER_ROUTER_SENDSEND / ws`였다. 해당 그룹의 끊긴 B1 산출물은 `.stopped`로 보존하고 완료된 전체 rerun 결과를 사용했다. 상세: `confirm/pause-1639io.txt`, `confirm/pre-pause/`, `confirm/B1-multi-ROUTER_ROUTER_SENDSEND-ws.txt` 및 해당 `.log`.

## Runs-3 gate 최종 줄

| 비교 | single | multi |
|---|---|---|
| C 대 B1 | `Final: FAIL (cells=840, failed_cells=89, aggregates=210, failed_aggregates=79, report_errors=0)` | `Final: FAIL (cells=560, failed_cells=133, aggregates=140, failed_aggregates=73, report_errors=0)` |
| C 대 B2 | `Final: FAIL (cells=840, failed_cells=62, aggregates=210, failed_aggregates=74, report_errors=0)` | `Final: FAIL (cells=560, failed_cells=156, aggregates=140, failed_aggregates=82, report_errors=0)` |
| B2 대 B1 (A/A) | `Final: FAIL (cells=840, failed_cells=105, aggregates=210, failed_aggregates=113, report_errors=0)` | `Final: FAIL (cells=560, failed_cells=129, aggregates=140, failed_aggregates=67, report_errors=0)` |

## Runs-5 뒤에도 실패한 cell

표의 `A/A 안쪽?`은 runs-5 candidate ratio가 1.0000과 같은 cell의 B2/B1 ratio 사이에 들어오는지 나타낸다. #4135 경로는 ROUTER/DEALER/PAIR의 multipart send 및 reply 경로, #3930 경로는 send/recv content reference 경로다. 표의 원자료는 `gate-C-vs-B1-{single,multi}-corrected.txt`, `gate-C-vs-B2-{single,multi}-corrected.txt`, `gate-B2-vs-B1-{single,multi}-corrected.txt`, 해당 그룹의 `confirm/gate-<suite>-<pattern>-<transport>.txt`이다.

| Suite | 패턴 | 전송 | 크기 | 지표 | runs 3 C/B1 | runs 3 C/B2 | runs 5 | A/A B2/B1 | A/A 안쪽? | #4135 | #3930 |
|---|---|---|---:|---|---:|---:|---:|---:|---|---|---|
| multi | DEALER_DEALER | tcp | 256 | latency | 0.8075 | 0.6902 | 2.3896 | 1.1699 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tcp | 256 | latency_p95 | 1.0785 | 0.7169 | 2.3009 | 1.5044 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tcp | 256 | latency_p99 | 1.0220 | 0.7824 | 1.2454 | 1.3062 | 예 | 예 | 예 |
| multi | DEALER_DEALER | tcp | 65536 | latency | 0.8919 | 0.8429 | 1.2663 | 1.0582 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tcp | 65536 | latency_p95 | 0.7629 | 0.5501 | 1.2149 | 1.3867 | 예 | 예 | 예 |
| multi | DEALER_DEALER | tls | 64 | latency | 0.4053 | 0.8747 | 3.4007 | 0.4633 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 64 | latency_p95 | 0.0412 | 0.0220 | 55.5114 | 1.8718 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 64 | latency_p99 | 0.5374 | 0.5299 | 2.1196 | 1.0141 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 256 | latency | 1.1087 | 1.1823 | 1.0953 | 0.9378 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 256 | latency_p95 | 1.0491 | 1.0455 | 1.1241 | 1.0035 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 256 | latency_p99 | 1.0121 | 0.9740 | 1.2158 | 1.0391 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 1024 | latency | 0.7257 | 1.1622 | 1.1069 | 0.6244 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 1024 | latency_p95 | 0.6490 | 1.1678 | 1.1822 | 0.5558 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 65536 | bandwidth | 0.9849 | 0.9960 | 0.9309 | 0.9888 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 65536 | latency_p99 | 1.1963 | 1.0870 | 1.6025 | 1.1006 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | 65536 | throughput | 0.9849 | 0.9960 | 0.9309 | 0.9888 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | 64 | latency | 1.3002 | 2.7136 | 2.2434 | 0.4792 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | 64 | latency_p95 | 1.8147 | 50.4309 | 41.6404 | 0.0360 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | 64 | latency_p99 | 1.2703 | 2.1814 | 1.6327 | 0.5823 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | 256 | latency | 1.0772 | 0.7626 | 1.3578 | 1.4124 | 예 | 예 | 예 |
| multi | DEALER_DEALER | ws | 256 | latency_p95 | 1.0817 | 0.7261 | 1.4062 | 1.4897 | 예 | 예 | 예 |
| multi | DEALER_DEALER | ws | 256 | latency_p99 | 0.7665 | 1.0429 | 2.1563 | 0.7349 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | 1024 | latency_p95 | 32.4406 | 0.8851 | 1.0632 | 36.6524 | 예 | 예 | 예 |
| multi | DEALER_DEALER | wss | 256 | latency_p95 | 0.4302 | 0.4713 | 1.1186 | 0.9128 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | wss | 256 | latency_p99 | 0.5610 | 0.9226 | 1.1205 | 0.6081 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | wss | 65536 | latency_p95 | 0.1562 | 0.3324 | 1.1340 | 0.4700 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | wss | 65536 | latency_p99 | 1.4420 | 1.3088 | 1.1289 | 1.1018 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | 256 | latency | 0.8626 | 1.1106 | 1.3433 | 0.7767 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | 256 | latency_p95 | 0.8714 | 1.1208 | 1.4463 | 0.7775 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | 256 | latency_p99 | 0.8611 | 1.0226 | 1.3483 | 0.8420 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tls | 1024 | latency | 1.0079 | 0.9107 | 1.0529 | 1.1067 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 256 | bandwidth | 1.0267 | 0.6195 | 0.8729 | 1.6575 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 256 | latency | 0.8056 | 1.1432 | 1.2532 | 0.7047 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 256 | latency_p95 | 0.7565 | 1.3806 | 1.6033 | 0.5480 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 256 | latency_p99 | 0.1264 | 1.7741 | 1.8912 | 0.0713 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 256 | throughput | 1.0268 | 0.6195 | 0.8729 | 1.6575 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 1024 | bandwidth | 1.3604 | 0.8822 | 0.6374 | 1.5421 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 1024 | latency | 1.0026 | 0.7439 | 1.1620 | 1.3478 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 1024 | latency_p99 | 0.4842 | 0.1520 | 1.3239 | 3.1863 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | 1024 | throughput | 1.3604 | 0.8822 | 0.6374 | 1.5420 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 64 | bandwidth | 1.0217 | 0.9118 | 0.9159 | 1.1205 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 64 | throughput | 1.0216 | 0.9118 | 0.9156 | 1.1205 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 256 | bandwidth | 0.6654 | 1.0574 | 0.9434 | 0.6293 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 256 | throughput | 0.6654 | 1.0574 | 0.9434 | 0.6293 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | latency | 1.8096 | 1.2905 | 1.1333 | 1.4023 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | latency_p99 | 20.6038 | 1.4581 | 1.7591 | 14.1309 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | 64 | bandwidth | 1.0731 | 1.1012 | 0.8781 | 0.9745 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | 64 | throughput | 1.0731 | 1.1012 | 0.8781 | 0.9745 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency | 1.9436 | 1.3256 | 1.2841 | 1.4662 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency_p95 | 2.1335 | 1.3799 | 1.2561 | 1.5462 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency_p99 | 2.1794 | 1.3840 | 1.3100 | 1.5747 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 64 | latency | 1.0302 | 1.2043 | 1.2060 | 0.8555 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 64 | latency_p95 | 1.0410 | 1.1307 | 1.1585 | 0.9206 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency | 1.5485 | 0.9604 | 1.0821 | 1.6123 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency_p95 | 1.5211 | 0.9897 | 1.1516 | 1.5369 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency_p99 | 1.5058 | 1.0560 | 1.1353 | 1.4260 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 65536 | bandwidth | 1.1341 | 1.1131 | 0.9049 | 1.0189 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | 65536 | throughput | 1.1341 | 1.1131 | 0.9049 | 1.0189 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | 256 | bandwidth | 1.2525 | 1.2421 | 0.9241 | 1.0084 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | 256 | throughput | 1.2525 | 1.2421 | 0.9241 | 1.0084 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | 65536 | bandwidth | 1.0249 | 1.0460 | 0.9389 | 0.9799 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | 65536 | throughput | 1.0250 | 1.0460 | 0.9389 | 0.9799 | 아니오 | 예 | 예 |
| multi | PUBSUB | tcp | 256 | latency | 1.7783 | 1.1656 | 103.7107 | 1.5257 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tcp | 256 | latency_p95 | 1.6189 | 1.0034 | 121.7001 | 1.6134 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tcp | 256 | latency_p99 | 1.6125 | 0.9932 | 41.1231 | 1.6235 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tcp | 1024 | latency_p99 | 0.3429 | 0.9671 | 1.3944 | 0.3545 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | 64 | latency | 0.9994 | 0.9699 | 1.0951 | 1.0305 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | 64 | latency_p95 | 0.9879 | 1.0102 | 1.1256 | 0.9779 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | 64 | latency_p99 | 0.9906 | 1.0105 | 1.1165 | 0.9803 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | ws | 1024 | latency_p95 | 0.9853 | 1.0274 | 1.0547 | 0.9590 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | ws | 1024 | latency_p99 | 0.9571 | 0.9561 | 1.0526 | 1.0011 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | 64 | bandwidth | 0.9981 | 0.9955 | 0.9405 | 1.0026 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | 64 | latency | 1.0263 | 1.0290 | 1.2370 | 0.9974 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | 64 | latency_p95 | 1.0278 | 1.1197 | 1.1811 | 0.9179 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | 64 | latency_p99 | 1.0259 | 1.1179 | 1.1812 | 0.9177 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | 64 | throughput | 0.9981 | 0.9955 | 0.9405 | 1.0027 | 아니오 | 아니오 | 예 |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency | 1.0558 | 1.0777 | 1.1211 | 0.9796 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency_p95 | 1.0838 | 0.6585 | 1.1690 | 1.6459 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency_p99 | 1.2014 | 0.5868 | 1.2155 | 2.0472 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tcp | 65536 | latency_p95 | 1.0240 | 1.0950 | 1.0577 | 0.9352 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency | 2.4445 | 3.1158 | 1.8655 | 0.7845 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency_p95 | 5.2434 | 6.8989 | 2.6979 | 0.7600 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency_p99 | 7.4567 | 10.3179 | 3.6544 | 0.7227 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 256 | latency | 0.5732 | 0.6345 | 1.1669 | 0.9034 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 256 | latency_p95 | 0.5623 | 0.6109 | 1.2328 | 0.9204 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 256 | latency_p99 | 0.5523 | 0.5996 | 1.2055 | 0.9211 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 1024 | bandwidth | 1.0307 | 0.9124 | 0.9488 | 1.1297 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | 1024 | throughput | 1.0307 | 0.9124 | 0.9488 | 1.1297 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | ws | 1024 | latency | 0.8402 | 0.6576 | 1.2141 | 1.2777 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | ws | 1024 | latency_p95 | 0.7271 | 0.5277 | 1.4036 | 1.3777 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | ws | 1024 | latency_p99 | 0.8234 | 0.5889 | 1.2608 | 1.3984 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | 64 | latency | 0.0489 | 0.4937 | 1.0756 | 0.0989 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | 64 | latency_p95 | 0.0169 | 0.5511 | 1.0571 | 0.0307 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | 256 | latency | 0.9586 | 0.7916 | 1.3426 | 1.2111 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | 256 | latency_p95 | 0.9129 | 0.7192 | 1.4059 | 1.2693 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | 256 | latency_p99 | 0.9150 | 0.6873 | 1.4332 | 1.3314 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tcp | 64 | bandwidth | 0.9627 | 0.9813 | 0.9464 | 0.9811 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tcp | 64 | throughput | 0.9627 | 0.9813 | 0.9464 | 0.9810 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency | 1.1763 | 1.2001 | 1.1672 | 0.9801 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency_p95 | 1.2375 | 1.2391 | 1.2086 | 0.9987 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency_p99 | 1.2701 | 1.2482 | 1.1931 | 1.0176 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 256 | latency | 1.0401 | 1.1214 | 1.3764 | 0.9275 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 256 | latency_p95 | 1.0319 | 1.1583 | 1.4396 | 0.8909 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | 256 | latency_p99 | 1.0160 | 1.1536 | 1.5195 | 0.8807 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | 256 | bandwidth | 1.0553 | 1.0692 | 0.9179 | 0.9870 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | 256 | throughput | 1.0553 | 1.0692 | 0.9179 | 0.9870 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | 1024 | bandwidth | 0.9965 | 1.1729 | 0.8497 | 0.8496 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | 1024 | throughput | 0.9965 | 1.1729 | 0.8497 | 0.8496 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 64 | latency | 0.9872 | 0.3261 | 1.1702 | 3.0272 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 64 | latency_p95 | 0.9492 | 0.2372 | 1.1490 | 4.0018 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 64 | latency_p99 | 0.9013 | 0.2109 | 1.1108 | 4.2741 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 256 | latency | 0.8786 | 0.9000 | 1.0860 | 0.9762 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 256 | latency_p95 | 0.8302 | 0.8401 | 1.0996 | 0.9882 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 256 | latency_p99 | 0.8080 | 0.5056 | 1.1306 | 1.5982 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 1024 | latency | 0.9852 | 1.0148 | 1.1138 | 0.9708 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 1024 | latency_p95 | 0.9470 | 1.0323 | 1.0898 | 0.9174 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | 1024 | latency_p99 | 0.9142 | 1.0003 | 1.0570 | 0.9139 | 아니오 | 예 | 예 |
| multi | STREAM | wss | 65536 | latency_p99 | 0.9631 | 0.8977 | 1.0766 | 1.0728 | 아니오 | 아니오 | 예 |
| single | DEALER_DEALER | ws | 65536 | latency | 1.2267 | 1.1864 | 1.1835 | 1.0340 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ws | 65536 | latency_p95 | 1.2354 | 1.2063 | 1.1792 | 1.0242 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ws | 65536 | latency_p99 | 1.2119 | 1.1937 | 1.1944 | 1.0153 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | 64 | latency_p99 | 1.1139 | 1.0552 | 1.0507 | 1.0556 | 예 | 예 | 예 |
| single | DEALER_DEALER | wss | 256 | latency_p99 | 0.8644 | 0.9796 | 1.0587 | 0.8824 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | 65536 | latency | 1.0304 | 1.0218 | 1.0662 | 1.0084 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | 65536 | latency_p95 | 1.0694 | 1.0007 | 1.0557 | 1.0686 | 예 | 예 | 예 |
| single | DEALER_DEALER | wss | 65536 | latency_p99 | 1.0846 | 1.0011 | 1.0665 | 1.0835 | 예 | 예 | 예 |
| single | DEALER_ROUTER | wss | 65536 | latency | 0.9198 | 1.1651 | 1.0574 | 0.7894 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | wss | 65536 | latency_p95 | 0.8974 | 1.1650 | 1.0930 | 0.7703 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | wss | 65536 | latency_p99 | 0.9048 | 1.1568 | 1.0997 | 0.7821 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | inproc | 65536 | latency_p95 | 1.0547 | 1.0034 | 1.0993 | 1.0512 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 256 | bandwidth | 0.9875 | 0.9969 | 0.9080 | 0.9906 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 256 | latency | 1.0119 | 1.0023 | 1.1014 | 1.0096 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 256 | latency_p95 | 1.0107 | 1.0083 | 1.0896 | 1.0024 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 256 | throughput | 0.9875 | 0.9969 | 0.9080 | 0.9906 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency | 1.0750 | 0.9239 | 1.0577 | 1.1636 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency_p95 | 1.1025 | 0.9069 | 1.0808 | 1.2157 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency_p99 | 1.0961 | 0.9013 | 1.0712 | 1.2161 | 예 | 예 | 예 |
| single | PAIR | inproc | 64 | latency | 7.3502 | 0.7315 | 1.5893 | 10.0486 | 예 | 예 | 예 |
| single | PAIR | inproc | 64 | latency_p95 | 9.4693 | 0.0256 | 1.2670 | 369.3234 | 예 | 예 | 예 |
| single | PAIR | inproc | 256 | latency | 0.0299 | 0.0334 | 5.0627 | 0.8958 | 아니오 | 예 | 예 |
| single | PAIR | inproc | 256 | latency_p95 | 0.0043 | 0.0044 | 1.2010 | 0.9803 | 아니오 | 예 | 예 |
| single | PAIR | wss | 64 | latency | 0.9851 | 0.9878 | 1.1392 | 0.9973 | 아니오 | 예 | 예 |
| single | PAIR | wss | 64 | latency_p95 | 0.9857 | 0.9888 | 1.1258 | 0.9969 | 아니오 | 예 | 예 |
| single | PAIR | wss | 64 | latency_p99 | 0.9732 | 0.9853 | 1.1076 | 0.9877 | 아니오 | 예 | 예 |
| single | PUBSUB | ipc | 64 | latency_p99 | 1.1385 | 1.0095 | 1.1738 | 1.1278 | 아니오 | 아니오 | 예 |
| single | PUBSUB | ipc | 256 | latency_p99 | 1.0015 | 1.0180 | 1.0580 | 0.9838 | 아니오 | 아니오 | 예 |
| single | PUBSUB | tcp | 64 | latency_p99 | 0.9977 | 0.9788 | 1.0563 | 1.0193 | 아니오 | 아니오 | 예 |
| single | PUBSUB | ws | 64 | latency | 0.9604 | 0.9561 | 1.0649 | 1.0044 | 아니오 | 아니오 | 예 |
| single | PUBSUB | ws | 64 | latency_p95 | 0.9561 | 0.9658 | 1.0566 | 0.9899 | 아니오 | 아니오 | 예 |
| single | ROUTER_ROUTER | inproc | 256 | latency | 0.4701 | 3.2368 | 12.9343 | 0.1452 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | 256 | latency_p95 | 0.7394 | 1.0140 | 3.0239 | 0.7292 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | 256 | latency_p99 | 0.9193 | 1.0538 | 1.8634 | 0.8724 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | 1024 | latency | 0.3132 | 0.2055 | 3.7086 | 1.5237 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | 1024 | latency_p95 | 0.9033 | 0.8740 | 1.1311 | 1.0335 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | 1024 | latency_p99 | 1.0175 | 0.9915 | 1.0994 | 1.0263 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ipc | 64 | latency_p99 | 0.3778 | 1.1149 | 1.2396 | 0.3388 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ws | 65536 | latency | 1.0391 | 1.1102 | 1.0642 | 0.9360 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | 256 | latency | 0.8607 | 1.0168 | 1.0885 | 0.8465 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | 256 | latency_p95 | 0.8528 | 0.9944 | 1.1101 | 0.8577 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | 256 | latency_p99 | 0.8502 | 0.9893 | 1.1244 | 0.8593 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | 64 | bandwidth | 1.2310 | 1.4841 | 0.8117 | 0.8295 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | 64 | latency | 0.8122 | 0.6743 | 1.2322 | 1.2045 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | 64 | latency_p95 | 0.8155 | 0.6658 | 1.3670 | 1.2249 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | 64 | latency_p99 | 0.7529 | 0.6313 | 1.4803 | 1.1926 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | 64 | throughput | 1.2310 | 1.4840 | 0.8117 | 0.8295 | 아니오 | 예 | 예 |

## Runs-5 뒤에도 실패한 aggregate

집계 크기는 64, 256, 1024, 65536의 기하 평균이다. 원자료 경로는 위 cell 표와 같다.

| Suite | 패턴 | 전송 | 크기 | 지표 | runs 3 C/B1 | runs 3 C/B2 | runs 5 | A/A B2/B1 | A/A 안쪽? | #4135 | #3930 |
|---|---|---|---:|---|---:|---:|---:|---:|---|---|---|
| multi | DEALER_DEALER | tcp | geomean(4 sizes) | latency | 1.4894 | 1.1254 | 1.0599 | 1.3234 | 예 | 예 | 예 |
| multi | DEALER_DEALER | tls | geomean(4 sizes) | bandwidth | 1.0118 | 1.0116 | 0.9666 | 1.0002 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | geomean(4 sizes) | latency | 0.7183 | 1.0304 | 1.2968 | 0.6971 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | geomean(4 sizes) | latency_p95 | 0.3029 | 0.3530 | 1.5122 | 0.8580 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | geomean(4 sizes) | latency_p99 | 0.7999 | 0.9664 | 1.3859 | 0.8278 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | tls | geomean(4 sizes) | throughput | 1.0118 | 1.0116 | 0.9666 | 1.0002 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | geomean(4 sizes) | latency | 1.3992 | 1.1609 | 1.3186 | 1.2053 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | geomean(4 sizes) | latency_p95 | 2.9139 | 2.4131 | 2.7936 | 1.2075 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | ws | geomean(4 sizes) | latency_p99 | 0.9915 | 1.0933 | 1.1118 | 0.9069 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | wss | geomean(4 sizes) | bandwidth | 0.9935 | 0.9930 | 0.9851 | 1.0005 | 아니오 | 예 | 예 |
| multi | DEALER_DEALER | wss | geomean(4 sizes) | throughput | 0.9935 | 0.9930 | 0.9851 | 1.0005 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | geomean(4 sizes) | latency | 0.9758 | 1.0855 | 1.0297 | 0.8990 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | geomean(4 sizes) | latency_p95 | 0.9298 | 1.0302 | 1.0527 | 0.9025 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tcp | geomean(4 sizes) | latency_p99 | 0.9034 | 0.9444 | 1.0247 | 0.9567 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | tls | geomean(4 sizes) | latency_p95 | 0.9719 | 0.9726 | 1.0032 | 0.9993 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | bandwidth | 1.0809 | 0.8723 | 0.8561 | 1.2391 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency | 0.9339 | 0.9952 | 1.0593 | 0.9384 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency_p95 | 0.9293 | 1.0246 | 1.0421 | 0.9069 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency_p99 | 0.4941 | 0.6406 | 1.1893 | 0.7713 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | throughput | 1.0809 | 0.8723 | 0.8562 | 1.2390 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | bandwidth | 0.8795 | 0.9944 | 0.9583 | 0.8844 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | latency | 1.1164 | 0.9966 | 1.0414 | 1.1202 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | latency_p95 | 1.1636 | 1.0625 | 1.0091 | 1.0952 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | latency_p99 | 1.9800 | 0.7825 | 1.1734 | 2.5305 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | throughput | 0.8794 | 0.9944 | 0.9583 | 0.8844 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tcp | geomean(4 sizes) | bandwidth | 1.0104 | 0.9695 | 0.9983 | 1.0422 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tcp | geomean(4 sizes) | throughput | 1.0104 | 0.9695 | 0.9983 | 1.0422 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | geomean(4 sizes) | bandwidth | 1.1304 | 1.1383 | 0.9852 | 0.9931 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | tls | geomean(4 sizes) | throughput | 1.1304 | 1.1383 | 0.9852 | 0.9931 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | geomean(4 sizes) | bandwidth | 1.0769 | 1.0533 | 0.9899 | 1.0224 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | geomean(4 sizes) | latency | 1.1748 | 1.0780 | 1.0093 | 1.0898 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | geomean(4 sizes) | latency_p95 | 1.1698 | 1.0665 | 1.0312 | 1.0969 | 예 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | ws | geomean(4 sizes) | throughput | 1.0769 | 1.0533 | 0.9899 | 1.0224 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | geomean(4 sizes) | bandwidth | 1.1320 | 1.1175 | 0.9486 | 1.0130 | 아니오 | 예 | 예 |
| multi | DEALER_ROUTER_SENDSEND | wss | geomean(4 sizes) | throughput | 1.1320 | 1.1175 | 0.9486 | 1.0130 | 아니오 | 예 | 예 |
| multi | PUBSUB | tcp | geomean(4 sizes) | latency | 1.0021 | 3.2959 | 3.2417 | 0.3040 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tcp | geomean(4 sizes) | latency_p95 | 0.8696 | 3.2812 | 3.3467 | 0.2650 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tcp | geomean(4 sizes) | latency_p99 | 0.7992 | 2.9769 | 2.7473 | 0.2685 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | geomean(4 sizes) | bandwidth | 0.9731 | 0.9827 | 0.9900 | 0.9903 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | geomean(4 sizes) | latency | 0.9722 | 1.0677 | 1.0013 | 0.9106 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | geomean(4 sizes) | latency_p95 | 0.9983 | 1.0773 | 1.0167 | 0.9266 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | geomean(4 sizes) | latency_p99 | 0.9860 | 1.0659 | 1.0205 | 0.9251 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | tls | geomean(4 sizes) | throughput | 0.9731 | 0.9827 | 0.9900 | 0.9903 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | geomean(4 sizes) | bandwidth | 0.9795 | 0.9736 | 0.9961 | 1.0061 | 아니오 | 아니오 | 예 |
| multi | PUBSUB | wss | geomean(4 sizes) | throughput | 0.9795 | 0.9736 | 0.9961 | 1.0061 | 아니오 | 아니오 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | geomean(4 sizes) | latency | 1.1773 | 1.2067 | 1.1454 | 0.9757 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | geomean(4 sizes) | latency_p95 | 1.3196 | 1.3982 | 1.3361 | 0.9437 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | tls | geomean(4 sizes) | latency_p99 | 1.4256 | 1.5007 | 1.4524 | 0.9499 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | latency | 0.4646 | 0.8208 | 1.0896 | 0.5660 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | latency_p95 | 0.3562 | 0.8394 | 1.0938 | 0.4244 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tcp | geomean(4 sizes) | bandwidth | 0.9769 | 0.9989 | 0.9616 | 0.9780 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tcp | geomean(4 sizes) | throughput | 0.9769 | 0.9989 | 0.9617 | 0.9779 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | geomean(4 sizes) | latency | 1.0251 | 1.1110 | 1.0889 | 0.9226 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | geomean(4 sizes) | latency_p95 | 1.0163 | 1.1482 | 1.1227 | 0.8851 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | tls | geomean(4 sizes) | latency_p99 | 0.9865 | 1.1300 | 1.1211 | 0.8730 | 아니오 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | geomean(4 sizes) | bandwidth | 1.0138 | 1.1327 | 0.9188 | 0.8951 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | ws | geomean(4 sizes) | throughput | 1.0138 | 1.1327 | 0.9188 | 0.8951 | 예 | 예 | 예 |
| multi | ROUTER_ROUTER_SENDSEND | wss | geomean(4 sizes) | latency | 0.9837 | 0.7575 | 1.0313 | 1.2986 | 예 | 예 | 예 |
| multi | STREAM | tls | geomean(4 sizes) | bandwidth | 0.9977 | 1.0042 | 0.9866 | 0.9935 | 아니오 | 아니오 | 예 |
| multi | STREAM | tls | geomean(4 sizes) | throughput | 0.9977 | 1.0042 | 0.9865 | 0.9935 | 아니오 | 아니오 | 예 |
| multi | STREAM | wss | geomean(4 sizes) | latency_p99 | 1.0295 | 1.0127 | 1.0151 | 1.0167 | 예 | 아니오 | 예 |
| single | DEALER_DEALER | inproc | geomean(4 sizes) | bandwidth | 0.9914 | 0.9936 | 0.9936 | 0.9978 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | inproc | geomean(4 sizes) | latency | 1.0009 | 1.0046 | 1.0036 | 0.9963 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | inproc | geomean(4 sizes) | latency_p95 | 1.0069 | 1.0106 | 1.0013 | 0.9964 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | inproc | geomean(4 sizes) | throughput | 0.9914 | 0.9936 | 0.9936 | 0.9978 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ipc | geomean(4 sizes) | bandwidth | 1.0133 | 0.9973 | 0.9961 | 1.0160 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ipc | geomean(4 sizes) | latency | 1.0003 | 1.0114 | 1.0040 | 0.9891 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ipc | geomean(4 sizes) | latency_p99 | 0.9653 | 0.9974 | 1.0107 | 0.9678 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ipc | geomean(4 sizes) | throughput | 1.0133 | 0.9973 | 0.9961 | 1.0160 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ws | geomean(4 sizes) | latency | 1.0405 | 1.0437 | 1.0107 | 0.9970 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ws | geomean(4 sizes) | latency_p95 | 1.0443 | 1.0512 | 1.0137 | 0.9934 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | ws | geomean(4 sizes) | latency_p99 | 1.0381 | 1.0458 | 1.0110 | 0.9926 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | geomean(4 sizes) | latency | 0.9991 | 1.0225 | 1.0324 | 0.9770 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | geomean(4 sizes) | latency_p95 | 1.0094 | 1.0206 | 1.0286 | 0.9891 | 아니오 | 예 | 예 |
| single | DEALER_DEALER | wss | geomean(4 sizes) | latency_p99 | 0.9885 | 1.0080 | 1.0420 | 0.9806 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | inproc | geomean(4 sizes) | latency_p99 | 1.0204 | 0.9916 | 1.0060 | 1.0290 | 예 | 예 | 예 |
| single | DEALER_ROUTER | ws | geomean(4 sizes) | bandwidth | 1.0184 | 1.0155 | 0.9912 | 1.0029 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | ws | geomean(4 sizes) | throughput | 1.0184 | 1.0155 | 0.9912 | 1.0029 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | wss | geomean(4 sizes) | bandwidth | 1.0140 | 1.0124 | 0.9863 | 1.0016 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER | wss | geomean(4 sizes) | throughput | 1.0140 | 1.0124 | 0.9863 | 1.0016 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | inproc | geomean(4 sizes) | latency | 1.0203 | 1.0225 | 1.0095 | 0.9979 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | inproc | geomean(4 sizes) | latency_p95 | 1.0157 | 1.0045 | 1.0222 | 1.0112 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | bandwidth | 0.9965 | 1.0236 | 0.9743 | 0.9735 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency | 1.0120 | 0.9777 | 1.0270 | 1.0350 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency_p95 | 1.0222 | 0.9706 | 1.0318 | 1.0531 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | latency_p99 | 1.0293 | 0.9816 | 1.0161 | 1.0486 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | ws | geomean(4 sizes) | throughput | 0.9965 | 1.0236 | 0.9743 | 0.9735 | 예 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | bandwidth | 0.9355 | 0.9370 | 0.9926 | 0.9984 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | latency | 1.1069 | 1.1016 | 1.0074 | 1.0048 | 아니오 | 예 | 예 |
| single | DEALER_ROUTER_REQREP | wss | geomean(4 sizes) | throughput | 0.9355 | 0.9370 | 0.9926 | 0.9984 | 아니오 | 예 | 예 |
| single | PAIR | inproc | geomean(4 sizes) | latency_p95 | 0.3733 | 0.0838 | 1.0489 | 4.4529 | 예 | 예 | 예 |
| single | PAIR | inproc | geomean(4 sizes) | latency_p99 | 1.3807 | 0.9321 | 1.0027 | 1.4813 | 예 | 예 | 예 |
| single | PAIR | wss | geomean(4 sizes) | bandwidth | 0.9907 | 0.9982 | 0.9979 | 0.9924 | 예 | 예 | 예 |
| single | PAIR | wss | geomean(4 sizes) | throughput | 0.9907 | 0.9982 | 0.9979 | 0.9924 | 예 | 예 | 예 |
| single | PUBSUB | ipc | geomean(4 sizes) | latency_p95 | 1.0016 | 0.9976 | 1.0037 | 1.0041 | 예 | 아니오 | 예 |
| single | PUBSUB | ipc | geomean(4 sizes) | latency_p99 | 1.0384 | 0.9981 | 1.0548 | 1.0404 | 아니오 | 아니오 | 예 |
| single | PUBSUB | ws | geomean(4 sizes) | bandwidth | 0.9914 | 1.0161 | 0.9972 | 0.9757 | 예 | 아니오 | 예 |
| single | PUBSUB | ws | geomean(4 sizes) | latency | 0.9974 | 0.9677 | 1.0144 | 1.0308 | 예 | 아니오 | 예 |
| single | PUBSUB | ws | geomean(4 sizes) | latency_p95 | 0.9995 | 0.9726 | 1.0155 | 1.0276 | 예 | 아니오 | 예 |
| single | PUBSUB | ws | geomean(4 sizes) | latency_p99 | 1.0057 | 0.9690 | 1.0016 | 1.0379 | 예 | 아니오 | 예 |
| single | PUBSUB | ws | geomean(4 sizes) | throughput | 0.9914 | 1.0161 | 0.9972 | 0.9757 | 예 | 아니오 | 예 |
| single | ROUTER_ROUTER | inproc | geomean(4 sizes) | latency | 0.6200 | 0.5876 | 1.9120 | 1.0551 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | inproc | geomean(4 sizes) | latency_p99 | 0.8392 | 0.6853 | 1.0269 | 1.2245 | 예 | 예 | 예 |
| single | ROUTER_ROUTER | ipc | geomean(4 sizes) | bandwidth | 1.0110 | 0.9987 | 0.9925 | 1.0123 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ipc | geomean(4 sizes) | latency_p95 | 0.9843 | 0.9868 | 1.0029 | 0.9974 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ipc | geomean(4 sizes) | latency_p99 | 0.7719 | 1.0313 | 1.0550 | 0.7485 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ipc | geomean(4 sizes) | throughput | 1.0110 | 0.9987 | 0.9925 | 1.0123 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ws | geomean(4 sizes) | bandwidth | 1.0023 | 0.9945 | 0.9996 | 1.0079 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ws | geomean(4 sizes) | latency | 0.9985 | 1.0189 | 1.0093 | 0.9800 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | ws | geomean(4 sizes) | throughput | 1.0023 | 0.9945 | 0.9996 | 1.0079 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | geomean(4 sizes) | latency | 1.0109 | 1.0148 | 1.0317 | 0.9961 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | geomean(4 sizes) | latency_p95 | 0.9976 | 1.0069 | 1.0268 | 0.9908 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER | wss | geomean(4 sizes) | latency_p99 | 0.9913 | 1.0050 | 1.0228 | 0.9864 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | inproc | geomean(4 sizes) | bandwidth | 1.0018 | 0.9920 | 0.9962 | 1.0099 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | inproc | geomean(4 sizes) | throughput | 1.0018 | 0.9920 | 0.9962 | 1.0099 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | ipc | geomean(4 sizes) | latency_p95 | 0.8706 | 0.8957 | 1.0005 | 0.9720 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | bandwidth | 1.0567 | 1.1143 | 0.9621 | 0.9483 | 예 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | latency | 0.9515 | 0.9046 | 1.0309 | 1.0518 | 예 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | latency_p95 | 0.9711 | 0.9189 | 1.0641 | 1.0568 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | latency_p99 | 0.9592 | 0.9019 | 1.0864 | 1.0635 | 아니오 | 예 | 예 |
| single | ROUTER_ROUTER_REQREP | wss | geomean(4 sizes) | throughput | 1.0566 | 1.1143 | 0.9621 | 0.9483 | 예 | 예 | 예 |

남은 실패는 cell 165개, aggregate 121개, 총 48개 그룹이다. A/A 안쪽은 68개 실패 항목이다. 최종 실패 그룹은 verdict.md에 적었다.

## 리팩토링 점검

- Core build와 harness build의 메모리·디스크·load preflight가 중복된다: `/root/jobs/fx/cx/core123-gate/build-core.sh:10-20`, `/root/jobs/fx/cx/core123-gate/build.sh:8-18`.
- benchmark 실행, argv 구성, 시작·종료 시각 기록, 종료 코드 확인이 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:69-137`, `/root/jobs/fx/cx/v21/s033/core124b/resume-b2.sh:66-82`, `/root/jobs/fx/cx/v21/s033/core124b/confirm-run.sh:35-45`.
- comparator 실행과 종료 코드 처리가 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:138-145`, `/root/jobs/fx/cx/v21/s033/core124b/confirm-run.sh:47-56`.
- Core prefix RUNPATH/ldd 확인이 benchmark preflight와 verifier 양쪽에서 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:75-80`, `/root/jobs/fx/cx/v21/s033/core124b/verify-harnesses.py:10-19`.
- 이 작업에서는 중복을 수정하지 않았다.
