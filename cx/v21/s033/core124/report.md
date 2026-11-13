# Core 1.24.0 release comparison gate 진행 보고서

기준은 `core/v1.23.0` 커밋 `79567d379162e67476ba13e18cfe0f69925e63c9`이다. 후보는 `0005a4d6c07160bbf661a6f7aea3d8093880514e`이다. 후보에는 #3930과 #4135가 포함된다. tag와 baseline 커밋은 일치한다. 두 커밋의 `bindings/c/perf` 트리는 동일하다. 후보 소스의 `VERSION`은 1.23.0이어서 양쪽 harness 디렉터리는 `build-release-1.23.0`을 사용했다.

Core runtime SHA-256은 baseline `c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a`, candidate `2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64`이다. `verify-builds.txt`는 PASS이며 harness 42개를 검사했다. Core 의존 실행 파일은 각자 자기 prefix의 RUNPATH와 ldd 경로를 사용했다. 세부 내용은 `sha-ldd.tsv`, `ldd-all.txt`에 있다. 두 source worktree는 측정 전 clean이었다. Prefix별 로컬 provenance 파일은 runner 요구사항 때문에 작성했다.

| 단계 | 시작 UTC | 종료 UTC | 코드 | 로그 |
|---|---|---|---:|---|
| B1-single | 2026-11-09T08:39:31.111671846Z | 2026-11-09T09:28:25.971052380Z | 0 | results/B1-single.log |
| B1-multi | 2026-11-09T09:29:26.459863747Z | 2026-11-09T10:37:59.895266252Z | 0 | results/B1-multi.log |
| C-single | 2026-11-09T10:40:30.598894016Z | 2026-11-09T11:29:25.581137039Z | 0 | results/C-single.log |
| C-multi | 2026-11-09T11:30:56.134234503Z | 2026-11-09T12:39:18.656254106Z | 0 | results/C-multi.log |
| B2-single 첫 preflight | 2026-11-09T12:39:18Z | 2026-11-09T13:09:22Z | 3 | gate-run.log |
| B2-single 재개 preflight | 2026-11-09T13:20:35Z | 2026-11-09T13:50:09Z | 3 | resume-b2-final.log |
| B2-multi | 시작 안 됨 | 시작 안 됨 | — | gate-run.log |
| hotpath_gate | 2026-11-09T08:37:44.028082909Z | 2026-11-09T08:37:53.433579827Z | 0 | logs/hotpath_gate.log |
| wake_invariants | 2026-11-09T08:37:53.457855440Z | 2026-11-09T08:38:08.485267520Z | 0 | logs/wake_invariants.log |

측정 preflight 기록은 `preflight.jsonl`에 있다. B2-single은 원래 전체 gate 실행에서 30분 동안 통과하지 못했다. 재개 실행에서도 30분 안에 통과하지 못했다. 두 번 모두 `cpp-build-slot`의 외부 `cmake --install` 프로세스가 감지됐다. MemAvailable은 기준 12 GiB를 넘었지만 외부 빌드 감지 조건을 만족하지 못했다. 마지막 시점은 2026-11-09T13:50:09Z이다. B2 benchmark는 시작되지 않았고 B2-multi는 미실행이다. preflight 세부값은 다음과 같다.

- 2026-11-09T13:46:38Z: B2-single, MemAvailable 26701246464 bytes, load 0.1, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:47:08Z: B2-single, MemAvailable 26706382848 bytes, load 0.11, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:47:39Z: B2-single, MemAvailable 26696507392 bytes, load 0.12, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:48:09Z: B2-single, MemAvailable 26695950336 bytes, load 0.14, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:48:39Z: B2-single, MemAvailable 26773983232 bytes, load 0.08, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:49:09Z: B2-single, MemAvailable 26732920832 bytes, load 0.17, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:49:39Z: B2-single, MemAvailable 26778140672 bytes, load 0.16, 허용 False, 외부 build 프로세스 1개
- 2026-11-09T13:50:09Z: B2-single, MemAvailable 26721521664 bytes, load 0.22, 허용 False, 외부 build 프로세스 1개

## C 대 B1 첫 gate 결과

