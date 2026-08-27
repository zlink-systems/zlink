# CP3 C++ spotruntime G2 — Spot context·생성 lane 전환

## 결론 — STOP

이 패스는 소스 변경 없이 중단했다. 앞선 패스의 4그룹 표에서 `Spot context·생성`으로
명명한 상태가 실제 구현에서는 Actor registry·실행 그룹의 `actor_count` 및 route 등록과
같은 전이에서 함께 결정된다. 이 경계를 분리한 lane으로 옮기려면 두 lane 사이의
claim/commit 경계와 callback 이후 정산 순서를 새로 설계해야 한다. 이는 이번 패스의
"본문 무변경·관측 동작 불변" 조건으로는 수행할 수 없다.

STOP 사유는 크기나 복잡성이 아니라 다음의 실제 교차 불변식이다.

- `spot_runtime.hpp:765-769`의 `record_actor_context_route_unlocked`는 Actor route를
  등록한 바로 뒤 같은 `spot_context_state_t`의 `actor_count`를 증가시킨다.
- `spot_runtime.hpp:1617-1629`의 `commit_actor_left`는 Actor location/route/generation을
  지운 뒤 같은 context의 `actor_count`를 감소시키고 OnLeave를 결정한다.
- `spot_runtime.hpp:1295-1300`은 entry spot id map에서 얻은 id로 context를 찾아 Actor
  Join callback 대상과 Actor registry commit의 대상을 결정한다.

따라서 `spot_ids_by_name`·`spot_names_by_id`·`spot_contexts_by_id`만 G2 lane으로 옮기고
Actor registry는 주 mutex에 남기면, 위 전이가 두 primitive에 나뉜다. 반대로 위 Actor
전이 전체를 G2 lane으로 감싸면 Actor registry·실행 그룹도 G2가 소유하게 되어 이번 허용
범위를 넘는다. callback을 lane 밖으로 빼려면 claim과 callback 뒤 commit을 새 boundary로
정의해야 하며, 이는 요구된 재설계에 해당한다.

## 상태 그룹과 분류

| 그룹 | 상태 | 분류 | 근거 | 이번 결과 |
|---|---|---|---|---|
| Builder 구성 | snapshot, factory, lifecycle, resolver | C2 | 구성 등록의 교차 상태 | 미변경 |
| Spot context·생성 (G2) | id 양방향 map, context map, pending creation, native Spot | C2 | id/name/context 및 claim/release | **STOP** — 아래 Actor 경계 누출 |
| Actor registry·실행 | actor location/generation/route/instance 및 context `actor_count` | C2 | route 등록과 context actor 수가 같은 전이 | 미변경 |
| Relocation·handoff | coordinator, recovery, remote cleanup, terminal | C2 | fence/exact-once terminal | 미변경 |
| `route_client` | optional route client | C1 | 기존 `route_client_lane` | 미변경 |
| `stopping` | stop flag | C3 | atomic flag | 미변경 |

## 파일별 계수·분류

| 파일 | 전 | 후 | C1 / C2 / C3 | 판정 |
|---|---:|---:|---|---|
| `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp` | 감사표: 실행 primitive 31, 상태 보호 147 | 동일 | G2는 C2, `route_client` C1, `stopping` C3 | 소스 변경 없음 |
| `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp` | 감사표: 상태 보호 5 | 동일 | G2 context와 Actor context-count 교차 C2 | 소스 변경 없음 |

`cp3-audit-cpp.ko.md` §3의 대상 행 분류를 그대로 사용했다. 실행 primitive 31개와
socket·dispose 프로토콜 취득은 변경하지 않았다.

## 재진입·외부 callback·블로킹 bridge

- 재진입 실측: `record_actor_spot`은 `spot_runtime.cpp:9974-9984`에서 mutex를 잡고
  `spot_name_for_unlocked`을 호출한다. 기존 L2가 기록한 public `spot_name_for` 재획득
  형태와 달리 현재 위치는 unlocked helper를 호출하므로 이 자리는 G2 재진입 원인이 아니다.
- 그러나 header template의 Actor Join/Leave 전이는 위 교차 불변식을 유지하려고 같은
  recursive mutex 소유 문맥에서 `find_context`와 Actor route helper를 함께 호출한다.
  private unlocked helper로 분리해도 두 상태 그룹의 원자 전이가 남으므로 해소되지 않는다.
- 외부 callback: `create_spot_context_unlocked`는 native Spot close, staged restore,
  `on_create`, `on_initialize`, Location Store claim을 위해 `node_lock`을 해제한다
  (`spot_runtime.cpp:9440-9556`). 발견 7에 따라 이 callback/외부 작업을 G2 lane 안에
  넣을 수 없다. 이를 lane 밖에 유지하면서 pending claim/context registration을 보존하려면
  별도 claim/commit protocol이 필요하다.
- 새 blocking bridge: 0개. 기존 bridge는 변경하지 않았다. 따라서 스펙 06 §5 판정은
  새 항목 없음이다.

## 발견 10

`application_relocation_units`는 context 목록, context 특성, Actor spot/generation을
함께 읽어 relocation unit을 만든다(`spot_runtime.cpp:10642-10676`). 이는 하나의 파생값을
만드는 read 묶음이다. G2 map read만 별도 turn으로 감싸면 발견 10의 캡처 분할이 되며,
Actor registry와 같은 turn으로 유지하려면 위 교차 ownership 재설계가 필요하다.

## 본문 조정

없음. STOP 조건을 확인한 뒤 source/test 기대값을 수정하지 않았다.

## 테스트

실행하지 않음. source 변경 전에 STOP 조건이 확인되어 지정 build/ctest를 실행할 대상이
없었다. 따라서 집계 원문도 없다.

## 예상과 달랐던 점

앞선 4그룹 표의 "Spot context·생성" 설명에는 context 안의 `actor_count`가 Actor
membership 전이와 함께 변경된다는 사실이 드러나지 않았다. 실제 구현에서는 context map
소유와 Actor registry 전이가 단방향 조회가 아니라 동일 critical section의 commit/leave
전이로 결합되어 있다.
