# CP3 C++ spotruntime lane 전환 — 1차 본 전환

## 결론

`spot_node_builder_state_t`의 상태 보호 취득 **147(.cpp) + 5(.hpp)** 중, 이번 패스에서는
다른 Spot·Actor·relocation 상태와 교차 불변식이 없는 `route_client` 등록/조회 그룹을
`route_client_lane`으로 옮겼다. 상태 보호 mutex 취득은 **152 → 149**다. 실행 primitive
31개와 socket·dispose 프로토콜 취득은 변경하지 않았다.

STOP은 **아님**이다. 이번에 옮기지 않은 C2 그룹은 다음 패스의 대상이며, 크기나 복잡성은
STOP 사유로 쓰지 않았다.

## 상태 그룹과 분류

| 그룹 | 포함 상태 | 분류 | 근거 | 이번 패스 |
|---|---|---|---|---|
| Builder 구성 | `snapshot`, spot/actor factory, lifecycle, resolver 구성 | C2 | factory 등록은 snapshot의 entry/instance/execution-mode와 relocation 설정을 함께 전이한다. | 기존 builder `lane` 유지, 주 mutex 전환은 다음 패스 |
| Spot context·생성 | spot id 양방향 map, context, pending creation, native Spot | C2 | id/name/context 및 claim/release의 교차 불변식이 있다. | 다음 패스 |
| Actor registry·실행 | location/generation/fence, instance/index, route, queue snapshot, native actor | C2 | actor lifecycle 한 전이가 여러 map·set·context actor count를 함께 바꾼다. | 다음 패스 |
| Relocation·handoff | coordinator, recovery, remote cleanup, reply/terminal 상태 | C2 | route fence·actor registry·exact-once terminal의 교차 불변식이 있다. | 다음 패스 |
| `route_client` | `std::optional<route_client_t>` | C1 | 등록/복사 조회만 하며 위 C2 상태와 함께 결정을 만들지 않는다. 외부 dispatcher 호출은 복사한 값으로 lane 밖에서 계속 수행한다. | **`route_client_lane`으로 완료** |
| `stopping` | `std::atomic_bool` | C3 | 독립 stop flag다. | 기존 atomic 유지 |

취득별 집계는 C1 **3 → 0 mutex 취득** (`spot_runtime.cpp:10608,10779,11273`의 전환 전
위치), C2 **149 → 149**, C3 **mutex 취득 0**이다. `.hpp`의 상태 보호 5개와 C2 147개 중
C1 세 곳을 제외한 .cpp 144개는 다음 패스에 남긴다. CP3 감사가 실행 primitive로 분류한
31개는 이 계수에서 제외했다.

## 변경 파일

### `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp`

- `route_client_lane_executor`와 `route_client_lane`을 `route_client` 바로 앞에 추가했다.
- 이 lane은 `route_client` 한 필드만 소유한다. Spot/Actor aggregate와 양방향 호출이나
  교차 write가 없다.

### `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp`

- `set_route_client`, `spot_context_t::spot_route_client`, 그리고 mesh record의 두 dispatcher
  선택 지점이 모두 같은 lane에서 optional 값을 등록 또는 복사한다.
- 복사 뒤 `spot_route_internal_dispatcher_t`를 호출하는 위치는 원래처럼 lane 밖이다. lane
  내부에 external callback·transport await를 넣지 않았다.

## 재진입

1차에서 확인된 `record_actor_spot()` → `spot_name_for()` 중첩 취득은
`spot_name_for_unlocked()`으로 이미 제거된 상태였다. 이번 C1 lane의 네 turn 본문은 optional
대입 또는 복사만 하고 같은 객체의 public 메서드나 external callback을 호출하지 않는다.
따라서 새 lane 재진입은 **0곳**이고, 추가 private helper는 필요 없었다.

## 블로킹 호환 경계와 스펙 06 §5

새 `.get()` bridge는 **4개**다.

| 위치 | 수 | 유지 사유 |
|---|---:|---|
| `spot_context_t::spot_route_client()` | 1 | 공개 동기 반환값을 유지하고, 반환 전에 route client 사본을 확정한다. |
| `set_route_client()` | 1 | 동기 등록 API가 반환할 때 등록이 완료돼야 한다. |
| `dispatch_mesh_record()` dispatcher 선택 | 2 | 동일한 동기 dispatch 호출 안에서 route client 존재 여부와 사본을 확정한 뒤 기존 순서대로 dispatcher를 만든다. |

§5 조건은 충족한다. (1) 새 lane 본문은 node mutex나 caller의 외부 gate를 재취득하지 않는다.
(2) C++ `std::future::get()`은 continuation을 lane current scope에서 inline 실행하지 않으며
`state_lane_t::run`은 promise 결과만 완료한다. (3) 모두 공개 동기 표면 또는 reply/dispatch 전
등록·캡처 완료가 필요한 return-before 경계다.

## 본문 조정과 발견 10

전환한 세 기존 lock 범위의 대입/복사 본문은 그대로 lane lambda로 옮겼다. `spot_route_client()`는
기존에 무잠금으로 optional을 직접 읽던 자리여서, null state/node 검사는 lane 밖에 두고 optional
사본만 lane에서 얻도록 보완했다. 그 밖의 본문 조정은 없다.

발견 10과 관련해 이 그룹에는 여러 read를 합쳐 파생 값을 만드는 캡처가 없다. 각 dispatcher
선택은 `route_client` **한 값**을 한 turn에서 복사하고, dispatcher 생성은 그 정확한 사본만
사용한다. 따라서 read를 여러 turn으로 나누지 않았다.

## 검증

빌드 디렉터리는 지정된 `framework/languages/cpp/build`만 사용했다.

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
exit 0
... Built target zlink_framework
... Built target test_cpp_framework_state_lane
... Built target test_cpp_framework_app_host
```

최종 label 게이트 집계 원문은 다음과 같다.

```text
98% tests passed, 1 tests failed out of 45

Total Test time (real) =  57.66 sec

The following tests FAILED:
	19 - test_cpp_framework_layout_contract (Failed)
```

명령은 다음과 같다.

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
```

`test_cpp_framework_layout_contract`는 요청에서 지정된 기존 실패다. 최초 전체 실행에서 한 번
실패했던 `test_cpp_framework_host_lifecycle`는 focused 재실행 1/1 통과했고, 위 최종 전체
게이트에서도 통과했다.

## 예상과 달랐던 점과 다음 패스

`route_client`는 node mutex 안에서 읽는 두 dispatch 지점 외에 `spot_context_t`에서 무잠금으로
직접 읽히고 있었다. 한 lane으로 네 접근을 모두 모으면서 그 경로도 같은 소유 규칙으로 맞췄다.

다음 패스는 Spot id/name/context/pending-creation aggregate와 Actor registry aggregate를 서로
분리할 수 있는지 각 write transition 기준으로 확인한다. relocation·handoff는 Actor registry와
fence를 함께 바꾸는 곳이 있으므로 독립 lane으로 나누기 전에 더 작은 transition 단위의
교차 불변식을 먼저 기록해야 한다.