candidate comparator의 첫 `--runs 3` 판정은 `gate-C-vs-B1.txt`에 있다. 최종 줄은 `Final: FAIL (cells=1400, failed_cells=222, aggregates=350, failed_aggregates=152, report_errors=0)`이다. B2 결과가 없어 C 대 B2 및 B2 대 B1 비교는 산출되지 않았다.

| 비교 | gate 최종 줄 |
|---|---|
| C vs B1 | `Final: FAIL (cells=1400, failed_cells=222, aggregates=350, failed_aggregates=152, report_errors=0)` |
| C vs B2 | 산출되지 않음. B2 결과 없음. |
| B2 vs B1 | 산출되지 않음. B2 결과 없음. |

| Suite | 전체 cell | 실패 cell | 전체 aggregate | 실패 aggregate | 오류 |
|---|---:|---:|---:|---:|---:|
| single | 840 | 89 | 210 | 79 | 0 |
| multi | 560 | 133 | 140 | 73 | 0 |

아래 cell과 aggregate는 모두 첫 runs 3 gate 실패 항목이다. 요청된 runs 5 확인과 A/A ratio는 B2 단계가 시작되지 않아 산출할 수 없었다. 따라서 아래 표는 runs 5 뒤에도 실패한 결과를 뜻하지 않는다. 확인 대상 58개 조합은 `confirm/groups.tsv`, 전체 첫 gate 항목은 `gate-failures.json`에 있다.

### 첫 gate에서 실패한 cell

