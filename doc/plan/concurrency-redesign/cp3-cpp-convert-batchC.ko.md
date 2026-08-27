# CP3 C++ 상태 보호 전환 — batchC

## 결과

부분 전환 완료: `service_topology_registry_t`와 `service_liveness_registry_t`의 상태 보호 잔존 17개를 객체별 `state_lane_t` 소유로 옮겼다. 각각의 plain container와 교차 불변식은 하나의 lane에 남겼고, C1 concurrent container나 C3 치환은 하지 않았다.

`fanout_location_runtime.cpp`와 `endpoint_connections.cpp`는 이번 배치에서 **STOP**이다. 전자는 run/stop/snapshot이 publisher·subscriber·snapshot/observer 상태를 서로 다른 실행 경계에서 직접 만져, lock acquisition만 lane으로 바꾸면 lane 밖 상태 접근이 남는다. 후자는 live callback을 기존 lock 안에서, endpoint 삽입/삭제 전에 호출한다. callback 밖 실행과 그 시점 관측을 동시에 보존하려면 placeholder claim 및 turn A/B 재검증 경계가 필요하다. 둘 다 관측 동작을 바꾸지 않고 처리할 수 있는 단순 lock-to-lane 치환이 아니므로 여기서 멈췄다.

## 대안 및 CMake

lane/executor를 forward declaration+pimpl으로 숨기는 안을 먼저 검토했다. 그러나 `test_cpp_framework_service_wire_codec`는 헤더만 포함하는 target이 아니라 `service_topology_registry.cpp`를 직접 compile한다. pimpl이어도 그 source가 `state_lane_t`와 `offload_executor_t` 정의를 참조하므로 이 target의 link 요구는 사라지지 않는다. 따라서 기존 형식처럼 선언 헤더가 lane/executor를 직접 소유하게 했고, 최소 CMake 변경으로 해당 test target에 다음 두 source만 추가했다.

| target | 추가 source | 이유 |
|---|---|---|
| `test_cpp_framework_service_wire_codec` | `runtime/dispatch/offload_executor.cpp`, `runtime/execution/state_lane.cpp` | 독립 compile되는 `service_topology_registry.cpp`의 lane/executor symbol 해소 |

새 target을 만들거나 기존 target 구성을 바꾸지 않았다.

## 파일별 판정

| 파일 | lock 전→후 | C1/C2/C3 | 재진입 실측·해소 | bridge와 spec 06 §5 | 본문 조정 | 발견 10 |
|---|---:|---|---|---|---|---|
| `runtime/mesh/service_topology_registry.cpp` | 10→0 | 0/10/0 | `publish_local`·`admit_impl`·`disconnect`의 change handler를 lane 작업의 반환값으로 빼고 lane 밖에서 호출했다. callback이 `topology()`를 다시 읽어도 같은 lane 재진입이 아니다. | 10개 `.run(...).get()`: 동기 registry API의 peer/selection 갱신·snapshot 반환 전 완료 계약을 보존한다. item은 외부 socket/dispose gate를 잡지 않으며, 호출자가 lifecycle gate를 잡은 경로에서도 topology item은 그 gate를 재획득하지 않는다. C++ `std::future` 기다림은 inline dependent continuation을 실행하지 않는다. §5 충족. | `admit_impl`의 change callback 실행 위치만 lane 완료 뒤로 분리; 상태 전이와 handler capture는 같은 turn에 유지. | `admit_impl`의 local·admitted/not-required map·topology version 판독/갱신, `select`의 peer/weight/cumulative 묶음을 각각 한 turn에 유지했다. |
| `runtime/mesh/service_liveness_registry.cpp` | 7→0 | 0/7/0 | 같은 객체 public 재호출·외부 callback은 발견되지 않았다. | 7개 `.run(...).get()`: admit/disconnect/ack/tick과 조회의 기존 동기 반환 전 상태 완료를 보존한다. item은 호출자 gate를 획득하지 않고 C++ future completion은 inline continuation을 실행하지 않는다. §5 충족. | 없음(기존 각 lock 본문을 한 lane turn으로 감쌌다). | `tick`의 peer map, deadline/next_probe/outstanding probe, `_next_probe_id`를 한 turn으로 유지했다. |
| `runtime/fanout/fanout_location_runtime.cpp` | 10→10 | 0/10/0 | public 재호출은 확정하지 못했으나 run/stop/snapshot 경계가 `_publishers`·`_subscribers`·sequence/cache/observers를 lane 밖에서도 직접 다룬다. | 0; 전체 C2 ownership boundary를 먼저 설계해야 한다. §5 미판정. | 없음. | `build_snapshot_locked`의 desired/raw connection/sequence read는 하나의 turn이어야 한다. |
| `runtime/configuration/endpoint_connections.cpp` | 7→7 | 0/7/0 | `attach`·`connect`·`disconnect`가 live callback을 lock 안, endpoint 변화 전에 호출한다. 스펙 06 §6 유형 ③의 turn A/callback 밖/turn B를 보존하려면 placeholder가 필요하다. | 0; callback 관측 순서를 지키는 bridge 경계를 아직 만들 수 없다. §5 미판정. | 없음. | `frozen`, endpoints, live callback의 판독·결정은 현재 한 lock turn에 함께 있다. |

