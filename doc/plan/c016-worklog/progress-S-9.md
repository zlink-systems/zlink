# S-9 진행

- 22:48 worktree ~/project/zlink-work/s9 생성(detached 6f64e76b51), release --lib-only 빌드 시작.
- 읽기 경로 파악: on_read_complete → maybe_drain_stream_reads → speculative_read(EAGAIN) → start_async_read(asio 자체 speculative recv, EAGAIN) = 3 recv/msg 가설.
- 설계 후보: 짧은 read = 소켓 비었음 규칙(fastpath_policy의 기존 short-read 판정 재사용)으로 drain 진입/반복을 한 규칙으로 통합.
- 22:52 release --lib-only 완료(baseline lib). dev 빌드는 임시 계측(S9STATS: async full/short, spec data/EAGAIN) 포함으로 진행 중. bindings/c bench 빌드 중.
- 23:00 baseline callgrind 축소셀(CCU 20, 1024 B) 완료: recv 3.001/msg, speculative_read 1.000/msg, Ir/msg 11349 (S-A 3.000/11096과 일치 — 재현됨).
- 다음: 계측 dev lib로 read 크기 분포(async full/short, spec data/EAGAIN) 수집 → 변경 적용.
- 23:00 계측 실측(dev lib, CCU 20):
  - 1024 B: async_reads 223963, full 0, short 223963(100%). speculative_read 223963(1.00/msg) 중 데이터 반환 0, EAGAIN 223962 → 짧은 read 뒤 speculative는 100% 헛수고.
  - 65536 B: async full 59981 / short 22373. speculative 125561(2.03/msg) 중 data 43207(평균 62 B), EAGAIN 82353(1.33/msg).
- 23:01 변경 적용: "요청보다 적게 읽힌 read = 소켓 비었음" 규칙을 fastpath_policy에 stream_read_filled_request()로 두고 drain 진입/반복 조건과 read target 성장 판정이 공유. 계측 코드 제거. release 재빌드 시작.
- 23:03 after callgrind: recv 3.001 → 2.001/msg, speculative_read 1.000 → 0/msg, Ir/msg 11349 → 10967.
- 23:05 with_stream CCU 1000 run1: zlink 217.0/241.6/27.97 (asio 314.6/299.9/37.70), run2: zlink 261.2/241.1/29.59 (asio 316.5/269.7/37.39). 머신 노이즈 ±15%(asio 1024가 316→269로 흔들림).
- 23:09 ctest 55개 5회 전부 통과. TSan 트리 구성·빌드 시작.
- 23:23 TSan: 변경본/baseline 동일 트리 각각 1회 — 둘 다 WSL2 mapping FATAL + 기존 mailbox/ypipe race로 같은 실패. 차이 없음.
- 23:24 보고서 doc/plan/c016-worklog/core-rf-S-9-summary.md 작성 완료. 커밋 안 함.
