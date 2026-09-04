# C multi perf policy 2차 진행 기록

- 2026-09-04 13:58 KST: 시작. candidate는 `cfc284f2e4` detached/clean이며 `main`과 같은 revision임을 확인했다. 수정 범위는 `bindings/c/perf/**`, 산출물은 이 작업 기록과 최종 요약으로 한정한다. baseline PUBSUB 계수 의미, one-way pending 경로, 실행 harness를 병렬 조사 중이다.
- 2026-09-04 14:20 KST: baseline `perf_multi_pubsub_client.cpp`를 확인한 결과 RESULT numerator는 PUB 제출 수가 아니라 모든 SUB socket의 active 수신 합이다. 649k에 CCU 100을 다시 곱하는 해석은 이중 계수다. baseline은 PUB `NODROP`을 설정하지 않아 lossy 기본값 0이고 candidate는 1이다.
- 2026-09-04 14:20 KST: DEALER/ROUTER initiator와 ROUTER relay의 단일 completion ID 상태를 unordered exact-ID/context set으로 교체했다. active cap은 `clamp(floor(1 MiB/msg_size),16,4096)`, latency logical in-flight 1은 유지했다. PUB active 종료 barrier에 최종 publish count를 실어 SUB별 leading/internal/trailing sequence gap을 `PUBSUB_DIAG`로 출력하도록 추가했다. 수정 후 C perf 전체가 `-j4`로 compile되고 metric executable이 통과했다(정식 검증 전 빠른 확인).
- 2026-09-04 14:11 KST: Python perf unittest를 두 suite로 실행해 12개와 47개, 합계 59개가 모두 통과했다. 현재 변경은 `bindings/c/perf/**`에만 있고 `git diff --check`도 통과했다. 앞 두 항목의 `14:20`은 실제 기록 시각보다 앞서 적힌 시각 오기다.