| Suite | 패턴 | 전송 | 크기 | 지표 | runs 3 C/B1 ratio | runs 5 재확인 | A/A runs 3 B2/B1 | pipe write 경로 |
|---|---|---|---:|---|---:|---|---|---|
| single | DEALER_DEALER | inproc | 1024 | bandwidth | 0.9492 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | inproc | 1024 | throughput | 0.9492 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ipc | 64 | latency | 1.0841 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | tcp | 64 | latency_p95 | 1.0960 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | tcp | 64 | latency_p99 | 1.2875 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | 65536 | latency | 1.2267 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | 65536 | latency_p95 | 1.2354 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | 65536 | latency_p99 | 1.2119 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | 64 | latency | 1.1117 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | 64 | latency_p95 | 1.1073 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | 64 | latency_p99 | 1.1139 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | 65536 | latency_p95 | 1.0694 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | 65536 | latency_p99 | 1.0846 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | 1024 | bandwidth | 0.9007 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | 1024 | latency | 1.1250 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | 1024 | latency_p95 | 1.1096 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | 1024 | latency_p99 | 1.0846 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | 1024 | throughput | 0.9007 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | 64 | latency | 1.0724 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | 64 | latency_p95 | 1.0712 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | 64 | latency_p99 | 1.0749 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 64 | latency | 1.0735 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 64 | latency_p95 | 1.0851 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 64 | latency_p99 | 1.1030 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 256 | latency | 1.3253 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 256 | latency_p95 | 1.2971 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | 256 | latency_p99 | 1.3001 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | 64 | latency | 1.0891 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | 64 | latency_p99 | 1.0702 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | 65536 | latency_p95 | 1.0547 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | tcp | 64 | latency_p99 | 1.1438 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | tcp | 1024 | latency | 1.0551 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency | 1.0750 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency_p95 | 1.1025 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | 1024 | latency_p99 | 1.0961 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 64 | bandwidth | 0.6541 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 64 | latency | 1.5259 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 64 | latency_p95 | 1.5433 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 64 | latency_p99 | 1.6038 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 64 | throughput | 0.6541 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 256 | bandwidth | 0.9190 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 256 | latency | 1.0889 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 256 | latency_p95 | 1.1051 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 256 | latency_p99 | 1.1244 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | 256 | throughput | 0.9190 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | 64 | latency | 7.3502 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | 64 | latency_p95 | 9.4693 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | 64 | latency_p99 | 4.7416 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | wss | 65536 | latency_p95 | 1.0664 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | wss | 65536 | latency_p99 | 1.0752 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PUBSUB | ipc | 64 | latency_p99 | 1.1385 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | tls | 1024 | latency_p99 | 1.0560 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ws | 65536 | latency_p99 | 1.0566 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 256 | latency | 1.0930 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 256 | latency_p95 | 1.1000 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 256 | latency_p99 | 1.0854 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 1024 | latency_p99 | 1.1218 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 65536 | latency_p95 | 1.0561 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | 65536 | latency_p99 | 1.0845 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | ROUTER_ROUTER | inproc | 64 | latency_p95 | 1.3191 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tcp | 256 | latency | 1.1833 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tcp | 256 | latency_p95 | 1.1772 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tcp | 256 | latency_p99 | 1.1923 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tcp | 1024 | latency | 1.0578 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 64 | latency_p95 | 1.0510 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 1024 | latency | 1.3116 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 1024 | latency_p95 | 1.3104 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 1024 | latency_p99 | 1.3040 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 65536 | bandwidth | 0.9359 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 65536 | latency | 1.0692 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | 65536 | throughput | 0.9359 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | ws | 65536 | latency_p95 | 1.0594 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | 64 | latency | 1.0548 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | 65536 | latency | 1.1457 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | 65536 | latency_p95 | 1.1335 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | 65536 | latency_p99 | 1.1185 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | inproc | 65536 | latency_p95 | 1.1174 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ipc | 256 | latency | 1.1566 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ipc | 256 | latency_p99 | 1.4846 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | tls | 64 | latency_p95 | 1.3090 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | tls | 64 | latency_p99 | 1.3369 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | 256 | latency_p95 | 1.0526 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | 1024 | latency_p99 | 1.0517 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 256 | latency_p99 | 1.0525 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 1024 | bandwidth | 0.9267 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 1024 | latency | 1.1004 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 1024 | latency_p95 | 1.1206 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 1024 | latency_p99 | 1.1201 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | wss | 1024 | throughput | 0.9267 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 64 | latency | 7.0012 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 64 | latency_p95 | 91.3742 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 64 | latency_p99 | 11.5641 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 256 | latency_p95 | 1.0785 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 65536 | bandwidth | 0.9464 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | 65536 | throughput | 0.9464 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tls | 256 | latency | 1.1087 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tls | 65536 | latency_p99 | 1.1963 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 64 | latency | 1.3002 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 64 | latency_p95 | 1.8147 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 64 | latency_p99 | 1.2703 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 256 | latency | 1.0772 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 256 | latency_p95 | 1.0817 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 1024 | latency | 2.7638 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 1024 | latency_p95 | 32.4406 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 1024 | latency_p99 | 1.5339 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | 65536 | latency_p95 | 1.1321 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | 64 | latency | 1.4478 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | 64 | latency_p99 | 1.4161 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | 1024 | latency_p99 | 1.0565 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | 65536 | latency_p99 | 1.4420 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | 64 | latency | 1.1741 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | 64 | latency_p95 | 1.1278 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | 1024 | bandwidth | 0.9347 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | 1024 | throughput | 0.9347 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | 65536 | latency | 1.1342 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | 64 | bandwidth | 0.7497 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | 64 | throughput | 0.7497 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | 1024 | bandwidth | 0.9314 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | 1024 | throughput | 0.9314 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | ws | 64 | latency_p95 | 1.0918 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 256 | bandwidth | 0.6654 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 256 | throughput | 0.6654 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | bandwidth | 0.8668 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | latency | 1.8096 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | latency_p95 | 2.2326 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | latency_p99 | 20.6038 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | 1024 | throughput | 0.8668 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tcp | 64 | bandwidth | 0.9438 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tcp | 64 | throughput | 0.9438 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 64 | latency | 1.1960 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 64 | latency_p95 | 1.2044 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 64 | latency_p99 | 1.1674 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 256 | latency | 1.3920 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 256 | latency_p95 | 1.5022 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 256 | latency_p99 | 1.5898 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency | 1.9436 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency_p95 | 2.1335 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | 1024 | latency_p99 | 2.1794 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 256 | latency | 1.1075 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 256 | latency_p95 | 1.1234 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 256 | latency_p99 | 1.1945 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency | 1.5485 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency_p95 | 1.5211 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 1024 | latency_p99 | 1.5058 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 65536 | latency | 1.0783 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | 65536 | latency_p95 | 1.0526 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 64 | latency | 1.2367 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 64 | latency_p95 | 1.2289 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 64 | latency_p99 | 1.2022 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 256 | latency | 1.4838 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 256 | latency_p95 | 1.6005 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 256 | latency_p99 | 1.7024 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 1024 | latency | 1.8732 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 1024 | latency_p95 | 2.0840 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 1024 | latency_p99 | 2.3467 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 65536 | latency_p95 | 1.0725 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | 65536 | latency_p99 | 1.1075 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | PUBSUB | tcp | 256 | latency | 1.7783 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | tcp | 256 | latency_p95 | 1.6189 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | tcp | 256 | latency_p99 | 1.6125 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | tls | 65536 | latency_p99 | 1.0755 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 64 | bandwidth | 0.9019 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 64 | latency | 1.2541 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 64 | latency_p95 | 1.4680 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 64 | latency_p99 | 1.4272 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 64 | throughput | 0.9019 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 256 | bandwidth | 0.9192 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 256 | throughput | 0.9192 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 65536 | bandwidth | 0.9356 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 65536 | latency_p99 | 1.0958 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | 65536 | throughput | 0.9356 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 256 | bandwidth | 0.9302 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 256 | latency | 1.2666 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 256 | latency_p95 | 1.1449 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 256 | throughput | 0.9302 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 1024 | latency_p99 | 1.1112 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 65536 | latency | 1.0673 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 65536 | latency_p95 | 1.0693 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | 65536 | latency_p99 | 1.0520 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency | 1.0558 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency_p95 | 1.0838 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tcp | 64 | latency_p99 | 1.2014 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tcp | 256 | latency_p99 | 1.1635 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency | 2.4445 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency_p95 | 5.2434 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 64 | latency_p99 | 7.4567 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 256 | bandwidth | 0.7422 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 256 | throughput | 0.7423 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 1024 | latency | 1.0765 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 1024 | latency_p99 | 1.0542 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | 65536 | latency | 1.2737 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | ws | 1024 | bandwidth | 0.9154 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | ws | 1024 | throughput | 0.9154 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 64 | bandwidth | 0.7553 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 64 | throughput | 0.7553 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 256 | bandwidth | 0.8042 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 256 | throughput | 0.8043 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 1024 | bandwidth | 0.8531 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 1024 | throughput | 0.8531 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 65536 | latency_p95 | 1.0873 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | 65536 | latency_p99 | 1.1075 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency | 1.1763 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency_p95 | 1.2375 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | 64 | latency_p99 | 1.2701 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | 65536 | latency_p95 | 1.0505 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | 65536 | latency_p99 | 1.0851 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | 256 | latency | 1.2833 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | 256 | latency_p95 | 1.2768 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | 256 | latency_p99 | 1.2436 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | 1024 | latency_p95 | 1.1068 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | 1024 | latency_p99 | 1.1567 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | 256 | bandwidth | 0.9387 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | 256 | throughput | 0.9388 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | 65536 | latency | 1.0957 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | 65536 | latency_p95 | 1.2031 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | 65536 | latency_p99 | 1.0918 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | STREAM | tcp | 1024 | latency_p95 | 1.0516 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tcp | 1024 | latency_p99 | 1.0527 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | 256 | latency_p99 | 1.0786 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | 1024 | latency_p99 | 1.0548 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | 256 | latency_p95 | 1.0971 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | 256 | latency_p99 | 1.1426 | 미실행 | 미실행 | 아니요 (signature 변경만) |

