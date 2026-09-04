# C++·Node DONTWAIT refactor pass 결과

## 결과

0.17.0 DONTWAIT campaign이 건드린 C++·Node 범위를 POSDDD의 정보 소유권, 반복,
hot path 비용과 dead diagnostics 기준으로 검토했다. 공개 API와 assertion은 변경하지 않았다.
`framework/languages/node/test/contract/fixtures/node-public-contract.json`의 SHA-256은 작업 전후
모두 `f829a1b953664efe977f07e9add6bcb2d0e8bfae734a1e057e3583e714b56dc8`이다.

## C++ 파일별 결과

| 파일 | 변경 | 분류 | 동작이 같은 이유 |
|---|---|---|---|
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | stale same-endpoint를 판정하는 lambda를 `any_of`와 `erase_if`가 함께 사용하도록 합쳤다. | POSDDD | 같은 predicate를 같은 container와 같은 시점에 적용하며, 검색·삭제 순서와 예외 시 상태 보존은 그대로다. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.hpp` | 변경 없음 | 검토만 | campaign 구현에 대응하는 새 중복 상태나 helper가 없었다. |
| `framework/src/runtime/channels/channel_outbound_exchange.cpp` | 중복된 `poller.hpp` include 하나를 제거했다. | dead-code | 동일 header가 바로 아래에 계속 포함되므로 전처리 결과가 같다. |
| `framework/src/runtime/backend/raw_route_port.cpp` | 제거된 errno 임시 계측과 과거 test 변경을 설명하던 장문의 주석을 제거했다. transient route 규칙은 `transient_route_errno`에 남겼다. | dead-code, POSDDD | 실행문은 바뀌지 않았고 ENOENT 분류의 소유자는 `raw_binding_adapter.hpp` 한 곳으로 유지된다. |
| `framework/src/runtime/backend/raw_dealer_port.cpp` | 변경 없음 | 검토만 | async send progress poll 등록에 불필요한 할당·복사가 추가되지 않았다. |
| `framework/src/runtime/streams/stream_host_service.cpp` | 변경 없음 | 검토만 | poll 등록은 binding completion을 진행시키는 데 필요하며 hot path에 새 payload 복사가 없었다. |
| `framework/src/runtime/backend/raw_binding_adapter.hpp` | 변경 없음 | 검토만 | ENOENT/NOT_FOUND를 포함한 transient route errno 규칙이 이미 이 내부 경계 한 곳에 모여 있다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp` | 변경 없음 | 검토만 | 새 assertion과 completion poll 진행은 계약을 직접 검증하며 제거·완화할 항목이 없었다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp` | 임시 계측 결과, 과거 assertion 변경 이력과 미확인 parity 메모를 현재 동작을 설명하는 두 줄로 바꿨다. | dead-code | assertion과 test 실행문은 그대로이며 주석은 현재의 bounded retry 결과만 설명한다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp` | 변경 없음 | 검토만 | campaign 변경과 관련된 공용화 가능한 in-file 중복이 없었다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp` | 변경 없음 | 검토만 | campaign 변경과 관련된 dead branch나 불필요한 hot-path 작업이 없었다. |

표의 C++ 경로는 `framework/languages/cpp/` 기준이다.

## Node 파일별 결과

| 파일 | 변경 | 분류 | 동작이 같은 이유 |
|---|---|---|---|
| `packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts` | 변경 없음 | 검토만 | managed Spot send는 flags와 관계없이 binding async admission 한 경로를 사용하며 WRITABLE 재제출 책임을 중복 구현하지 않는다. |
| `packages/framework/src/runtime/foundation/service-stateful-runtime.ts` | 변경 없음 | 검토만 | 남은 deadline의 절반을 첫 시도에 배분하는 hunk는 terminal replay 예산을 보존하는 동작 정책이다. |
| `packages/stream-connector/src/Runtime/ZlinkStreamConnectorOptions.ts` | 변경 없음 | 검토만 | diagnostics level membership은 module-level `ReadonlySet` 하나가 소유하여 호출마다 Set을 만들지 않는다. |
| `samples/run-sample.mjs` | 변경 없음 | 검토만 | shared runner의 PASS marker는 sample 완료 뒤 한 번만 출력된다. |
| `test/contract/backend-contract.test.js` | 두 DONTWAIT Spot send test의 동일한 socket stub을 `wrapSpotSendSocket`으로 합쳤다. | POSDDD | 각 test가 같은 `submit` 객체를 같은 `message()` closure로 전달하며 call 순서와 assertion은 그대로다. |
| `test/contract/channel-client.test.js` | 변경 없음 | 검토만 | pull-completion 전환의 reply token·monitor drain 계약을 검증한다. |
| `test/contract/channel-malformed-reply.test.js` | 변경 없음 | 검토만 | malformed reply 계약의 reply token assertion을 유지했다. |
| `test/contract/client-server-location-runtime.test.js` | 변경 없음 | 검토만 | monitor drain과 reply token fixture는 pull-completion 계약에 필요하다. |
| `test/contract/documentation-regression.test.js` | 변경 없음 | 검토만 | 생성·공개 계약 회귀 검사를 약화하지 않았다. |
| `test/contract/fanout-location-runtime.test.js` | 변경 없음 | 검토만 | pull-completion fixture 변경 외에 공용화할 campaign 중복이 없었다. |
| `test/contract/fixtures/node-public-contract.json` | 변경 없음 | public contract | 작업 전후 byte hash가 같다. |
| `test/contract/native-artifact-freshness.test.js` | 변경 없음 | 검토만 | native artifact freshness gate를 그대로 유지했다. |
| `test/contract/node-binding-parity.test.js` | 변경 없음 | 검토만 | 0.17.0 binding surface parity assertion을 그대로 유지했다. |
| `test/contract/object-routing.test.js` | 변경 없음 | 검토만 | reply token 전환 계약을 그대로 유지했다. |
| `test/contract/sample-bingo-lifecycle-gate.test.js` | 변경 없음 | 검토만 | sample lifecycle assertion을 그대로 유지했다. |
| `test/contract/sample-bingo-routing-id-allocation-gate.test.js` | 변경 없음 | 검토만 | RoutingId allocation assertion을 그대로 유지했다. |
| `test/contract/sample-message-naming-contract.test.js` | 변경 없음 | 검토만 | sample message naming 계약을 그대로 유지했다. |
| `test/contract/sample-regression.test.js` | 변경 없음 | 검토만 | DeliveryDispatch deadline 위치를 확인하는 regression assertion을 그대로 유지했다. |
| `test/contract/sample-spot-lifecycle.test.js` | 변경 없음 | 검토만 | shared PASS marker와 `SampleTimings` 기반 lifecycle 검사를 그대로 유지했다. |
| `test/contract/sample-supportchat-message-semantics-gate.test.js` | 변경 없음 | 검토만 | SupportChat timing/message semantics assertion을 그대로 유지했다. |
| `test/contract/sample-zoneworld-gate.test.js` | 변경 없음 | 검토만 | pull-completion 이후 sample gate를 그대로 유지했다. |
| `test/contract/spot-manager.test.js` | 변경 없음 | 검토만 | reply token·async completion 계약을 그대로 유지했다. |
| `test/contract/stream-runtime.test.js` | 변경 없음 | 검토만 | binding-owned admission과 stream send flags 계약을 그대로 유지했다. |
| `test/contract/stream-session-runtime.test.js` | 변경 없음 | 검토만 | packet receive, permit, lifecycle assertion을 그대로 유지했다. |

표의 Node 경로는 `framework/languages/node/` 기준이다. `git log --since=2026-09-02`에 나온
contract 파일을 모두 검토했으며, DONTWAIT commit `4d263e66b9`가 직접 추가한 contract hunk는
`backend-contract.test.js`에만 있었다.

## Deferred

- `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:802`: 현재 구현은
  `any_of` 뒤 성공한 connect에서만 `erase_if`를 실행하므로 같은 map을 두 번 순회한다. connect 전에
  한 번에 지우면 실패 시 기존 기대 peer를 보존하는 동작이 달라지고, iterator 목록을 보관하면 이
  드문 control path에 할당이 생겨 적용하지 않았다.
- `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:57`,
  `raw_dealer_port.cpp:38`, `channels/channel_outbound_exchange.cpp:662`,
  `streams/stream_host_service.cpp:1566`: `pollout | pollcompletion` mask가 반복된다. 범위 안에 자연스러운
  공통 소유 header가 없으며 raw binding adapter에 두면 channel/stream이 backend 변환 helper에
  의존한다. 새 내부 header 추가도 승인된 파일 범위를 벗어나므로 보류했다.
- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:1517`와
  `test_cpp_framework_m6b_runtime.cpp:4452`: poll/pump-until-ready 형태는 유사하지만 대상 task와
  허용 pump 결과가 다르다. 공통 helper를 만들면 assertion 위치와 timeout 의미를 숨기거나 새 shared
  test 파일이 필요하므로 보류했다.
