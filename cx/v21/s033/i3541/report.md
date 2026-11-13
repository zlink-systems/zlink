# #3541 Core connect session/connecter I/O thread selection

## 결정 소유

Core spec `core/doc/spec/core/systems/03-io-thread.ko.md:144-150` states: “Transport connection(연결·수락된 session)은 후보를 round-robin으로 고른다.”

| 상태 | 결정 지점 | 동작 |
|---|---|---|
| Before | `core/src/runtime/sockets/common/socket_base_endpoint.cpp:533` | Connect session의 I/O thread를 round-robin으로 선택 |
| Before | `core/src/runtime/core/session_base.cpp:854` | 같은 connect의 connecter I/O thread를 round-robin으로 다시 선택 |
| Intended after | `socket_base_endpoint.cpp:533` | connect당 유일한 선택 지점으로 유지 |
| Intended after | `session_base.cpp:start_connecting` | session이 이미 소유한 `_io_thread`를 connecter에 전달 |

수정 전 판정 지점은 2곳, 수정 후 목표는 1곳이다. 새 option이나 상태는 필요하지 않다. Runtime 수정은 아직 적용하지 않았다.

## 구현 및 검증 상태

- Branch: `fix/3541-core-io-thread-choice`.
- Core unit test target `unittest_connect_io_thread_choice`를 추가하고 CMake unit 목록에 등록했다. 테스트는 4 I/O thread에서 100개의 순차 SUB connect session을 만들고 실제 session `_io_thread`별 수를 검사한다. 현재 Core configure에서 TLS 지원도 활성화되어 TCP와 TLS case를 포함한다.
- 테스트 초기 구성은 listener가 없어 연결 거절 뒤 session이 제거됐다. 보존된 GDB 실행에서 TCP/TLS 모두 `0/0/0/0`으로 집계됐고 TLS 종료 timeout도 확인했다. 이에 실제 PUB listener에 연결하는 구성으로 수정했다. 수정된 테스트 source는 아직 build-slot 대기 때문에 빌드·실행되지 않았다.
- 수정 전 첫 실행은 SIGSEGV(exit 139)였으며 raw output은 `/root/jobs/fx/cx/v21/s033/i3541/raw/pre-fix-negative.log`에 보존했다. 이 실행은 유효한 negative control로 간주하지 않는다. 유효한 baseline assertion 실행이 남아 있다.
- `bash scripts/format/format.sh --check cpp`: 통과(exit 0). Core 테스트 파일 직접 검사 `clang-format-18 --dry-run -Werror`: 통과. `git diff --check`: 통과.
- Runtime 수정, 50회 반복, 전체 Core unit suite, production Core build, commit 및 build-dir 정리는 미완료다.

## 외부 차단

모든 Core/C++ 컴파일은 지정된 `/root/jobs/fx/cx/cpp-build-slot.sh`로만 요청했다. 여러 worktree의 측정 job이 `/root/jobs/fx/cx/measure.lock`을 순차 사용해 wrapper가 `measure.gate`에서 55분 이상 대기했다. gate/lock은 직접 변경하거나 우회하지 않았다. lock 해제 후 baseline negative control부터 이어서 실행해야 한다.
