# CP3 C++ `spot_runtime` lane 소유권 회귀 수정

## 재현 방법

빌드 디렉터리는 `framework/languages/cpp/build`를 사용했다.

```bash
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -R '^test_cpp_framework_execution$' --output-on-failure
```

수정 전 첫 단독 실행은 현재 실행 환경에서 0.48초, 수정 뒤 재실행은 0.47초에 통과했다. 따라서 이 실행만으로는 이전 중앙 게이트의 간헐 실패를 다시 만들지 못했다. `test_cpp_framework_execution.cpp`의 원격 actor cutover 단언은 대상 `on_actor_joined`가 source `on_leave_actor`보다 먼저 시작되고, 이후 같은 노드의 재 Join도 현재 host handle로 완료해야 한다는 점을 확인한다.

## 원인

`spot_node_runtime_t::prepare_remote_actor_to_spot`은 대상 Context·factory·admission을 node lane에서 읽은 뒤 actor 생성·상태 복원·lifecycle callback을 lane 밖에서 수행하고, 마지막에 actor registry와 Context route를 다시 node lane에서 기록했다.

따라서 하나의 소유권 전이가 다음 세 turn으로 갈렸다.

1. Context 및 admission 확인.
2. lane 밖의 actor 생성·복원·`on_actor_joined` callback.
3. `actor_instances`, `actor_spot_ids`, `actor_generations`, Context `actor_count` 기록.

2번 동안 `actor_count`가 0이어서 idle eviction 또는 Spot close가 대상 Context를 비어 있는 것으로 관측할 수 있었다. 이후 3번이 닫히는 Context에 actor를 등록하거나 PREPARE를 실패시키면 cutover의 target-owner 경계가 깨진다.

## 처방

`spot_runtime.cpp:8238`의 첫 node-lane turn에 Context 유효성 검증과 `pending_actor_contexts` placeholder 예약을 합쳤다. 예약은 Context `actor_count`를 하나 증가시켜 close/idle eviction이 빈 Context로 관측하지 못하게 한다.

actor 생성·복원·callback은 그대로 lane 밖에서 실행한다. 실패하면 `spot_runtime.cpp:8294`가 같은 placeholder를 제거하고 `actor_count`를 원복한다. 성공하면 `spot_runtime.cpp:8433`의 완료 turn이 placeholder를 확인한 뒤 실제 actor registry와 Context route로 교체한다. 이 교체는 같은 turn에서 count를 원복하고 다시 기록하므로 외부에는 중간 빈 상태가 노출되지 않는다.

`recursive_mutex`를 복구하거나 lock을 인자로 전달하지 않았다. 관측 순서, timeout, 오류 코드는 정상 경로에서 변경하지 않았다.

## 테스트 결과

빌드:

```bash
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j10
```

결과: 성공.

focused 재실행:

```text
1/1 Test #33: test_cpp_framework_execution .....   Passed    0.47 sec
100% tests passed, 0 tests failed out of 1
```

전체 라벨 게이트:

```bash
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
```

집계 원문:

```text
98% tests passed, 1 tests failed out of 45

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

`test_cpp_framework_layout_contract`만 실패했다. 이는 작업 시작 시 지정된 기존 실패이며 `ShoppingMall/Server/OrderWorkflow/main.cpp`의 blocking `result()` layout 검사 두 건이다. 이 작업의 허용 파일 밖이므로 수정하지 않았다.

중간 전체 게이트에서는 `test_cpp_framework_host_lifecycle`가 한 번 1.45초에 실패했으나, 즉시 단독 재실행은 11.52초에 통과했고 최종 전체 게이트에서도 11.43초에 통과했다. 최종 집계에는 포함하지 않았다.

## STOP 여부

STOP 아님. 관측 동작 변경 없이 lane turn의 원자 경계만 복구했고, 전체 게이트는 알려진 기존 1건만 실패했다.
