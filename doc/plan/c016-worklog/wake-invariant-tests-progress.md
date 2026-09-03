# Wake/activation invariant contract tests — progress

## 2026-09-04 시작 상태

- 지정된 `/home/hep7hep7/project/zlink`의 `perf/phase2-judge` branch에서 직접 작업한다.
- 기존 `core/src/**`, `core/tests/CMakeLists.txt`, `core/tests/perf/**` 변경은 병렬 job 소유이므로 보존한다.
- 수정 범위는 `core/tests/integration/test_wake_invariants*.cpp`, `core/tests/CMakeLists.txt`, 이 진행 파일과 최종 요약으로 제한한다.
- 계약 근거를 확인했다: `core/doc/spec/core/05-polling.ko.md` §3, `1344022a3e`, `f3be895b3f`, D-036·D-039·D-051~053, phase 2 진행·fixup 요약.
- 기존 결정적 회귀를 확인했다: planned HWM shrink의 8C→4C·2C LWM activation은 `test_router_multiple_dealers.cpp`에 있고, completion-owner/HWM 회귀는 phase 3 contract test에 흩어져 있다.
- 구현 방침: 새 integration executable 하나에서 public poll/poller 계약을 검증하고, planned HWM shrink는 기존 executable의 결정적 내부 pipe test도 gate에 포함한다. 동기화에는 poller·monitor event·condition variable을 쓰고 고정 `sleep`은 쓰지 않는다.

## 2026-09-04 구현·집중 검증

- `core/tests/integration/test_wake_invariants.cpp`를 추가했다.
  - monitor close 직후 첫 DATA: D/R inproc(`zlink_poll`), D/R tcp(`zlink_poller_wait`), R/R count-2 inproc(`zlink_poller_wait`).
  - HWM `EAGAIN` 뒤 peer drain: monitor owner 해제/유지 양쪽에서 실제 poller wait-active를 확인한 다음 POLLOUT wake를 검증한다.
  - 일반 반복: PAIR·DEALER·ROUTER·SUB × inproc·tcp·ipc에서 각 200회의 false→true POLLIN 전이를 검증한다.
- poller가 실제 대기 중인지 준비 flag로 추정하지 않는다. 동시 `zlink_poller_size()`가 `ZLINK_CONFIG_BUSY`를 반환한 뒤에만 peer가 readiness 전이를 만든다.
- `core/tests/CMakeLists.txt` 끝에 별도 블록을 한 번 추가했다. 기존 hotpath 블록을 보존했다.
  - 새 public matrix executable.
  - 기존 `test_router_multiple_dealers`의 8C→4C·2C LWM case를 포함하는 focused gate.
  - 기존 phase 3 completion-owner HWM cycle case만 선택하는 focused gate.
- `core/build-wk`가 없어 Release로 새로 구성했다. 관련 세 target 빌드 성공.
- 집중 결과: `test_wake_invariants` PASS(3/3), `ctest -L wake-invariant -j1` PASS(3/3).

## 2026-09-04 최종 gate

- `cmake --build core/build-wk -j3`: PASS.
- `ctest -L wake-invariant -j1`: 3/3을 5회 반복해 모두 PASS.
- `ctest --test-dir core/build-wk -j1 --output-on-failure`: 138/138 PASS, 173.18초.
- CMake tracked diff의 `git diff --check`와 신규 파일의 `git diff --no-index --check /dev/null ...`에서 whitespace 진단이 없었다.
- 새 테스트 파일에서 `sleep`·`msleep`·`test_sleep` 호출이 없음을 확인했다.
- 남은 실패 없음. git commit·push와 perf 측정은 실행하지 않았다.