### 첫 gate에서 실패한 aggregate

| Suite | 패턴 | 전송 | 지표 | runs 3 C/B1 geomean | runs 5 재확인 | A/A runs 3 B2/B1 | pipe write 경로 |
|---|---|---|---|---:|---|---|---|
| single | DEALER_DEALER | inproc | bandwidth | 0.9914 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | inproc | latency | 1.0009 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | inproc | latency_p95 | 1.0069 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | inproc | throughput | 0.9914 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ipc | latency | 1.0003 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | tcp | latency_p99 | 1.0472 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | bandwidth | 0.9975 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | latency | 1.0405 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | latency_p95 | 1.0443 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | latency_p99 | 1.0381 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | ws | throughput | 0.9975 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_DEALER | wss | latency_p95 | 1.0094 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | bandwidth | 0.9898 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | latency | 1.0140 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | latency_p95 | 1.0168 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | latency_p99 | 1.0204 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | inproc | throughput | 0.9898 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | tls | bandwidth | 0.9994 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | tls | latency | 1.0081 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | tls | latency_p95 | 1.0188 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | tls | latency_p99 | 1.0109 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | tls | throughput | 0.9994 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | latency | 1.0205 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | latency_p95 | 1.0145 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | ws | latency_p99 | 1.0154 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | latency | 1.0329 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | latency_p95 | 1.0192 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER | wss | latency_p99 | 1.0194 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | bandwidth | 0.9969 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | latency | 1.0203 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | latency_p95 | 1.0157 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | latency_p99 | 1.0313 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | inproc | throughput | 0.9969 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | bandwidth | 0.9965 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | latency | 1.0120 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | latency_p95 | 1.0222 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | latency_p99 | 1.0293 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | ws | throughput | 0.9965 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | bandwidth | 0.9355 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | latency | 1.1069 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | latency_p95 | 1.0766 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | latency_p99 | 1.0745 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | DEALER_ROUTER_REQREP | wss | throughput | 0.9355 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | bandwidth | 0.9932 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | latency_p99 | 1.3807 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | inproc | throughput | 0.9932 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | wss | bandwidth | 0.9907 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PAIR | wss | throughput | 0.9907 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | PUBSUB | ipc | bandwidth | 0.9997 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ipc | latency_p95 | 1.0016 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ipc | latency_p99 | 1.0384 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ipc | throughput | 0.9997 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | tcp | latency_p95 | 1.0033 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ws | bandwidth | 0.9914 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ws | latency_p99 | 1.0057 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | ws | throughput | 0.9914 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | bandwidth | 0.9956 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | latency | 1.0445 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | latency_p95 | 1.0476 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | latency_p99 | 1.0722 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | PUBSUB | wss | throughput | 0.9956 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| single | ROUTER_ROUTER | tls | bandwidth | 0.9781 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | latency | 1.0804 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | latency_p95 | 1.0883 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | latency_p99 | 1.0687 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | tls | throughput | 0.9781 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | ws | latency_p95 | 1.0078 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | ws | latency_p99 | 1.0076 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | bandwidth | 0.9994 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | latency | 1.0109 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER | wss | throughput | 0.9994 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | inproc | latency_p95 | 1.0204 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | tls | latency_p95 | 1.0471 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | tls | latency_p99 | 1.0716 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | bandwidth | 0.9975 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | latency | 1.0051 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | latency_p95 | 1.0145 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | latency_p99 | 1.0155 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| single | ROUTER_ROUTER_REQREP | ws | throughput | 0.9975 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | bandwidth | 0.9814 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | latency | 1.4894 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | latency_p95 | 2.9002 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | latency_p99 | 1.7386 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | tcp | throughput | 0.9814 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | latency | 1.3992 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | ws | latency_p95 | 2.9139 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | bandwidth | 0.9935 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | latency_p99 | 1.0489 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_DEALER | wss | throughput | 0.9935 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | bandwidth | 0.9895 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tcp | throughput | 0.9895 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | bandwidth | 0.9156 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | tls | throughput | 0.9155 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | bandwidth | 0.8795 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | latency | 1.1164 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | latency_p95 | 1.1636 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | latency_p99 | 1.9800 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_REQREP | wss | throughput | 0.8794 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | latency | 1.3296 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | latency_p95 | 1.3862 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | tls | latency_p99 | 1.4103 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | latency | 1.1748 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | latency_p95 | 1.1698 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | ws | latency_p99 | 1.1783 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | latency | 1.3757 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | latency_p95 | 1.4480 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | DEALER_ROUTER_SENDSEND | wss | latency_p99 | 1.5186 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | PUBSUB | tcp | latency | 1.0021 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | tls | bandwidth | 0.9731 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | tls | throughput | 0.9731 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | bandwidth | 0.9343 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | latency | 1.0453 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | latency_p95 | 1.0909 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | latency_p99 | 1.0829 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | ws | throughput | 0.9343 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | bandwidth | 0.9795 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | latency | 1.0921 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | latency_p95 | 1.0684 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | latency_p99 | 1.0516 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | PUBSUB | wss | throughput | 0.9795 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | ROUTER_ROUTER_REQREP | tcp | latency_p99 | 1.0391 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | bandwidth | 0.9412 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | latency | 1.1773 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | latency_p95 | 1.3196 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | latency_p99 | 1.4256 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | tls | throughput | 0.9412 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | ws | bandwidth | 0.9829 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | ws | throughput | 0.9829 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | bandwidth | 0.8436 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_REQREP | wss | throughput | 0.8436 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tcp | bandwidth | 0.9769 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tcp | throughput | 0.9769 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | latency | 1.0251 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | tls | latency_p95 | 1.0163 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | ws | latency | 1.0092 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | bandwidth | 0.9960 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | ROUTER_ROUTER_SENDSEND | wss | throughput | 0.9960 | 미실행 | 미실행 | 예 (pipe write 또는 reply multipart send) |
| multi | STREAM | tcp | bandwidth | 0.9885 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tcp | latency | 1.0152 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tcp | latency_p95 | 1.0078 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tcp | latency_p99 | 1.0095 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tcp | throughput | 0.9885 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tls | bandwidth | 0.9977 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | tls | throughput | 0.9977 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | bandwidth | 0.9943 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | latency_p95 | 1.0031 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | latency_p99 | 1.0254 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | ws | throughput | 0.9943 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | bandwidth | 0.9937 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | latency_p95 | 1.0065 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | latency_p99 | 1.0295 | 미실행 | 미실행 | 아니요 (signature 변경만) |
| multi | STREAM | wss | throughput | 0.9937 | 미실행 | 미실행 | 아니요 (signature 변경만) |

