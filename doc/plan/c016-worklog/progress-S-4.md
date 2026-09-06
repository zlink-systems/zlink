# S-4 진행 (STREAM raw gather 죽은 경로 제거)

- 설계: connection_fastpath_policy_t 가 gather 결정 전부 소유(transport 능력 + protocol 능력 + env).
- dev 빌드 OK. ctest -R 'stream|engine|asio|raw' 24/24 5회 통과. zmp 7/7 통과.
- release --lib-only 빌드 OK. with_stream 측정 대기(머신 조용해질 때까지).
- with_stream 측정: s10 이 PERF_LOCK 보유 중이라 대기(내 flock pid 113679). 시작 load 1.70.
- 보고서 초안 doc/plan/c016-worklog/core-rf-S-4-summary.md 작성 완료(성능표만 비어 있음).
- with_stream 2회 측정 완료(둘 다 load 10+ 경합, asio 레퍼런스도 붕괴 → 판정 불가). 측정 중단.
- 보고서 core-rf-S-4-summary.md 완성. 종료.
