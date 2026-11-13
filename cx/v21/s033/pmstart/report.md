# Instance Spot startup visibility bisect

## 결과

측정값은 stats port가 열리고 해당 프로세스가 소유한 listener임을 확인한 시점부터 `/pm/request` 첫 200까지의 초다.

| 커밋 | 결과 (초, 각 시작) |
|---|---|
| `3f83745bed` (양호 기준) | 0.0181, 0.00075, 0.00060 |
| `24703a81d5` | 0.0186 (양호) |
| `c5a9d26f41` | 15.0585, 15.0405, 15.0397 (나쁨) |
| `10c9ac2f6a` | 15.0567 (나쁨) |
| `d961e51e23` | 20.0697 (나쁨) |
| `54ee69b8c0` (나쁨 기준) | 20.0692, 20.0564, 20.0549 |

경로상 첫 나쁜 커밋은 **`c5a9d26f41`**이다. 이후 후보도 나빴고, 직전 확인한 `24703a81d5`는 양호했다.

## 원인

`mesh_node_host_service.cpp:63-74`가 명시적 Routing ID 등록을 `fixed_rid_new_claim`으로 분류하고, 첫 descriptor 게시(`:3257-3260`)에 사용한다. 이 벤치 서버는 고정 RID `bench-server`를 사용한다.

`in_memory_location_store.hpp:82-98`은 Fixed RID claim에서 이전 descriptor가 없는 경우에도 `reclaim_wait_elapsed`를 호출한다. 아직 대기 시간이 지나지 않으면 `waiting`을 반환한다. 판정식은 `location_repository.hpp:657-659`의 `elapsed / 3 >= fencing margin`이다. 기본 margin 5초이므로 첫 게시가 최소 15초 지연된다. 이 경로가 첫 나쁜 커밋에 들어왔고 c5의 세 번 측정에서 재현됐다.

## 스펙 대조와 판정

`framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md` §3.3은 “이전 owner lease가 만료된 뒤 시작한 lifecycle은 같은 RID를 claim한다”고 정한다. §6은 owner lease/descriptor 준비, descriptor 공개, local runtime 준비 후 `Serving`과 새 target 선택 공개 순서를 정하지만 시작 지연 상한은 두지 않는다. 따라서 **stats port 개방은 Framework readiness가 아니며, 이 지연 자체를 위반으로 판정할 시간 계약은 없다. 벤치는 Serving/readiness 또는 `/pm/request` 성공을 기준으로 준비 완료를 기다려야 한다.**

별도 계약 불일치도 확인했다. c5의 `options.hpp:31`은 C++ 기본 owner lease TTL을 30초로 바꿨지만 공통 스펙 `05-location-relocation/01-location-runtime.ko.md` §5.2와 C++ reference `07-location-authority.ko.md`는 15초를 명시한다. 이 기본값 변경은 스펙과 맞지 않는 제품 계약 결함이며, 위 15초 reclaim 대기와는 별개의 문제다.

## 검증 및 정리

기준·후보 빌드는 모두 지정된 Release 설정과 `cpp-build-slot.sh`로 수행했다. 서버는 프로세스 그룹 종료까지 확인했으며 5294/5295 listener가 남지 않았다. 전용 `.pmstart-build`와 임시 측정 스크립트, detached worktree를 정리했다.
