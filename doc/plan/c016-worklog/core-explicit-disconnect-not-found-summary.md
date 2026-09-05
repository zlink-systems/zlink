# Core 명시적 연결 제거의 REQUEST 완료 수정

ROUTER→ROUTER와 DEALER→ROUTER의 admitted REQUEST가 local endpoint 또는 RID 제거 시
`ZLINK_REQUEST_NOT_FOUND`(102)로 정확히 한 번 완료된다. 원격 target close는 D-090 계약대로
`ZLINK_REQUEST_NOT_CONNECTED`(109)를 유지한다. 변경은 미커밋 상태다.

- 작업 트리: `/home/hep7/project/zlink-core-a`
- 기준 HEAD: `d3ea1e4223414bc2a28a6cafbabe72b77e1a15d8` (detached)
- 패치: `/home/hep7/project/zlink-core-a/core-explicit-not-found.patch`
- 로그·복사한 repro: `/home/hep7/project/zlink-core-a/explicit-removal-logs/`
- main에는 이 요약만 작성했다. main의 `core/build-dev`, spec, binding, Framework는 수정하지 않았다.

## 원인과 수정

다음 원인 위치의 line은 기준 HEAD를 따른다.

- `core/src/api/socket/socket_request_reply_pending_api.cpp:171`: endpoint matcher가
  `logical_rid.empty()`인 DEALER 요청만 correlation pipe로 검사해 ROUTER 요청을 제외했다.
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1009`: endpoint 제거는 REQUEST 완료를
  먼저 호출했지만 위 matcher 때문에 ROUTER 요청이 남았다. 이어지는 RID 순회도 blocking send
  wait만 끝내므로 REQUEST를 처리하지 못했다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:242`와
  `core/src/runtime/sockets/router/router.hpp:87`: RID 제거는 send wait 정리 후 pipe를 종료했다.
  `socket_request_reply_dispatch.cpp:439`의 logical RID REQUEST 완료 함수는 호출부가 없었다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:1717`: 남은 REQUEST를
  `pipe_peer_terminated()`가 D-090 규칙으로 소비해 109를 게시했다.

수정 후에는 모든 admitted REQUEST가 이미 보유한 Application pipe correlation lease로 제거
대상을 판단한다. Endpoint matcher의 socket 종류 제한을 제거했고, RID 제거는 기존 routing map
또는 peer 검색이 실제로 선택한 pipe를 matcher에 전달한다. RID 검색이 실패한 경우에는 REQUEST를
소비하지 않는다. 변경 후 호출 위치는 `socket_base_api.cpp:243`, `socket_base_routing.cpp:222`다.

완료 소유자는 기존 `socket_request_reply_dispatch.cpp:402`의
`fail_matching_pending_requests()`다. 이 함수가 동일 pending 저장소에서 mutex 아래 요청을
꺼내고 102를 게시한 뒤 호출부가 pipe를 terminate한다. 따라서 제거에 따른 pair 종료가 실행될
때는 해당 pending record가 없다. D-090 종료 함수와 timeout 정책은 변경하지 않았다.

Endpoint의 RID 후속 순회에 logical RID 완료 호출을 추가하는 대안도 검토했다. 이 대안은
DEALER와 ROUTER의 판정 기준을 계속 분리하고 별도 RID 사본을 필요로 한다. 대신 기존 correlation
lease를 재사용하고, 호출되지 않던 RID matcher를 선택된 pipe matcher로 교체했다. 더 이상
필요하지 않은 `pending_request_t.logical_rid` 필드와 submit 시 복사도 제거했다. 새 pending
index, pipe flag, timer, retry 또는 public API는 없다.

**수정 전/후 규칙 수:** 명시적 제거의 REQUEST 식별 규칙 2→1
(DEALER endpoint correlation / 별도 logical RID 사본 → 모든 요청의 correlation lease).
별도 pair 종료 규칙 1개를 포함하면 3→2이며, pending의 중복 RID 사본은 1→0이다.

## 소유권과 계약 대조

- **소유 계층:** Core socket REQUEST pending/completion 소유자. 연결 검색은 기존 socket
  endpoint/routing 소유자가 수행하고, terminal record 생성은 기존 완료 소유자가 수행한다.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md` §6 completion 표의
  endpoint·logical RID 명시적 제거 → NOT_FOUND 행(`:1147`), submit 시점 pair 종료 →
  NOT_CONNECTED 행(`:1151`), `zlink_disconnect`의 admitted REQUEST 종결 문단(`:873`).
