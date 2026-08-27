# CP3 C++ 결함 수정 및 발견 10 재감사 보고

## 결론

- 작업 1의 CP3 감사 [H] 2건은 모두 **[실증]**이다. readiness 대기 뒤 `close()`가 socket을 reset하고 기존 제출 블록이 이를 재검증 없이 사용할 수 있었다. 두 곳을 최소 fence로 수정했다.
- 작업 2의 발견 10(찢어진 캡처 블록)은 현재 C++ Framework production (`framework/languages/cpp/framework/src`, `include`)에서 **[C] 0 / [H] 0 / [M] 0 / [L] 0**이다. 요청에 따라 base 대조와 회귀 판정은 하지 않았다.
- 단위·계약 게이트는 45건 중 44건 통과, 1건 실패다. 실패는 알려진 `test_cpp_framework_layout_contract` (ShoppingMall blocking result 지문)이다.

## 1. 작업 1 — [H] 2건 정적 검증과 수정

### 1.1 `channel_native_client_t::request` — [실증]

호출 경로는 `message_bus_t::submit_request_message_async()` → outbound exchange
(`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1112-1123`) →
`channel_outbound_exchange_t::submit_request()`의 native client 확보와 `co_await`
(`runtime/channels/channel_outbound_exchange.cpp:1247-1265`) →
`channel_native_client_t::request()`이다.

request는 `_mutex` 아래 `transport = sync_connections(current)`을 capture하고
(`channel_outbound_exchange.cpp:375-385`) readiness를 기다린다(`:386-394`). 병행 close 경로는
host stop → `channel_runtime_t::shutdown()` (`runtime/channels/channel_runtime.cpp:566-583`) →
`close_native_channel_transports()` (`channel_outbound_exchange.cpp:916-941`) →
client `close()` (`:555-571`) → `transport_t::close_noexcept()` (`:594-608`)다. 마지막 단계가
`transport->mutex` 아래 socket을 close/reset한다.

따라서 readiness가 true인 뒤 close가 reset을 끝내고 기존 request가 transport mutex를 얻으면,
`shared_ptr<transport_t>`로 객체 수명만 살아 있는 상태에서 `transport->socket->request()`가 null을
역참조할 수 있었다. **[실증]**이다.

제출 직전에 close와 같은 lock 순서 `_mutex` → `transport->mutex`로 `_closed`,
`_transport == transport`, `transport->socket`을 확인하도록 수정했다
(`channel_outbound_exchange.cpp:407-421`). 실패는 기존 closed request 표면과 같은 shutdown
boundary failure다.

### 1.2 `channel_native_client_t::send` — [실증]

호출 경로는 `message_bus_t::submit_send()` → outbound exchange
(`channel_runtime.cpp:1125-1135`) → native client 확보와 `co_await`
(`channel_outbound_exchange.cpp:1449-1469`) → `channel_native_client_t::send()`다.

send도 `_mutex` 아래 transport를 capture한 뒤 readiness를 기다린다(`:473-485`). 위와 같은
close 호출 경로가 그 사이 socket을 reset할 수 있으며, 기존 code는 transport mutex만 잡고
`transport->socket->options()`와 `send()`를 사용했다. 그러므로 close가 먼저 reset을 끝낸 뒤
send가 lock을 얻으면 null 역참조가 가능하다. **[실증]**이다.

제출 직전에 동일한 `_mutex` → `transport->mutex` gate에서 `_closed`, exact current transport,
non-null socket을 재검증하도록 수정했다(`channel_outbound_exchange.cpp:495-507`). 실패는 기존
closed send 표면과 같은 shutdown boundary exception이다. timeout, 오류 코드, send 순서,
SNDTIMEO 설치·복원은 변경하지 않았다.

## 2. 작업 2 — 발견 10 전수 재감사

