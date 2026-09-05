# C++ durable replay Stage 2 round 3 결과

## 결과

승인된 **A — 계약 적응**을 구현했다. 실제 Core에서 handover로 종료된 request의
`NOT_CONNECTED`를 replay 가능한 `route_unavailable`로 변환한다. 기존 owner가 같은
OperationId·correlation·wire deadline으로 survivor에 재전송하고 원래 deadline 안에
성공하는 회귀가 통과했다. **전체 gate는 65/69 통과이며 아래 BLOCKERS가 남아 있다.**

- 소유 계층: Core가 reciprocal handover의 pair 선택과 즉시 request completion을, binding이 typed completion 전달을, C++ Framework adapter가 결과 변환을, durable operation owner가 replay를 소유한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:159-167` §4 (`bb730c654f`, 구현 `8b82d51b75`)와 `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-679` sender bullets — 동일 OperationId, 남은 deadline 전체 사용, route 부재와 admitted reply timeout의 종결 구분.
- 교차언어 대조: Java `ZLinkActorSubmitFaults.java:28-55`도 session-actor/bound-session bind의 request `NOT_CONNECTED`를 retry 대상으로 보존한다. C++의 typed→raw adapter가 이를 `failed`로 축약하던 구조적 차이를 수정했다. Java의 더 넓은 timeout retry 조건은 이식하지 않았다.
- 변경 분류: 감독이 승인한 **A — 계약 적응**. Owner predicate·retry 간격·deadline·public API 변경 없음.

## Diff

다음 경로는 저장소 root 기준이다.

| 파일 | 변경 |
| --- | --- |
| `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:87` | 기존 typed completion switch에 `not_connected → route_unavailable` 추가. Initial submit과 request completion의 구분을 주석에 명시한다. |
| `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:116` | 실제 inproc reciprocal ROUTER handover에서 losing pair의 admission, completion phase·typed result·errno, 20ms 미만 completion을 검증한다. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:2504` | 실제 TCP owner 간 Actor create를 losing direction에 admit한 뒤 reciprocal connect한다. 자동 replay의 전체 header 동등성과 원래 deadline 안의 성공을 검증한다. |
| `doc/plan/c016-worklog/stage2-cpp-replay-scope-3-summary.md` | 이 결과 보고서. |

`raw_mesh_node_owner.cpp:227-241`의 기존 `route_unavailable` predicate가 handover
completion을 처리한다. `:195-204,268-286`의 남은 deadline 계산과 exhaustion mapping은
그대로다. `c835f5b325`의 route 부재 → Unavailable, admitted reply timeout →
DeadlineExceeded 구분을 유지한다.

Adapter의 typed mapping을 수정하는 방법과 owner에서 failure metadata를 다시 분류하는
방법을 비교했다. 전자는 기존 책임 경계에서 해결하며, 후자는 동일 결과의 분류를 두 곳에
두므로 채택하지 않았다. 새 errno table이나 predicate는 추가하지 않았다.

Core 계약의 errno는 `EHOSTUNREACH`지만 C++ binding
`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:43,435`는 typed
`NOT_CONNECTED`를 `ENOTCONN`으로 정규화한다. 첫 회귀 작성에서 Core errno를 직접
기대한 assertion이 실패해 이 경계를 확인했고, 최종 회귀는 binding의 정확한
`ENOTCONN`을 검사한다. Typed 결과로 분류하므로 errno별 별도 매핑은 필요 없다.
기존 assertion은 변경하지 않았다. Wait-token submit completion의 `NOT_FOUND`/`ENOENT`는
계속 `failed`이며, `timed_out`은 terminal이다.

## 검증 환경과 결과

`main`에서 지정한 `framework/languages/cpp/build/linux-ninja-c-e2e`를 사용했다.
`TMPDIR=/dev/shm/zlink-tmp-cpp`로 preset 재구성과 `cmake --build … -j4`를 완료했다.
설치된 Core와 `core/build-dev/lib/libzlink.so.0.17.0`의 SHA-256은 같다:

```text
a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3
```