- **교차언어 parity:** binding source를 읽어 확인했다. .NET
  `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs:227`은 `NotFound = 102`,
  `Runtime/Native/NativeTypes.cs:41`은 native completion을 같은 enum으로 수신하고
  `Runtime/Messaging/CompletionOwner.cs:1351`은 결과를 그대로 예외에 전달한다.
  Java `bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketEnums/RequestResult.java:10`,
  C++ `bindings/cpp/include/zlink/Contracts/Messaging/request_result.hpp:15`,
  Python `bindings/python/src/zlink/contracts/sockets/codes.py:56`도 이미 102→NotFound다.
  Binding·Framework 수정이나 실행 검증은 하지 않았다.
- **변경 분류:** B — 기존 Core 결함 수정. 계약 변경이나 상위 계층 보상은 없다.

## 변경 파일

모두 작업 트리 안의 경로다.

- `core/src/api/socket/socket_request_reply_pending_api.cpp`
- `core/src/api/socket/socket_request_reply_internal.hpp`
- `core/src/api/socket/socket_request_reply_dispatch.cpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/src/runtime/sockets/common/socket_base_routing.cpp`
- `core/tests/integration/test_request_explicit_removal_not_found.cpp`
- `core/tests/CMakeLists.txt`

새 테스트는 ROUTER/DEALER × TCP/inproc × endpoint 제거/RID 제거/target close의 12개 경우를
검증한다. 각 경우 target이 실제 수신한 REQUEST 3개에 대해 200ms 이내 terminal 결과,
completion ID·user context, 중복 없는 완료를 확인한다. 200ms는 기존 disconnect progress
통합 테스트와 같은 판정 기준이다. 이후 원래 REQUEST timeout(1초)을 지나도록 public poller로
관측해 pair 종료나 timeout이 두 번째 완료를 만들지 않는지도 확인한다. 준비 동기화는 public
CONNECTION_READY monitor event와 target의 REQUEST 수신을 사용하며 sleep은 추가하지 않았다.
CTest 전체 제한 30초는 12개 경우의 각 1초 중복 관측을 포함한다. Runtime timeout은 변경하지 않았다.

## 검증 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| 수정 전 새 Core 테스트 | 첫 ROUTER/TCP endpoint assertion에서 expected 102 / actual 109 재현. 초기 fixture의 poller 정리 누락으로 실패 후 teardown은 10초 timeout; 최종 fixture는 실패 시에도 poller를 정리한다 | `baseline-test.log` |
| 수정 전 복사한 공개 API repro | target close 109/1ms, source endpoint 제거 109/1ms | `baseline-repro.log` |
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, 작업 트리 자체의 dev library와 tests 빌드 | `build-final.log` |
| 지정 targeted 6개 | 6/6 PASS, 46.60초 | `targeted.log` |
| `ctest --test-dir core/build-dev -j2 --output-on-failure` | 173/174 PASS, 248.29초. 유일한 실패는 아래 dev hotpath 항목 | `full-ctest.log` |
| 새 테스트 `--repeat until-fail:5` | 5/5 PASS, 총 61.11초. 각 실행 12개 경우·36개 REQUEST 검증 | `repeat.log` |
| 수정 후 복사한 공개 API repro | target close 109/1ms 유지, source endpoint 제거 **102/1ms** | `fixed-repro.log` |
| `git diff --check` | PASS | 명령 실행 확인 |

Targeted 정규식은 job에서 지정한
`test_request_explicit_removal_not_found|test_router_reject_duplicate|test_socket_disconnect_boundary|test_phase3_request_reply_contract|test_socket_disconnect_progress_without_app_poll|test_zmp_request_reply_receive_transaction`을 사용했다.

전체 ctest에는 dev에서도 `hotpath_gate`가 등록되어 있어 실행 결과는 exit 8이다. 해당 gate의
instruction/reference 비율은 dealer_dealer_inproc 1.2954, dealer_router_reqrep_inproc 1.2597,
pair_inproc 1.3167, router_router_tcp 1.2970이었다. 이 job의 명시적 조건인 **hotpath_gate n/a on dev**로
판정하며 green으로 세지 않는다. `ENABLE_LTO=OFF` dev 빌드다. Gate/reference 변경이나 전체 gate
재실행은 하지 않았다. 그 외 기능·회귀·단위 테스트는 모두 통과했다.

## BLOCKERS

이 job의 Core 수정과 기능 검증에는 없음. 전체 ctest의 원시 결과는 hotpath 1건 실패이며,
위와 같이 작업 지시의 dev n/a 항목으로 기록한다. Release hotpath 성능 판정은 수행하지 않았다.

새 테스트는 `git add -N`으로 diff에 포함했고 다음 명령으로 패치를 만들었다. commit은 하지 않았다.

```bash
git diff HEAD > /home/hep7/project/zlink-core-a/core-explicit-not-found.patch
```