`state_lane_t`/`callback_lane`/`route_cache_lane`의 `.run()` 298곳과 production coroutine의
`co_await` 경계를 함께 검색했다. 한 파생 값에 필요한 mutable read가 여러 lane turn 또는 lock
구간에 나뉘었는지, 그리고 경계 뒤 그 값들이 함께 쓰이는지를 추적했다. `git` 명령과 base 대조는
수행하지 않았다.

### 발견 목록

| 심각도 | 위치 | 함께 잡혀야 하는 값 | 현재 찢어진 경계 | 판정 |
|---|---|---|---|---|
| - | - | - | 같은 파생 snapshot을 여러 lane/lock turn에서 섞어 만드는 현재 코드 후보 없음 | 없음 |

다음은 파생 값을 한 turn에서 함께 capture하거나 final turn에서 exact identity를 재검증하는
음성 근거다.

| 위치 | 함께 잡은 값 | 판정 근거 |
|---|---|---|
| `runtime/client_server/raw_client_server_owner.cpp:1488-1501` | `ready`, `port`, `channel` | 한 `_lane.run()`의 `state_t`에서 send admission snapshot을 만든다. |
| `runtime/client_server/raw_client_server_owner.cpp:1548-1563` | `ready`, `port`, `channel`, `endpoint` | 한 lane turn에서 request snapshot을 만든다. |
| `runtime/client_server/raw_client_server_owner.cpp:677-693` | liveness result, port, descriptor update, clients | `prepared_t` 단일 lane turn으로 묶어 이후 전송한다. |
| `runtime/mesh/raw_mesh_node_owner.cpp:2775-2828`, `:2851-2877` | connection id/direction/endpoint, expected descriptor | candidate를 한 lane turn에서 구성하고 commit turn에서 connection id의 현재성을 다시 확인한다. |
| `runtime/mesh/raw_mesh_node_owner.cpp:3925-3934` | liveness result, port | `prepared_t` 한 turn에서 함께 capture한다. |
| `runtime/stateful/raw_stateful_dispatch.cpp:1480-1546`, `:1589-1640` | terminal registration, digest, callback, completion flag | 첫 turn의 exact item key/claim을 `work`에 묶고 외부 persist 뒤 같은 key를 다시 검증한다. |
| `runtime/stateful/stateful_object_runtime.cpp:1749-1786`, `:1793-1840`, `:1848-1935` | object ref, restore identity, state, callback | final turn이 exact ref와 restore identity를 재검증한다. |
| `runtime/stateful/maintenance_runtime.cpp:2063-2119`, `:2226-2263` | intent, shutdown claim, preflight/inventory ownership | 각 파생 preflight/claim을 한 lane turn에서 만들고 subsequent turn에서 current claim을 검증한다. |

나머지 연속 lane read는 단일 scalar 조회, 독립 protocol resource snapshot 또는 결과 사용 전 exact
identity 재검증으로 분류했다. 발견 10의 "서로 다른 시점 값을 섞어 하나의 파생 값"에는 해당하지
않는다. 작업 2에서는 소스를 변경하지 않았다.

## 3. 검증

실행 위치는 `framework/languages/cpp`, 빌드 디렉터리는 `framework/languages/cpp/build`였다.

```text
flock -w 7200 /tmp/zlink-cpp-gate.lock cmake --build build -j2
flock -w 7200 /tmp/zlink-cpp-gate.lock ctest --test-dir build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
```

첫 명령은 수정된 `channel_outbound_exchange.cpp`를 재컴파일하고 완료했다. 둘째 명령의 테스트
집계 원문은 다음과 같다.

```text
98% tests passed, 1 tests failed out of 45

Total Test time (real) =  56.18 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

실패는 요청에 명시된 기존 ShoppingMall blocking-result 지문이다. exit 86/134가 아니므로 rules
§4의 1회 재실행 대상이 아니다.

## 변경 파일

- `framework/languages/cpp/framework/src/runtime/channels/channel_outbound_exchange.cpp`
- `doc/plan/concurrency-redesign/cp3-cpp-fix-report.ko.md`
