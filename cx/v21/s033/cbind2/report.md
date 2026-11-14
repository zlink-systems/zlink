# C++ 벤치 HTTP bind 실패 종료

- 재현: source stats 포트를 점유하고 c4json와 같은 인자·CPU affinity로 실행. 수정 전 stats 충돌 10/10회, trigger 충돌 10/10회 `-11`(SIGSEGV); 수정 후 stats 충돌 10/10회 `2`(정상적인 비제로 종료). 로그·집계: `raw/before-stats-results.json`, `raw/before-trigger-results.json`, `raw/repeat-stats-results.json`.
- 스택: bind 실패와 `C++ bench HTTP host exited with rc=1` 뒤 종료 worker가 `framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp:249`에서 SIGSEGV. 경로: `channel_runtime.cpp:690` → `app.cpp:4514,4521`; 메인 스레드는 `app_state_t::~app_state_t`의 worker join (`app.cpp:303-304`). 전체 GDB stack: `raw/gdb-run-01.log`.
- 소유자·원인: 벤치 `stats_http_server_t::start()`가 완료된 실패 run을 보고 `stop()`을 호출 (`framework/bench/grpc/cpp/common/bench_stats_server.hpp:358-366`). `stop()`이 이미 종료된 run에 `request_stop()`을 보내 종료 worker가 닫힌 runtime을 읽었다 (`:369-374`). Framework 실패 처리 (`app.cpp:3462-3474,3493-3503`) 자체는 안전하며, 점유 포트에서 `app.run()==1` 후 파괴되는 테스트가 수정 전·후 모두 통과.
- 수정: 벤치 `stop()`에서 기존 `_run_done`이 false일 때만 stop 요청하고 worker를 join. 새 상태 없이 판정 위치 1곳을 유지. Framework HTTP startup 테스트 5/5 통과 (`raw/test-http-startup-before.log`, `raw/test-http-startup-after.log`); C++ format check 통과 (`raw/format-check.log`).
- 수정 후 빌드: rc 0, 바이너리가 수정 헤더보다 최신. SHA-256 `9a29eeb6b8e4b3099acd50abbca71b86ca19bdbe88198f72bb314792822902e2`; 상세 `raw/build-after.log`, `raw/build-after-meta.txt`.
- 로컬 커밋: `e2e18700ecc79852933a5d645dc1c5adfc367ca9`.
