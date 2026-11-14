# C++ 벤치 HTTP bind 실패 종료

- 재현: source stats 포트를 점유하고 동일 c4json 인자·CPU affinity로 실행 시 수정 전 10/10회 `-11` (SIGSEGV); trigger 포트 충돌도 10/10회 `-11`. 결과: `raw/before-stats-results.json`, `raw/before-trigger-results.json`.
- 스택: bind 실패와 `C++ bench HTTP host exited with rc=1` 뒤 worker가 `state_lane.hpp:249`에서 SIGSEGV. 경로는 `channel_runtime.cpp:690` → `app.cpp:4514,4521`; 메인 스레드는 `app_state_t::~app_state_t` (`app.cpp:303-304`)에서 종료 worker를 join. 전체 로그: `raw/gdb-run-01.log`.
- 원인 소유자: 벤치 `stats_http_server_t::start()`가 실패한 `run()`을 감지하고 `stop()` (`bench_stats_server.hpp:358-366`)을 부른 뒤, `stop()`이 이미 끝난 run에 `request_stop()` (`:369-374`)을 보냄. Framework의 실패 처리(`app.cpp:3462-3474,3493-3503`)와 app 파괴는 단독으로 정상이며 생산 경로 테스트로 확인.
- 수정: `stop()`에서 기존 `_run_done`이 false일 때만 `request_stop()`하고 thread는 join. 벤치 코드만 변경; Framework runtime은 수정하지 않음.
- Framework HTTP startup 테스트: 수정 전 5/5, 수정 후 5/5 통과. 새 `AppRunReturnsFailureAndCanBeDestroyedWhenPortIsOccupied` 테스트는 점유 포트에서 `run()==1` 및 app 파괴를 확인 (`raw/test-http-startup-before.log`, `raw/test-http-startup-after.log`). C++ format check 통과 (`raw/format-check.log`).
- 로컬 커밋: `e2e18700ecc79852933a5d645dc1c5adfc367ca9`.
- 수정 후 벤치 재빌드와 10회 재현: 지정 build-slot 대기 중이며 완료 후 결과를 추가할 예정.
