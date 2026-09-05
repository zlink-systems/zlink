# C++ durable replay Stage 2 round 2 결과

## 결과

**BLOCKED — 검증용 Core·C++ binding 설치 package 준비 필요.**
조사 기준은 `main`, HEAD `0a46daf0edd5d991814a91a3ee3b7d832cf9e535`다.
Framework CMake가 설치 package를 소비하므로 이번 작업 지시의
“if the framework only consumes the installed `.artifacts/wsl/install/zlink-cpp`
package, STOP and report” 조건에 따라 구현과 빌드 전에 중단했다.

정적 조사에서는 completion 단계의 `NOT_CONNECTED`가 **현재 replay되지 않음**을
확인했다. Core의 즉시 completion 계약으로 round 1의 deadline 전제 문제는 해결됐지만,
C++ adapter의 terminal 매핑 수정과 실제 Core 회귀 검증은 남아 있다.

- 소유 계층: Core가 handover pair 선택과 해당 request의 즉시 종결을 소유하고, C++ Framework adapter가 typed 결과 변환을, Framework durable operation runtime이 같은 operation의 replay를 소유한다.
- Spec 조항: Core socket `core/doc/spec/core/socket/README.ko.md:159-167` (§4 handover, `NOT_CONNECTED`/`EHOSTUNREACH` 즉시 종결); Actor model `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680` (durable replay 주체, OperationId 유지, 전체 deadline).
- 교차언어 대조: Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSubmitFaults.java:28-55`는 session-actor/bound-session bind의 request `NOT_CONNECTED`를 retry 대상으로 보존한다. C++는 typed completion을 raw enum으로 바꾸는 adapter에서 `failed`로 축약하는 차이가 있다. Java의 timeout retry 범위를 C++에 적용하지 않는다.
- 변경 분류: 승인된 **A — 계약 적응**; package 준비 조건으로 구현 보류. Runtime 변경 없음.

## 매핑 조사

| 경로 | 현재 동작과 근거 |
| --- | --- |
| Initial submit `NOT_CONNECTED` + `ENOTCONN`/`EHOSTUNREACH` | `raw_binding_adapter.hpp:98-108`의 `transient_route_failure()`가 true를 반환하고, `raw_route_port.cpp:152-164`가 `route_unavailable`로 변환한다. |
| Request completion `NOT_CONNECTED` + `EHOSTUNREACH` | `raw_route_port.cpp:172-177`이 `map_binding_request_result()`를 호출한다. `raw_binding_adapter.hpp:79-91`에 `not_connected` 분기가 없어 `failed`가 된다. Typed result와 errno는 failure metadata에 남지만 replay predicate는 그 metadata를 읽지 않는다. |
| Durable replay predicate | `raw_mesh_node_owner.cpp:227-241`은 `route_unavailable`만 replay하고 `failed`는 `transport_failed`로 종결한다. 따라서 현재 predicate만으로 handover row가 처리된다고 주장할 수 없다. |
| Admitted request `TIMED_OUT` | Adapter `:85-86`이 `timed_out`을 보존하고 owner `:235-237`이 terminal로 처리한다. 이번 요청대로 유지할 대상이다. |
| Deadline와 식별자 | Owner `:195-204`는 남은 deadline 전부를 request에 전달하며, `:268-286`은 원래 deadline 안에서만 retry한다. `_operation`과 `_correlation`은 retry state의 동일 필드(`:294-297`)다. 실제 handover 후 성공은 미검증이다. |

위 C++ 파일 경로는 각각
`framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp`,
`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp`,
`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp`다.

재개 시에는 request completion의 typed `NOT_CONNECTED`를 replay 가능한 결과로
변환하는 경계를 수정해야 한다. Initial admission과 submit completion의 구분을 없애거나,
completion의 `ENOENT`를 일반적인 route 부재로 바꾸는 수정으로 확장하지 않는다.

## BLOCKERS

`framework/languages/cpp/CMakeLists.txt:58-63`은 C++ binding과 Core의 **설치 prefix**를
받으며 기본값은 `.artifacts/wsl/install/zlink-cpp/0.17.0`과
`.artifacts/wsl/install/zlink-core/0.17.0`이다. `:85-90`은 standalone configure에서
`find_package(zlink_cpp 0.17.0 EXACT CONFIG REQUIRED)`를 사용한다.
`CMakePresets.json`에도 Core build tree를 직접 사용하는 구성이 없다.

현재 설치된 `zlink_cppConfig.cmake:28`은 `find_dependency(zlink 0.17.0 EXACT CONFIG)`를
호출하고, `zlink_cppTargets.cmake:59-64`는 설치된 static binding과 `libzlink`를
연결한다. Framework의 native runtime 경로도 `CMakeLists.txt:80-82`에서 Core 설치
prefix의 `lib/libzlink.so`로 정한다. Prefix 변경은 가능하지만 설치 package가 필요하다.
따라서 지정 worktree에서 `scripts/build-core.sh dev`만 실행하는 것으로 요청된
Framework package 준비를 대체하지 않았다.

감독이 새 handover Core commit을 포함하는 Core·C++ binding 설치 package를 준비하고
그 prefix를 제공해야 한다. 이후 별도 `framework/languages/cpp/build/linux-ninja-replay`를
configure하고 실제 로드한 Core를 확인한 뒤 매핑 수정, 실제 reciprocal handover 회귀와
요청 검증을 진행할 수 있다. Worktree checkout, Core build, package 교체는 실행하지 않았다.

## Diff와 검증 결과

| 항목 | 결과 |
| --- | --- |
| 변경 파일 | 이 보고서만 추가 |
| C++ runtime·회귀 test | 미수정 — STOP 조건 적용 |
| Core worktree build·replay configure | 미실행 — 설치 package 준비 필요 |
| 실제 Core에서 동일 OperationId/correlation의 handover replay 회귀 | 미구현·미실행 |
| 4개 unit binary | 미실행 |
| `ctest -R 'm6a\|m6b\|channel_messaging\|execution\|raw_route_port'` | 미실행 |
| `git diff --check` 및 새 보고서 whitespace 검사 | 통과 |

기존 .NET·Node 변경과 untracked 디렉터리는 보존했다. Assertion과 deadline을 변경하지
않았고 commit하지 않았다. 실행한 runtime test가 없어 새 통과·실패 결과는 없다.
