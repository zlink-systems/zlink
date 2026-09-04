## 2026-09-04 시작

- detached worktree와 기존 사용자 변경을 확인했다. 요청에 따라 checkout/commit/push 없이 현재 worktree에서 진행한다.
- 변경 범위는 `bindings/cpp/**`로 제한하고, Core 스냅샷과 다른 바인딩의 변경은 읽기 전용 근거로만 사용한다.
- C++ send 구현, 계약 테스트, 문서·gate 경로를 병렬로 정찰 중이다.

## 2026-09-04 기준 빌드

- `ulimit -v 16777216; bash scripts/build-core.sh dev`를 실행했다.
- 진행 중인 Core 스냅샷이 `core/src/runtime/sockets/router/router_send_path.cpp:641`의 미정의 `copy_routing_id_from_bytes`에서 컴파일 실패했다. 바인딩 범위 밖이므로 수정하지 않고, 확정 D-B79 B 계약을 기준으로 C++ 구현을 계속한다.

## 2026-09-04 구현·1차 통합

- C++ async send entry가 operation state와 payload를 소유하고, 최초/재시도 모두 Core DONTWAIT로 제출하도록 변경했다. 정상 admission은 ID 0에서 즉시 완료하고, BACKPRESSURED/EAGAIN/nonzero token은 동일 WRITABLE(token/context/RID)까지 유지한 뒤 같은 payload를 재전송한다.
- send retry는 public poller(`POLLOUT | POLLCOMPLETION`)만 구동한다. pending send가 있으면 REQUEST용 fallback completion thread를 중지하고 재시작하지 않는다. REQUEST completion join 경로는 유지했다.
- raw public API HWM→BACKPRESSURED token→peer drain→POLLOUT/WRITABLE→retry 및 high-level async public-poller 테스트를 추가했고, WRITABLE enum ABI 노출을 고정했다.
- Core 스냅샷 갱신 뒤 `bash scripts/build-core.sh dev`를 재실행해 `libzlink.so` 생성까지 성공했으나, 새로 남은 Core 미정의 symbol `zlink::socket_base_t::fail_all_send_writable(int)` 때문에 Core test executable 링크에서 실패했다(바인딩 범위 밖, 미수정).
- 생성된 `core/build-dev/lib/libzlink.so`를 명시한 C++ test build는 바인딩 소스 컴파일까지 성공했으나 같은 Core 미정의 symbol 때문에 test executable 링크에서 실패했다.

## 2026-09-04 최종 통합·gate

- Core 스냅샷이 파일 timestamp를 보존한 채 바뀌어 이전 object가 남아 있음을 확인했다. `cmake --build core/build-dev --target clean` 뒤 `ulimit -v 16777216; bash scripts/build-core.sh dev`를 실행했으며, 현재 Core 소스로 dev build가 완료됐다.
- `ZLINK_CORE_SOURCE=local`, `ZLINK_CPP_CORE_BUILD_DIR=core/build-dev`와 해당 `LD_LIBRARY_PATH`를 지정해 `bindings/cpp/tests/run_tests.sh`를 실행했다. contract test 15/15와 sample smoke 7/7이 통과했다.
- 변경된 contract test 5개를 같은 dev library로 5회 실행해 25/25가 통과했다. 문서 주석 최종 반영 뒤 해당 test 5/5와 전체 contract label 15/15도 다시 통과했다.
- `git diff --check`, test/sample script `bash -n`, runtime의 `ZLINK_COMPLETION_SEND` 부재 검사, `core/include`와 C++ raw header mirror 8개 `cmp`가 모두 통과했다.
- README의 Doxygen 생성 명령은 환경에 `doxygen` executable이 없어 실행하지 못했다. public header를 포함한 C++ compile/test 검증은 완료됐다.
- commit, push, checkout과 `scripts/local-package/**`는 실행하지 않았다.
