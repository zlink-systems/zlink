# CCU-1 진행

- 시작: 조사 착수. 공통 규칙·pull-variants 요약 읽음.
- 다음: 스윕 결과 디렉터리(20260908_162232 등)에서 zlink CCU 4000 실패 증거 확인.
- 증거: 20260908_162232/benchmark.log — client rc=2, 16:22:32→16:22:41(9s). zlink server/client 로그 모두 0바이트.
- rc=2 경로 = perf_stream_bench_client.hpp:238 `connect_success <= 0` (4000개 connect 전부 실패). wait_for_port는 통과 → listen은 됨.
- 다음: PERF_DEBUG=1로 재현.
- 17:06 재현 실행이 PERF_LOCK 대기 중(다른 job 2개 보유). 대기하며 코드 분석.
- 클라이언트 connect timeout은 90 s인데 실제 9 s만에 rc=2 → 4000개 connect가 전부 즉시 실패(ECONNREFUSED 추정) = 서버 프로세스가 죽었을 가능성.
- zlink 서버는 stderr에 아무 것도 못 남김(로그 0바이트) → return 2 경로 아님. 시그널/크래시 의심.
- 17:11 재현 성공(20260908_171056): connect_ok=4000, connect_fail=0, send/recv_error=0, **timeout_error=1, samples=0, throughput=0, window_ok=0**.
  → 연결은 전부 되지만 3 s 액티브 윈도에서 **에코가 한 건도 오지 않음**. 서버 CPU ~100 %(1코어), RSS 134 MB.
  → 서버 로그가 빈 것은 러너가 SIGINT 후 1 s만에 SIGKILL하기 때문(poller_destroy가 늦으면 METRIC 미출력). 행(hang)의 증거는 아님.
- 다음: 수동 서버/클라이언트 프로브로 CCU 2000/3000/4000 임계와 스레드별 CPU 확인.
- 17:30 원인 확정(샘플러 13개 중 12개 동일 스택):
  zlink_poller_wait → socket_poller_t::wait → check_events → socket_base_t::get_events_internal
  → process_commands → object_t::process_command → **socket_base_t::attach_pipe**
  → **ctx_t::auto_hwm_recalculate_now** → ctx_physical_queue_registry_t::plan_application_queues/find_locked.
  즉 STREAM 연결 1건이 붙을 때마다 컨텍스트 전체 Auto-HWM 재계산(모든 physical queue 순회)을 동기로 수행 → O(N^2).
  메인 스레드 utime(초당 tick): 1000→231에서 포화, 2000→282, 3000→580, 4000→1048(12 s 시점에도 계속 증가). IO 스레드는 거의 0.
- 다음: worktree에서 동기 recalc를 debounce 예약으로 바꿔 CCU 4000 재측정(인과 검증).
- 17:50 인과 검증 완료: 실험 패치(attach 동기 recalc → debounce)로 CCU 4000 263.2 kops 통과, CCU 1000은 263→294 kops.
- 보고서 doc/plan/c016-worklog/core-rf-CCU-1-report.md 작성 완료. job 종료.