## §5.1 candidate 결과

`hotpath_gate`는 PASS였다. 로그는 `logs/hotpath_gate.log`이다. `test_wake_invariant*`에 `--repeat until-fail:10`을 적용한 CTest는 세 테스트 모두 PASS였다. 로그는 `logs/wake_invariants.log`이다. build 및 test preflight는 각각 `build-preflight.jsonl`, `test-build-preflight.jsonl`, `test-preflight.jsonl`에 있다. 작업 디렉터리는 25 GB 제한 안이었다.

## 재사용 스크립트 중복 점검

Core build와 harness build의 메모리, 디스크, load preflight가 반복된다: `/root/jobs/fx/cx/core123-gate/build-core.sh:10-20`, `/root/jobs/fx/cx/core123-gate/build.sh:8-18`.

benchmark 실행, argv 구성, 시작·종료 시각 기록, 종료 코드 확인이 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:69-137`, `/root/jobs/fx/cx/core123-gate/aa-run.sh:12-28`, `/root/jobs/fx/cx/core123-gate/confirm-run.sh:6-24`.

comparator 실행과 종료 코드 처리도 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:138-145`, `/root/jobs/fx/cx/core123-gate/aa-run.sh:30-36`, `/root/jobs/fx/cx/core123-gate/confirm-run.sh:25-30`.

Core prefix의 ldd/RUNPATH 확인은 benchmark preflight와 별도 verifier에서 겹친다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:75-80`, `/root/jobs/fx/cx/core123-gate/verify-builds.py:15-24`. 중복은 수정하지 않았다.

## 원시 결과

runner 내부 결과도 `raw-runner-results/1.23/` 및 `raw-runner-results/1.24/`에 복사해 보존했다. B2와 runs 5 확인은 preflight 조건 미통과로 미실행이다.

B2 재개용 `resume-b2.sh`는 기존 stage argv와 측정 조건을 유지해 B2-single, B2-multi만 잇도록 두었다. B2-single이 재개 preflight 제한에 다시 도달해 benchmark는 실행하지 않았다.