`fanout`의 `_stop`은 기존 atomic C3이나, 이번 계수 대상 lock acquisition은 모두 C2다. 나머지 세 컴포넌트도 한 C2가 컴포넌트 메커니즘을 결정하므로 C1/C3로 따로 쪼개지 않았다.

## 발견 1·2·4·6·7·9·10 대조

- 1·4: 새 lane 안에서 timeout/retry/background task를 시작하지 않는다. C++ lane은 thread-local current marker를 사용하지만 context-flow 차단이 필요한 신규 장기 작업은 없다.
- 2: topology/liveness bridge item은 외부 gate를 다시 획득하지 않고, C++ future completion에는 inline dependent continuation 경로가 없다.
- 6: topology의 change handler는 state transition과 handler capture를 turn 안에서 끝낸 뒤 turn 밖으로 실행시켜 재진입을 제거했다. endpoint는 같은 유형이지만 placeholder 없이 바꾸면 callback 관측이 달라져 STOP했다.
- 7: 감사표의 대상 34개는 모두 상태 보호 잔존이며 socket·dispose 또는 실행 primitive는 건드리지 않았다.
- 9: 전환한 17개는 fire-and-forget post가 아니라 `.get()`을 사용해 반환 전에 상태 등록·판독·결정을 끝낸다.
- 10: 위 표의 topology admit/select와 liveness tick의 연속 read/write 묶음을 read별 lane turn으로 분할하지 않았다.

## 검증

빌드 디렉터리는 모두 `framework/languages/cpp/build`를 사용했다.

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
exit 0
```

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
98% tests passed, 1 tests failed out of 45
Total Test time (real) =  56.89 sec
The following tests FAILED:
 19 - test_cpp_framework_layout_contract (Failed)
```

`test_cpp_framework_layout_contract`는 요청에서 지정한 알려진 기존 실패다. 전환한 topology standalone codec test, liveness를 포함하는 M6A runtime, state-lane test는 모두 통과했다. exit 86/134는 없어서 재실행 대상이 아니었다. 테스트 기대값은 바꾸지 않았다.

## 변경 파일

- `framework/languages/cpp/CMakeLists.txt`
- `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.hpp`
- `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp`
- `framework/languages/cpp/framework/src/runtime/mesh/service_liveness_registry.hpp`
- `framework/languages/cpp/framework/src/runtime/mesh/service_liveness_registry.cpp`
- 이 보고서

## 예상과 달랐던 점

이번에는 CMake 허용으로 이전의 codec-link STOP은 해소됐다. 대신 batch 전체의 나머지 두 컴포넌트는 객체별 lane 추가만으로는 충분하지 않은 실제 ownership/callback 경계를 드러냈다. source-local static lane이나 객체 주소 side table은 사용하지 않았다.

## STOP

batch 전체: **부분 STOP** — topology/liveness 전환은 완료, fanout/endpoint는 위의 구조적 재진입·ownership 경계 사유로 중단했다.
