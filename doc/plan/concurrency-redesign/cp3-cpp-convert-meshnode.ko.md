# CP3 C++ `mesh_node_runtime_t` lane 전환 보고

## 범위와 상태 그룹

대상은 `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp`와
`.hpp`이다. 감사의 `38 = 실행 primitive 14 + 독립 상태 24` 판정을 출발점으로 삼았다.
이번 변경은 서로 양방향 write가 없는 다음 keyspace를 각각 별도 lane으로 소유시켰다.

| 그룹 | 분류 | 소유/근거 |
|---|---|---|
| observed Spot authority | C1 | `_observed_spot_authorities` 단일 map. authority 관측의 overwrite/lookup만 한다. |
| negotiated receive chunk limit | C1 | `_negotiated_receive_chunk_limits` 단일 map. actor key의 기록과 단일/복수 source read는 같은 lane turn에 둔다. |
| Message Follow subscription registry | C1 | `_next_message_follow_subscription_id`와 `_message_follow_subscriptions`는 하나의 subscription keyspace다. handler 호출은 snapshot 뒤 lane 밖이다. |
| peer connection intent | C1 | `_peer_connection_intents` 단일 endpoint map. node connect/disconnect와 intent 갱신의 기존 한 임계구역을 한 turn으로 보존한다. |
| Spot builder actor index/ownership/epoch | C2 | `_state->spot_state`의 `actor_types_by_id`, `mesh_runtime_owned_native_actor_ids`, `core_actor_membership_epochs`는 Spot builder aggregate의 기존 lane이 이미 소유한다. 기존 직접 `recursive_mutex` 취득만 그 lane으로 옮겼다. |
| callback active-count, completion/tombstone, subscription in-flight | 작업 프로토콜 | 외부 callback/deactivate 및 completion의 exact-once·drain을 소유하므로 mutex를 유지했다(발견 7). |
| `_pending_application_callbacks`, `_active_application_callbacks`, `_active_completion_waiters`, `_stopping` | C3 | 이미 atomic이며 변경하지 않았다. |

새 lane들 사이에는 write가 없고, Spot builder lane으로의 write도 단방향이다. registry 사이에
새 cross-invariant나 양방향 의존은 추가하지 않았다.

## 파일별 전환

### `framework/src/runtime/mesh/mesh_node_runtime.hpp/.cpp`

- lock 전후: 파일의 직접 lock acquisition **38 -> 17**.
  - C1 registry lock: **14 -> 0** (authority 2, chunk limit 3, subscription registry 3,
    peer intent 6).
  - borrowed Spot builder C2 direct `recursive_mutex`: **7 -> 0**.
  - 남은 17개: peer callback active-count 7, completion/tombstone protocol 7,
    Message Follow subscription 객체별 active/in-flight protocol 3. 모두 상태 보호 대상에서
    제외한 실행/수명 protocol이다.
- 네 개의 새 state lane은 각각 위의 C1 keyspace를 소유한다. 컨테이너는 모두 기존의 보통
  `std::map`/`std::unordered_map` 그대로이며 concurrent container로 바꾸지 않았다.
- Spot builder의 C2 상태는 새 lane을 만들지 않고 기존 `_state->spot_state->lane`을 사용했다.
- 재진입 실측: 대상의 직접 `recursive_mutex` 취득 7곳은 중첩 취득이나 같은 public 표면의
  재호출 없이 짧은 actor index/epoch mutation만 수행했다. 해당 본문을 기존 Spot builder
  lane turn으로 옮겼고, lane 안에서 `mesh_node_runtime_t` public 메서드를 호출하는 자리는
  없다. 따라서 private unlocked helper 분리가 필요한 구조적 재진입은 0곳이다.
- 블로킹 브리지: **21개 `.get()`**.
  - 동기 공개 표면이므로 기존의 반환 전 map 등록·삭제·epoch 갱신·스냅샷 완료를 유지한다
    (발견 9).
  - 호출 시 외부 gate를 보유하지 않고, 제출한 turn도 caller가 보유한 gate를 재획득하지
    않는다. `state_lane_t`의 C++ `std::future` 표면에는 inline dependent continuation이
    없다. 따라서 스펙 06 §5의 세 조건을 충족한다.
  - 외부 callback은 `dispatch_message_follow`의 subscription snapshot 뒤에서 계속 lane 밖으로
    실행한다. completion/callback mutex를 잡은 채 lane을 기다리는 새 경로는 없다.
- 본문 조정 목록: **없음**. 기존 lock body를 해당 lane turn으로 감쌌다. optional 반환형을
  유지하기 위해 두 read lambda에 명시 반환형만 추가했다.
- 발견 10: 복수 actor source에서 chunk limit의 최소값을 만드는 연속 read는
  `negotiated_receive_chunk_limit_bytes(sources)`의 **하나의** chunk-limit lane turn 안에
  유지했다. subscription dispatch도 map 전체 snapshot을 하나의 turn에서 잡는다.

## 검증

빌드:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
exit 0
```

테스트:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
96% tests passed, 2 tests failed out of 45
Total Test time (real) =  51.51 sec

The following tests FAILED:
  13 - test_cpp_framework_host_lifecycle (Failed)
  19 - test_cpp_framework_layout_contract (Failed)
```

`zlink_cpp_framework_mesh_node_vertical_test`는 통과했다. `test_cpp_framework_layout_contract`는
요청에서 지정한 기존 실패다. `test_cpp_framework_host_lifecycle`은 rules의 기존 간헐 실패
목록에 있는 항목이며, 이번 변경의 기대값/테스트는 수정하지 않았다. exit 86/134가 아니므로
재실행 조건은 해당하지 않았다.

## STOP 및 예상과 달랐던 점

- STOP: **없음**. 관측 동작·타임아웃·오류 코드를 바꿀 필요가 없었고, 구조적 재진입도 없었다.
- 예상과 달랐던 점: source의 38 취득 중 7개는 runtime 자체 mutex가 아니라 이미 lane 소유인
  Spot builder aggregate의 잔존 `recursive_mutex` 직접 접근이었다. 이를 그대로 두면
  `actor_types_by_id`/ownership/epoch의 C2 ownership 경계가 깨지므로, 새 per-map lane 대신
  기존 Spot builder lane으로 전환했다. 감사의 실행 primitive 14 외에
  `message_follow_subscription_state_t`의 3개 active/in-flight 취득도 외부 handler의
  deactivate/drain 수명 protocol임을 확인해 발견 7 예외로 유지했다. 따라서 source token
  기준 잔존 수는 14가 아니라 17이다.