- `framework/languages/node/packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts:248`:
  `_flags`를 제거하거나 `async` wrapper를 직접 Promise 반환으로 바꾸면 함수 arity, Promise rejection
  경계와 stack이 관찰될 수 있다. public contract 변경이 아님을 입증하지 못해 보류했다.
- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4017`:
  `/ 2` 비율을 공통 상수로 올릴 두 번째 사용처가 없고 비율 자체를 바꾸면 replay 동작이 달라진다.
  현재 변수명과 인접 주석이 정책을 설명하므로 그대로 뒀다.

## 검증

모든 Node 명령은 `flock -w7200 /tmp/zlink-node-gate.lock` 안에서 실행했고
`ZLINK_LIBRARY_PATH`를 unset했다. Core와 local package는 다시 만들지 않았다.

| 명령 | 결과 |
|---|---|
| `mkdir -p /dev/shm/zlink-tmp-cpp && TMPDIR=/dev/shm/zlink-tmp-cpp cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j2` | 성공, 102/102 build step 완료 |
| `TMPDIR=/dev/shm/zlink-tmp-cpp framework/languages/cpp/build/linux-ninja-c-e2e/test_cpp_framework_m6a_runtime` | 성공 |
| `TMPDIR=/dev/shm/zlink-tmp-cpp framework/languages/cpp/build/linux-ninja-c-e2e/test_cpp_framework_m6b_runtime` | 성공 |
| `TMPDIR=/dev/shm/zlink-tmp-cpp framework/languages/cpp/build/linux-ninja-c-e2e/test_cpp_framework_channel_messaging` | 성공 |
| `TMPDIR=/dev/shm/zlink-tmp-cpp framework/languages/cpp/build/linux-ninja-c-e2e/test_cpp_framework_execution` | 성공 |
| `TMPDIR=/dev/shm/zlink-tmp-cpp ctest --test-dir framework/languages/cpp/build/linux-ninja-c-e2e --output-on-failure -R 'channel_messaging\|m6a\|m6b\|execution'` | 성공, 4/4 |
| `TMPDIR=/dev/shm/zlink-tmp-node npm run build` | 성공 |
| `TMPDIR=/dev/shm/zlink-tmp-node node --test --test-force-exit --test-timeout=600000 test/contract/backend-contract.test.js test/contract/stream-runtime.test.js test/contract/stream-session-runtime.test.js test/contract/sample-regression.test.js test/contract/node-binding-parity.test.js` | 성공, 301/301 |
| `TMPDIR=/dev/shm/zlink-tmp-node npm run lint` | 실패, 범위 밖 기존 `packages/framework/src/runtime/spots/spot-timer.ts:137`의 `@typescript-eslint/strict-boolean-expressions` 1건 |
| `TMPDIR=/dev/shm/zlink-tmp-node node node_modules/eslint/bin/eslint.js packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts packages/framework/src/runtime/foundation/service-stateful-runtime.ts packages/stream-connector/src/Runtime/ZlinkStreamConnectorOptions.ts` | 성공 |
| `git diff --check` | 성공 |

## BLOCKERS

- 전체 Node lint는 `framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:137`의
  기존 오류 때문에 green이 아니다. 이 파일은 승인된 범위 밖이므로 수정하지 않았다.
- 작업 시작 전부터 `bindings/node/provenance/core-package-provenance.json`, .NET runtime/test 파일 8개,
  untracked `opah/`, `zlink-work/`가 존재했다. 이 작업에서는 건드리지 않았으므로 repository 전체
  `git status`를 요청 범위 파일만 남은 상태로 만들 수 없다.
- 요청에 적힌 `framework/languages/cpp/AGENTS.md`와 `framework/languages/node/AGENTS.md`는 현재
  worktree에 존재하지 않았다. root `AGENTS.md`, `framework/AGENTS.md`, `doc/AGENTS.md`와 POSDDD 관련
  두 원칙 문서를 적용했다.
