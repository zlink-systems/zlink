# Core close 후 completion poller 해제 검증

`ensure_completion_processing()`의 owner 획득 경로에 기존
`socket_public_api_scope_t`를 적용했다. Close가 먼저 승인되면 `ESHUTDOWN`으로
owner 획득을 거부한다. Owner 획득이 먼저 진입하면 quiescence 대기와 executor 시작이
끝날 때까지 기존 public admission이 close 승인을 막는다. 이미 안정된 owner를 사용하는
fast path는 그대로다. 새 상태·락·타이머·옵션·helper와 public API 변경은 없다.

작업 위치는 `/home/hep7/project/zlink-core-b`, detached HEAD는 `a170b187ad`이며
`7cbf12de41`을 포함한다. Diff는 이 worktree에 미커밋 상태로 남겼다. 이 보고서만 main에
작성했다. Main Core build, `zlink-core-a`, binding·Framework 소스와 spec은 수정하지 않았다.

- **소유 계층:** Core socket lifecycle / completion owner 획득과 close admission.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md:604-624`의 `zlink_close`;
  `core/doc/spec/core/05-polling.ko.md:43-49` §3 및 `:127-136` §5;
  `core/doc/spec/core/01-context.ko.md:47-61` §3.
- **교차언어 대조:** [승인된 진단](diag-cpp-sample-cleanup-137.md)의 Java 정상 종료 관측을
  사용했다. .NET·Node는 runner exit 0만으로 정상 종료를 입증하지 못했다. 이번에는 공통 Core만
  수정했으며 C++ 샘플의 실제 process 종료를 검증했다. 다른 언어 runtime 수정은 없다.
- **변경 분류:** B — `7cbf12de41`의 monitor lease 동작으로 노출된 기존 Core 결함.

**원인 위치와 변경 범위**

| 위치 | 원인 또는 최종 변경 |
|---|---|
| `core/src/runtime/sockets/common/socket_base_dispatch.cpp:263-296` | 마지막 completion poller 해제가 resume을 호출하는 경로. 기존 ownership 해제 절차 유지 |
| `core/src/runtime/sockets/common/socket_base_api.cpp:926-950` | Pending WRITABLE wait가 있으면 `ensure_completion_processing()` 호출 |
| `core/src/runtime/sockets/common/socket_base_dispatch.cpp:124-130` | 기존 admission scope 추가: 주석 포함 7줄. Close와 owner 획득을 같은 기존 gate로 직렬화 |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:872-909` | Executor 설치를 lifecycle coordinator로 넘기는 경로. 변경 없음 |
| `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:570-590,644-655` | 재시작이 완료 상태를 false로 되돌려 close waiter를 대기시키던 위치. 상태 및 wait predicate 변경 없음 |
| `core/tests/integration/test_close_completion_poller_release.cpp:21-110` | 공개 C API 회귀 테스트 2개 추가. 각 20회 반복 |
| `core/tests/CMakeLists.txt:116` | `test_close_completion_poller_release` 등록 1줄 추가 |

락 밖에서 closing 상태만 읽는 대안은 검사와 executor 설치 사이의 close 승인을 막지 못한다.
기존 admission scope는 이 경쟁을 닫고 새 동기화 상태를 요구하지 않는다. Scope는
poller resume과 blocking completion pull이 공유하는 owner 획득 함수에 한 번만 적용했다.
Close waiter의 predicate를 바꾸는 대안은 executor와 reaper의 동시 소유 가능성을 남긴다.
`7cbf12de41`의 monitor command lease 동작은 유지했다.

**빌드와 공개 API 회귀**

빌드는 지정된 worktree에서 `nice -n 10 env JOBS=4 scripts/build-core.sh dev`로 수행했다.
최종 shared library는 `/home/hep7/project/zlink-core-b/core/build-dev/lib/libzlink.so.0.17.0`이다.
SHA-256은 `101a3ba9092d91272525158cb00e516bc849aa1036341c99bfa8ca83254f7c34`이다.
생성된 `progress.make`가 최대 0.062초 미래라는 clock-skew 경고가 있었지만, 수정 소스 컴파일과
shared/static library 재링크가 완료됐다. Shared library disassembly에서도 새 admission 호출을 확인했다.

회귀는 연결하지 않은 DEALER의 DONTWAIT send가 `BACKPRESSURED`와 nonzero WRITABLE token을
반환하는지 확인한다. Monitor 유무별로 close thread를 시작하고 poller의 단일 `POLLERR`를
확인한 뒤 poller를 파괴한다. Poller 파괴 후 close 결과를 최대 1초 기다리며, close와
context term의 성공도 확인한다. Sleep, 내부 symbol, failpoint는 사용하지 않는다.

- 수정 전: monitor가 있는 `iteration=3`에서 close가 반환하지 않아 회귀 테스트가 abort했다.
- 최종 수정 후: `test_close_completion_poller_release_with_monitor`와
  `test_close_completion_poller_release_without_monitor`, 각각 20회 × 5회 연속 통과.
  공개 API 시나리오 총 200회이며, 이후 전체 gate에서도 40회 통과했다.

**Gate 결과**

| 검증 | 결과 |
|---|---|
| 최종 코드 `ctest --test-dir core/build-dev --output-on-failure -j2` | 171개 중 169개 통과, 2개 실패, 222.37초 |
| `test_disconnect_progress_*` | 집중 검증 24/24 통과, 전체 gate에서도 통과 |
| `test_single_lane_* -j2` ×2 | 29/29, 29/29 통과 |
| C/C++/Go/Rust 공개 헤더 mirror | 12/12 동일 |
| `git diff --check` | 통과 |
| C++ binding contract / sample-smoke | 16/16, 7/7 통과 |
| Python binding / samples | 190 tests 및 4 subtests, 7/7 samples 통과 |
| `test_connect_rid_alias_binding` 단독 재실행 | 1/1 통과 |