`ldd`로 실제 로드 경로가
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0`임을 확인했다.
CMake는 `.artifacts/wsl/install/zlink-cpp/0.17.0`의 binding package를 사용한다.

| 검증 | 결과 |
| --- | --- |
| Configure / full build / 최종 test rebuild | 통과 |
| 실제 binding completion + 기존 wait-token ENOENT contract binary | 통과; handover completion **232µs**, `NOT_CONNECTED`/`ENOTCONN`, 20ms 미만 |
| Unit `m6a_runtime` | 통과 |
| Unit `m6b_runtime` | 실패; 새 handover replay는 **78.004ms**에 성공, 이후 기존 미승인 DEALER 요청에서 예외 |
| Unit `channel_messaging` | 통과 |
| Unit `execution` | 통과 |
| `ctest -R 'm6a\|m6b\|channel_messaging\|execution\|raw_route_port' --output-on-failure` | **4/5 통과**, m6b 동일 실패 |
| 기존 m6b 소스의 새 replay + 기존 bind outcome 사례 독립 실행 | 통과; replay **77.599ms**, reply-withheld → DeadlineExceeded, route 부재 → Unavailable |
| 전체 `ctest -j2 --output-on-failure` 1회 | **65/69 통과**, 359.41초; 실패 목록은 BLOCKERS 참조 |
| TicTacToe 별도 runner 1회 | **실패 (exit 1)**; `tictactoe=completed` 뒤 process 2개 강제 종료, 각 exit 137 |
| GameQuest 별도 runner 1회 | **통과 (exit 0)**; `sample all result=passed` |
| `git diff --check` | 통과 |

독립 실행은 build 디렉터리의 `replay-round3/focused_m6b.cpp`에서 기존 test translation
unit을 include하고 두 검증 함수를 호출했다. 원래 assertion과 compile flags를 유지했으며
새 test target이나 runtime hook은 만들지 않았다. 실제 owner의 기존 75ms retry 간격 때문에
replay 성공은 약 78ms이며, Core completion의 20ms 상한과 구분된다.
전체 gate에서도 새 replay는 **77.079ms**에 성공했다.

## BLOCKERS

- **m6b의 미승인 DEALER 요청:** `test_cpp_framework_m6b_runtime.cpp:5201-5241`의
  `verify_unadmitted_request_is_rejected_without_framework_queue()`가 native binding
  reply를 기다리다가 `Network is unreachable (errno=110)` 예외로 abort한다. 보존한 mesh
  trace는 `application-drop reason=peer-not-admitted action=reply-not-connected`까지
  진행한다. 이 요청은 `await_native_reply()`로 native binding을 직접 await하며 수정한
  adapter를 거치지 않는다. 새 handover 회귀와 기존 reply-withheld 사례는 따로 통과했다.
  이번 작업은 이 경로의 추가 원인 수정이나 기존 실패 여부 입증으로 확장하지 않았다.
- **알려진 C — inventory:** `test_cpp_framework_common_e2e_inventory`가 정확히
  `278 required inventory conditions are open`으로 실패한다.
- **Bingo cleanup:** 전체 gate에서 `session-a`를 강제 종료해 exit 137로 실패했다.
- **TicTacToe cleanup:** 전체 gate에서 `tictactoe=completed` 뒤 process 강제 종료와
  exit 137로 실패했다. 별도 runner 1회도 process 35789·35790의 cleanup에서 같은
  실패를 보였다(`samples/TicTacToe/run_sample.sh:204-218`). Application 완료와 runner
  통과를 구분해 기록한다. Cleanup의 하위 계층 원인은 이번 범위에서 판정하지 않았다.

Core·binding·다른 언어·보호 문서는 수정하지 않았다. 기존 및 병행 작업 변경은 보존했고
commit하지 않았다.

## 실행 증거

Sample은 다음 명령을 각각 한 번 실행했다. 기존 runner의 임시 디렉터리 삭제에 대비해
외부 실행 wrapper가 flow·process 로그를 복사했다. Sample source는 수정하지 않았다.

```bash
TMPDIR=/dev/shm/zlink-tmp-cpp \
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
bash framework/languages/cpp/samples/run_samples.sh TicTacToe
TMPDIR=/dev/shm/zlink-tmp-cpp \
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
bash framework/languages/cpp/samples/run_samples.sh GameQuest
```

증거 root는 `framework/languages/cpp/build/linux-ninja-c-e2e/replay-round3/evidence/`다.
원본은 `/dev/shm/zlink-tmp-cpp/replay-round3/`에도 남아 있다.

- `raw-route.log`: typed completion·errno·232µs 결과와 ENOENT contract binary 통과.
- `unit-*.log`, `focused.log`, `full-ctest.log`: 지정 unit·focused·전체 gate 결과.
- `focused-m6b.log`: 실제 replay와 기존 admitted reply timeout·route 부재 사례의 독립 통과.
- `sample-results.log`, `TicTacToe.log`, `GameQuest.log`: 별도 runner exit와 결과.
- `TicTacToe-artifacts/`, `GameQuest-artifacts/`: 첫 별도 실행부터 보존한 flow·process 로그.

