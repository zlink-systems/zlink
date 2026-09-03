# Wake/activation invariant contract tests — summary

## 결과

`core/doc/spec/core/05-polling.ko.md` §3의 level-triggered wake 계약을 `wake-invariant` CTest suite로 고정했다. readiness가 거짓에서 참으로 바뀌기 전에 poller의 `wait_active` 상태까지 확인하므로, 고정 sleep으로 thread 진입을 추정하지 않는다. 신규 suite와 재사용한 두 기존 race reproducer는 5회 반복과 전체 Core CTest에서 모두 통과했다.

## 케이스별 검증 대상

| 케이스 | 검증 대상 | 위치 |
|---|---|---|
| monitor close 뒤 첫 메시지 | CONNECTION_READY를 monitor로 확인하고 monitor를 닫은 직후 첫 DATA를 보낸다. D/R count-1 inproc은 `zlink_poll`, D/R count-1 tcp와 R/R count-2 inproc은 `zlink_poller_wait`가 2,000ms 전에 POLLIN을 반환해야 한다. | `core/tests/integration/test_wake_invariants.cpp:395`, `:463` |
| HWM 거절 뒤 POLLOUT 회복 | 단일 D/R inproc peer에서 DONTWAIT send가 실제 `EAGAIN`에 도달하고 POLLOUT이 거짓임을 확인한다. monitor를 닫은 조건과 열린 채 둔 async-owner 조건 모두에서 poller가 실제 wait-active가 된 뒤 peer를 drain하고, 2,000ms 전에 POLLOUT을 확인한다. | `core/tests/integration/test_wake_invariants.cpp:367`, `:481`, `:548` |
| planned HWM shrink 경쟁 | 기존 결정적 내부 pipe case를 suite에서 재사용한다. 8C를 채우고 reader target을 4C로 줄인 뒤 2C LWM만큼 drain했을 때 blocked writer activation이 정확히 한 번 발생해야 한다. | `core/tests/integration/test_router_multiple_dealers.cpp:1385`, `:2020` |
| completion-owner quiesce 직렬화 | 기존 focused case를 `ZLINK_TEST_CASE`로 재사용한다. public completion poller가 async owner를 quiesce한 상태에서 blocking request가 reply backpressure cycle과 교차해도, send timeout 전에 POLLCOMPLETION과 네 reply completion을 관측해야 한다. | `core/tests/integration/test_phase3_request_reply_contract.cpp:3456`, `:4436` |
| 일반 false→true wake 반복 | PAIR·DEALER·ROUTER·SUB의 수신 readiness를 inproc·tcp·ipc에서 조합별 200회 반복한다(총 2,400회). 각 반복은 이전 record를 drain해 POLLIN을 거짓으로 되돌리고, `zlink_poller_size()`의 BUSY 결과로 poller가 대기 중임을 확인한 뒤 다음 record를 보낸다. 실패 메시지는 socket type, transport, 반복 번호, 경과 ms, count/event/error를 포함한다. PUB은 SUB 조합의 producer로 포함된다. STREAM은 inproc·ipc를 지원하지 않아 세 transport 공통 matrix 대상이 아니다. | `core/tests/integration/test_wake_invariants.cpp:20`, `:581`, `:621`, `:706` |

CTest 등록은 `core/tests/CMakeLists.txt:1065`에 있다. 새 executable과 HWM/LWM 기존 executable, completion-owner focused 실행을 모두 `integration;regression;serial;wake-invariant` label로 묶고 network resource lock을 적용했다.

## 변경 파일

- `core/tests/integration/test_wake_invariants.cpp` — 신규 public poll/poller contract test.
- `core/tests/CMakeLists.txt` — 신규 executable과 기존 race reproducer 두 개를 wake suite에 등록.
- `/home/hep7hep7/project/zlink-work/c016/wake-invariant-tests-progress.md` — 진행 기록.
- `/home/hep7hep7/project/zlink-work/c016/wake-invariant-tests-summary.md` — 이 요약.

`core/src/**`, `framework/**`, `bindings/**`, `scripts/local-package/**`는 수정하지 않았다. 작업 도중 같은 worktree에 나타난 `doc/plan/c016-worklog/**` 변경은 병렬 job 소유이며 읽거나 수정하지 않고 그대로 두었다. git commit·push는 수행하지 않았다.

## 검증 결과

모든 명령은 먼저 `ulimit -v 16777216`을 적용했다.

- 구성: `cmake -S core -B core/build-wk -DZLINK_BUILD_TESTS=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release` — PASS.
- 전체 build: `cmake --build core/build-wk -j3` — PASS.
- wake suite: `ctest --test-dir core/build-wk -L wake-invariant --output-on-failure -j1` — 3/3 PASS × 5회. 각 실행 1.55~1.58초.
- 전체 Core: `ctest --test-dir core/build-wk -j1 --output-on-failure` — 138/138 PASS, 173.18초.
- diff 검사: CMake tracked diff의 `git diff --check`와 신규 파일의 `git diff --no-index --check /dev/null ...` — whitespace 진단 없음(no-index의 차이 자체에 대한 status 1은 정상).
- 남은 실패: 없음.