전체 gate의 `test_connect_rid_alias_binding` 실패는 `:89`, `:138`의 bind에서 발생한
`EADDRINUSE`였다. 단독 재실행에서 통과했으며 fixture와 assertion은 바꾸지 않았다.
최종 코드의 전체 gate는 한 번 실행했다. 최종 수정 위치를 확정하기 전에 시작한 별도 전체
gate는 중단했으며 최종 판정에 포함하지 않는다.

`CONTRIBUTING.ko.md` §5의 binding 검증은 소스 디렉터리에 산출물을 쓰지 않는 동등한 명령으로
수행했다. C++은 `core/build-close-completion-release/cpp-binding`에 configure/build한 뒤
contract와 sample-smoke label을 실행했다. Python은 기본 interpreter에 pytest가 없어 격리
venv를 준비했고, native extension을 같은 작업 디렉터리에 빌드했다. 외부 extension 경로를
거부하는 성능 runner 검사 때문에 최종 검증은 tracked Python 파일 148개를 그대로 복사한
`python-snapshot/bindings/python`에서 extension을 해당 package 안에 배치해 실행했다.
원본 대비 148개 파일의 byte 일치를 확인했다. Runtime은 모두 이번 worktree library를 지정했다.

**Hotpath 실패 분리**

동일한 최종 static archive의 복사본에서 `socket_base_dispatch.cpp.o`만 HEAD 원본으로 교체하고,
같은 benchmark object와 링크 옵션으로 수정 전 benchmark를 만들었다. 원래 build 산출물은
교체하지 않았다. 아래 값은 instructions/message이며 두 실행 모두 기존 reference gate에 실패했다.

| Cell | Reference | 수정 전 HEAD | 최종 수정 | 수정 전 대비 |
|---|---:|---:|---:|---:|
| `dealer_dealer_inproc` | 3455.381 | 4476.026 | 4476.011 | -0.0003% |
| `dealer_router_reqrep_inproc` | 12054.895 | 14886.884 | 15147.101 | +1.7480% |
| `pair_inproc` | 2681.957 | 3531.272 | 3531.232 | -0.0011% |
| `router_router_tcp` | 2972.882 | 3855.914 | 3855.846 | -0.0018% |

관측한 기준 초과는 수정 전에도 존재한다. Reference 갱신이나 tolerance 변경은 하지 않았다.

**C++ 샘플 종료**

기존 main `framework/languages/cpp/build/linux-ninja-c-e2e` 바이너리와 공식 sample runner를 사용했다.
`ZLINK_CPP_BUILD_DIR`는 이 build 디렉터리, `LD_LIBRARY_PATH`와 `ZLINK_LIBRARY_PATH`는 이번
worktree library, `TMPDIR`는 `/dev/shm/zlink-tmp-cpp` 아래 실행별 하위 디렉터리로 지정했다.
Runner의 configure/build 전후 샘플 바이너리 SHA-256은 동일했다. `/proc/<pid>/maps`로 관찰한
39개 프로세스가 모두 이번 library를 로드했다.

| Sample | 실행 | Runner exit | 기록한 cleanup wait status | SIGKILL / 137 |
|---|---:|---:|---|---|
| TicTacToe | 1 | 0 | 5건 모두 0 | 없음 |
| TicTacToe | 2 | 0 | 5건 모두 0 | 없음 |
| TicTacToe | 3 | 0 | 5건 모두 0 | 없음 |
| Bingo | 1 | 0 | 7건 모두 0 | 없음 |
| Bingo | 2 | 0 | 7건 모두 0 | 없음 |
| Bingo | 3 | 0 | 7건 모두 0 | 없음 |

기존 message-flow/file log, discovery/host-stop trace를 사용했다. 외부 evidence wrapper는
runner가 실행 디렉터리를 삭제하기 직전에 파일을 복사했고, shell xtrace로 wait 결과를 보존했다.
Flow 파일 48개와 역할별 log, maps를 보존했다. Runtime·binding·Framework 로깅 소스 변경은 없다.

**증거 위치**

모든 실행 증거는 `/home/hep7/project/zlink-core-b/core/build-close-completion-release/`에 있다.
주요 파일은 `zlink-core-b-close-baseline.log`, `zlink-core-b-close-build-final.log`,
`focused-final.log`, `full-ctest.log`, `single-lane-{1,2}.log`, `alias-rerun.log`,
`hotpath-baseline/{commands.txt,gate.log}`, `cpp-binding-{contract,samples}.log`,
`python-binding-tests-final.log`, `python-binding-samples.log`, `sample-results.json`,
`sample-exits.json`, `sample-binary-hashes-{before,after}.json`이다.
샘플별 상세 증거는 `tictactoe-{1,2,3}/`, `bingo-{1,2,3}/`에 있다.

**BLOCKERS**

- 전체 gate는 `hotpath_gate` 때문에 green이 아니다. 같은 dev 구성의 수정 전 HEAD에서도 네 cell이
  reference를 초과한다. 이 기준 초과의 별도 판정이 필요하며 본 작업에서 reference를 바꾸지 않았다.
- Close 회귀, monitor progress, C++ 샘플 정상 종료에 남은 실패는 없다.
