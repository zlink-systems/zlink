# cpp Spot 메시지 hot path의 블로킹 state lane 브리지 조사

작성: 2026-08-27. 이 문서는 cpp Spot 계층의 대표 메시지 한 건이 수신 진입부터
application handler 호출까지 `state_lane_t::run(...).get()` 동기 대기를 몇 번 통과하는지
정적으로 센 결과다.

## 1. 결론

정상적으로 활성화된 Spot, warm Actor와 실행 큐, 유효한 route fence를 전제로 하면
**handler 호출까지의 `[매번]` 브리지는 경로별로 다음과 같다.**

| 경로 | 선택한 정상 경로 | `[매번]` 브리지 |
|---|---|---:|
| 원격 Actor send | spot-wide 실행, 유효한 Actor route, relocation·bound Session 없음 | **11** |
| 원격 Actor request | 위 조건 + 첫 hop의 wire operation ID 있음 | **13** |
| Spot 간 send/request | 유효한 Spot route fence가 있는 application packet | **5** |
| Actor join | 이미 활성화된 일반 User Spot, warm Actor instance | **7** |
| Spot timer 발화 | spot-wide 실행, fire batch의 첫 tick | **2** |
| Spot timer 발화 | per-actor 실행, fire batch의 첫 tick | **1** |

가장 큰 비용은 원격 Actor packet이다. send 한 건이 노드 상태 lane 9회와 Spot callback
lane 2회를 기다린다. request는 여기에 handoff reply 보관 lane과 pending-request 계수 lane을
각 1회 더 기다린다. 이 수치는 저장소 전체의 브리지 출현 개수가 아니라, 아래에 고정한
구체적인 성공 경로 한 번의 실제 통과 수다.

### 계수 경계

- 시작점은 cpp MeshNode 수신 callback 또는 native timer callback이다.
- 끝점은 application handler를 호출하는 문장이다. handler가 반환한 뒤의 terminal 정리와
  reply 전송은 주 계수에 넣지 않았다.
- `state_lane_t::run()`은 `std::future`를 반환한다
  (`framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp:30-62`). 그 결과를
  `.get()`으로 기다리는 호출만 센다.
- `serial_execution_queue_t::try_post*`, coroutine `co_await`, 일반 `task.result()`, mutex 대기,
  optional·smart pointer의 `.get()`은 세지 않았다.
- handler terminal까지 넓히면 Actor packet, Spot route, timer는
  `spot_context_state_t::leave_callback()`의 callback lane 대기 1회가 추가된다
  (`framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:1998-2014`). Actor join은
  handler 승인 뒤 commit 경로가 이어지지만, 요청한 “handler 호출까지” 경계 밖이므로 주 계수에
  넣지 않았다.

## 2. 경로별 상세

### 2.1 원격 Actor packet → Spot Actor handler

#### 선택한 경로와 진입점

진입점은 `mesh_node_host_service_t::start()` 안의 `dispatch_ready` receive callback이다
(`framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:1616`,
`:2213-2217`). 다음 정상 조건을 고정했다.

- `owner_kind=actor`, `record_kind=actor_send` 또는 `actor_request`
- 현재 노드와 Actor incarnation을 정확히 가리키는 `actor_route`가 있음
- bound Session source와 relocation/Message Follow 우회가 없음
- Actor instance와 Actor 실행 큐가 이미 만들어져 있음
- User Spot 실행 모드는 `spot_wide`

#### 호출 체인

1. `mesh_node_host_service_t::start`의 receive callback
   (`mesh_node_host_service.cpp:2213-2217`)
2. Application Job Queue permit을 받은 뒤 application executor에 제출
   (`mesh_node_host_service.cpp:2292-2339`)
3. `spot_node_runtime_t::dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2352-2359`, `spot_runtime.cpp:11985`)
4. Actor record 분기와 route admission
   (`spot_runtime.cpp:12324-12516`)
5. `spot_node_runtime_t::relay_actor_packet`
   (`spot_runtime.cpp:12756-12764`, `:9560`)
6. Actor materialization·현재 Spot dispatch projection
   (`spot_runtime.cpp:9845-9881`, `:10024-10077`)
7. `spot_handler_registry_t::invoke_erased`
   (`spot_runtime.cpp:10187-10194`, `:3681`)
8. 등록된 application handler 호출
   (`spot_runtime.cpp:3953-3967`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Actor type 조회 (`spot_runtime.cpp:12327-12334`) | `[매번]` | 선택한 send/request 모두 통과 |
| 2 | native node·relocation·admission callback 묶음 조회 (`:12385-12390`) | `[매번]` | route admission 전마다 통과 |
| 3 | 현재 Actor authority fence 조회 (`:12416-12420`) | `[매번]` | 선택한 “route 있음, bound Session 없음” 경로마다 통과 |
| 4 | handoff reply token 보관 (`:12576-12599`) | `[매번]` request 전용 | 첫 hop이고 operation ID가 있는 request마다 통과. send에는 없음 |
| 5 | relay parking node RID 조회 (`:12628-12630`) | `[매번]` | send/request 모두 metadata 구성 전에 통과 |
| 6 | retiring Actor 검사 (`:9595`) | `[매번]` | `relay_actor_packet` 진입마다 통과 |
| 7 | 첫 번째 현재 authority fence 조회 (`:9617-9622`) | `[매번]` | 선택한 fenced local-target 경로마다 통과 |
| 8 | 두 번째 현재 authority fence 조회 (`:9707-9712`) | `[매번]` | backlog/Message Follow 방향 결정 전에 같은 fence를 다시 읽음 |
| 9 | factory·location·Actor instance projection (`:9845-9881`) | `[매번]` | warm instance여도 조회 turn은 항상 통과 |
| 10 | 현재 generation·Spot context·실행 모드 projection (`:10024-10077`) | `[매번]` | handler dispatch 직전마다 통과 |
| 11 | pending request 증가 (`:10171-10176`) | `[매번]` request 전용 | request마다 통과. send에는 없음 |
| 12 | Spot serial queue snapshot (`:3873-3895` → `:2110-2114`) | `[매번]` spot-wide | Actor queue turn이 Spot queue에 넘기기 전에 callback lane을 기다림 |
| 13 | callback admission·depth 증가 (`:3953` → `:1988-1995`) | `[매번]` | application handler 호출 직전에 통과 |
| C1 | Actor type cache 채우기 (`:12335-12342`) | `[조건부]` | 이 노드가 Actor ID의 type을 처음 authority에서 찾은 때 1회. Actor별 cold hit |
| C2 | Actor instance 설치 (`:9890-9919`) | `[조건부]` | 이 노드의 첫 materialization 때 1회. factory 호출 자체는 lane 밖 |
| C3 | Actor 실행 큐 생성 (`:3740-3762`) | `[조건부]` | copy-on-write queue snapshot miss 때 1회. 보통 Actor별 첫 packet |

send는 표의 1, 2, 3, 5-10, 12, 13을 통과해 **11회**다. request는 4와 11이
추가되어 **13회**다. type cache·Actor instance·Actor queue가 모두 cold이면 각각 1회씩,
send는 최대 14회, 선택한 request는 최대 16회가 된다.

`per_actor` 실행이면 `invoke_erased`가 Actor queue에서 Spot queue로 한 번 더 넘기지 않으므로
#12가 빠진다. 따라서 같은 steady-state send/request는 각각 10회/12회다. Message Follow나
relocation backlog로 빠지는 분기는 로컬 application handler에 도달하지 않는 다른 terminal
경로라 이 경로의 조건부 가산으로 섞지 않았다.

### 2.2 Spot 간 route dispatch → Spot packet handler

#### 선택한 경로와 진입점

진입점은 2.1과 같은 `mesh_node_host_service_t::start()` receive callback
(`mesh_node_host_service.cpp:2213-2217`)이다. `owner_kind=spot`,
`record_kind=spot_send|spot_request`, 유효한 `spot_route`를 가진 일반 application packet을
선택했다. 내부 control packet은 선택하지 않았다.

#### 호출 체인

1. receive callback → application executor → `dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2213-2217`, `:2329-2359`)
2. Spot/node record envelope 및 내부 packet 판별
   (`spot_runtime.cpp:12005-12215`)
3. target Spot context와 route fence admission
   (`spot_runtime.cpp:12216-12250`)
4. `spot_handler_registry_t::invoke_erased`
   (`spot_runtime.cpp:12266-12276`, `:3681`)
5. 등록된 application handler 호출
   (`spot_runtime.cpp:3953-3967`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | route client snapshot (`spot_runtime.cpp:12012-12014`) | `[매번]` | application packet이어도 내부 packet 판별 전에 항상 통과 |
| 2 | target Spot context 조회 (`:12228-12230`) | `[매번]` | 선택한 Spot record마다 통과 |
| 3 | Location lifecycle 조회 (`:12236-12241`) | `[매번]` | 선택한 fenced route마다 통과. fence 없는 record에는 없음 |
| 4 | Spot serial queue snapshot (`:3917-3923` → `:2110-2114`) | `[매번]` | 활성화된 Spot의 serial queue에 게시할 때 통과 |
| 5 | callback admission·depth 증가 (`:3953` → `:1988-1995`) | `[매번]` | handler 호출 직전에 통과 |
| C1 | queue 부재 뒤 admission 재검사 (`:2115-2117` → `spot_runtime.hpp:628-632`) | `[조건부]` | 정상 활성화에서는 발생하지 않음. queue가 없거나 teardown 중인 경계 |

따라서 유효한 fenced Spot route는 **5회**다. fence가 없는 legacy/local record는 #3이 없어
4회다. route client를 설정하는 `set_route_client`의 브리지
(`spot_runtime.cpp:11812-11816`)는 `[초기화]`이며 이 메시지 경로에는 없다.

### 2.3 Actor join → User Spot `on_actor_join`

#### 선택한 경로와 진입점

진입점은 같은 receive callback에서 받은 `owner_kind=spot`, `record_kind=spot_control`,
`operation_kind=actor_join` record다 (`mesh_node_host_service.cpp:2213-2217`,
`spot_runtime.cpp:12824-12838`). 이미 활성화된 일반 User Spot으로 warm Actor instance가 join하는
경로를 선택했다. entry Spot과 remote relocation prepare 경로는 제외했다.

#### 호출 체인

1. receive callback → application executor → `dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2213-2217`, `:2329-2359`)
2. actor-control join 분기와 target 종류 projection
   (`spot_runtime.cpp:12824-12877`)
3. `join_actor_to_spot_erased`
   (`spot_runtime.cpp:12946-12959`, `:5245`)
4. `actor_join_context` → `actor_factory` → Actor instance lookup → `actor_admission`
   (`spot_runtime.cpp:5258-5268`, `:5318`, `:5353-5356`)
5. application `on_actor_join` callback 호출
   (`spot_runtime.cpp:5359-5363`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Actor type 조회 (`spot_runtime.cpp:12840-12847`) | `[매번]` | join record마다 통과 |
| 2 | entry Spot 여부와 local node RID projection (`:12868-12877`) | `[매번]` | 일반/entry 분기 전에 통과 |
| 3 | target Spot context 선택 (`:4747-4769`) | `[매번]` | 활성 target 확인마다 통과 |
| 4 | Actor factory 조회 (`:4787-4796`) | `[매번]` | join마다 통과 |
| 5 | Spot instance·serializer·source location projection (`:5281-5303`) | `[매번]` | handler 준비마다 통과 |
| 6 | 등록된 Actor instance 조회 (`spot_runtime.hpp:1867-1877`) | `[매번]` | warm instance여도 조회 turn은 통과 |
| 7 | Actor admission callback 조회 (`spot_runtime.cpp:4805-4822`) | `[매번]` | `on_actor_join` 호출 직전에 통과 |
| C1 | Actor type cache 채우기 (`:12848-12855`) | `[조건부]` | 이 노드에서 Actor ID를 처음 관찰한 join. authority 조회 뒤 1회 |
| C2 | Actor instance 설치 (`spot_runtime.hpp:1885-1903`) | `[조건부]` | target node에 instance가 없는 첫 join/materialization 때 1회 |
| C3 | target 동적 생성 후 재선택 (`spot_runtime.cpp:4771-4777`) | `[조건부]` | target Spot이 아직 없고 동적 factory가 하나로 결정될 때. activation 경로이므로 정상 hot path보다 훨씬 낮은 빈도 |

warm join은 **7회**다. C1과 C2가 함께 발생하는 첫 target-node join은 handler 호출 전
**9회**다. C3는 Spot 생성·Location 수명주기 전체로 분기하는 cold activation이므로 “메시지마다”
비용으로 보지 않았다. application `on_actor_join`은 별도 callback lane을 통하지 않고 직접
호출된다 (`spot_runtime.cpp:5361-5363`).

### 2.4 native timer fire → Spot timer handler

#### 선택한 경로와 진입점

진입점은 timer 등록 때 설치한 native `on_fire` callback이다
(`framework/languages/cpp/framework/src/runtime/timers/timer_runtime.cpp:121-125`). 정상적으로
활성화된 timer의 fire batch가 첫 application tick을 호출하는 경로를 선택했다.

#### 호출 체인

1. native `on_fire` callback (`timer_runtime.cpp:123-125`)
2. `timer_runtime_t::post_fire_count` (`:146-193`)
3. timer 전용 queue 또는 Spot serial queue에서
   `timer_runtime_t::dispatch_fire_count_async` 실행 (`:167-179`, `:325`)
4. timer application handler 호출 (`:394-398`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Spot serial queue snapshot (`timer_runtime.cpp:181-186` → `spot_runtime.cpp:2110-2114`) | `[매번]` spot-wide | native fire batch를 Spot queue에 게시할 때 통과 |
| 2 | callback admission·depth 증가 (`timer_runtime.cpp:356` → `spot_runtime.cpp:1988-1995`) | `[매번]` | batch의 handler loop 진입 전에 통과 |

spot-wide는 **2회**, timer 전용 queue를 쓰는 `per_actor`는 #1이 없어 **1회**다. bounded
catch-up이 한 fire batch에서 여러 tick을 만들면 #1과 #2는 batch의 첫 handler 전에 한 번만
통과하고, 같은 loop의 후속 tick 호출에는 추가되지 않는다 (`timer_runtime.cpp:380-399`). Timer
등록과 native timer 생성 (`timer_runtime.cpp:53-128`)은 `[초기화]`이며 발화 계수에는 없다.

## 3. 중첩 브리지

선택한 네 경로에서는 **`lane A.run(...).get()`의 lambda 안에서 다시
`lane B.run(...).get()`을 호출하는 중첩 브리지를 찾지 못했다.** 따라서 “lane A 완료를 기다리는
thread가 lane B 완료까지 함께 묶이는” 두 state lane 동기 대기는 0건이다.

다만 다음 실행기 중첩은 있다. 이것은 두 번째 동기 state-lane 대기가 아니므로 주 계수와 중첩
브리지 수에는 넣지 않았다.

- spot-wide Actor packet은 Actor serial queue turn에서 Spot serial queue로 비동기 게시한다
  (`spot_runtime.cpp:3864-3908`). 그 게시 과정이 callback lane snapshot을 동기 대기한다.
- Spot route와 spot-wide timer도 Spot serial queue에 비동기 게시한 뒤, handler turn에서
  callback lane admission을 동기 대기한다 (`spot_runtime.cpp:3917-3967`,
  `timer_runtime.cpp:181-186`, `:356`).

즉 queue turn을 보류하는 계층 중첩은 있지만, outer state lane의 `.get()`이 inner state lane의
`.get()`을 감싸 thread 두 개를 동시에 묶는 형태는 아니다.

## 4. dotnet 대조

| cpp 경로 | dotnet 대응 지점의 브리지 여부 |
|---|---|
| 원격 Actor packet | dispatcher와 serial executor는 async await다 (`ZLinkSpotActorPacketDispatcher.cs:8-35`, `ZLinkActorInboundPipeline.cs:101-132`). 다만 handler instance를 고를 때 `ZLinkActorRuntimeState.HandlerInstances`가 `AwaitStateLane`을 **1회** 사용한다 (`ZLinkSpotActivationConfiguration.cs:188-192`, `ZLinkActorRuntimeState.cs:98-120`). |
| Spot route dispatch | `ZLinkSpotRouteDispatcher.DispatchAsync`가 decode에서 handler까지 async로 이어지며 해당 파일에는 `AwaitStateLane`이 없다 (`ZLinkSpotRouteDispatcher.cs:14-57`, `:124-150`). 대응 handler 진입은 **브리지 아님**이다. |
| Actor join | warm Actor membership은 `ConcurrentDictionary` 조회이고 handler는 직접 await한다 (`ZLinkSpotActorMembership.cs:6-29`, `ZLinkSpotActorJoinDispatcher.cs:58-100`). handler 호출 전 대응 지점은 **브리지 아님**이다. |
| timer fire | 오히려 dotnet은 scheduler due pop, schedule 검증, `NotifyDue`, pending dispatch 준비에서 4회, Spot timer frozen 검사에서 2회로 `AwaitStateLane`을 **6회** 통과한 뒤 handler를 호출한다 (`ZLinkTimerScheduler.cs:102-147`, `ZLinkTimer.cs:161-185`, `:328-354`, `ZLinkSpotTimerRegistry.cs:26-27`, `ZLinkSpotActivationExecution.cs:1169-1176`, `:2084-2116`). |

설계 차이는 일률적이지 않다. dotnet의 Spot route와 join handler 경계는 async/동시성
컬렉션으로 cpp보다 브리지가 적지만, Actor handler instance 소유와 timer scheduler에는
동기 `AwaitStateLane`이 남아 있다.

## 5. 회수 우선순위

| 우선순위 | 경로 | `[매번]` 수 | 제거 난이도 | 판단 |
|---:|---|---:|---|---|
| 1 | 원격 Actor request/send | 13/11 | 높음 | 최고 빈도 경로이고 같은 authority fence를 두 번 읽는다. 다만 relocation·Message Follow·exactly-once·Actor/Spot 이중 queue 불변식을 함께 보존해야 한다. 먼저 `:9617`과 `:9707` projection 통합, `:9845`와 `:10024`의 materialization/dispatch projection 통합 가능성을 검토할 가치가 크다. |
| 2 | Actor join | 7 | 중간~높음 | handler 전 node lane read가 7회다. context/factory/instance/admission을 한 immutable projection으로 줄일 여지가 크지만 factory와 application callback은 반드시 lane 밖에 남겨야 한다. |
| 3 | Spot route dispatch | 5 | 중간 | route client·context·Location lifecycle을 수신 시점의 검증된 projection으로 묶고, callback admission을 이미 존재하는 Spot serial queue 소유로 옮길 수 있는지 검토한다. close/idle-eviction fence가 난점이다. |
| 4 | timer fire | 2/1 | 낮음~중간 | 별도 node lane이 없고 callback lane만 남는다. timer/Spot serial queue가 callback admission 상태까지 소유하도록 만들 수 있으면 회수 폭은 작지만 위험도도 비교적 낮다. |

회수의 첫 목표는 bridge primitive 자체를 바꾸는 것이 아니라, 이미 같은 메시지에서 반복하는
node-state projection을 한 turn으로 합치고 callback admission을 상위 serial 실행 단위가
소유하게 만드는 것이다. 새 lock이나 별도 cache를 추가하면 경로 비용만 다른 형태로 옮길 수 있다.

## 6. 수행/미수행 범위와 한계

### 수행

- 지정한 `state_lane.hpp`, `spot_runtime.hpp`, `spot_runtime.cpp`, 동시성 모델 초안 §6,
  실행기 계층 실측 문서를 정적으로 읽었다.
- 실제 timer 구현이 별도 파일에 있어 `runtime/timers/timer_runtime.{hpp,cpp}`를 추가로 읽었다.
- MeshNode receive callback부터 네 개의 구체적인 성공 경로를 handler 호출 문장까지 추적했다.
- 다중 행 `run(...).get()`을 함수 본문에서 직접 판정하고, 일반 `.get()`과 task/queue 대기를
  제외했다.
- 대응 dotnet 경로는 `AwaitStateLane` 여부만 정적으로 대조했다.

### 미수행·한계

- 소스와 스펙은 수정하지 않았고, 이 조사 문서 한 파일만 생성했다.
- git 명령, 빌드, 테스트, benchmark, runtime trace는 실행하지 않았다.
- 저장소 전체의 브리지 총개수나 제시된 근사치 327은 검증하지 않았다.
- `[조건부]` 빈도는 코드의 cache/수명 경계에서 추정한 값이다. 실제 workload의 Actor cold-hit,
  request/send 비율, Spot execution mode, timer catch-up 분포는 측정하지 않았다.
- 주 계수는 handler **호출까지**다. handler terminal 이후 callback-depth 감소, reply,
  Actor join commit·Location Store 갱신은 별도 후속 경로이며 포함하지 않았다.
- 에러, stale fence, teardown, relocation/Message Follow 우회는 handler에 도달하지 않거나 다른
  terminal을 갖는 경로라 선택한 성공 hot path의 `[매번]` 수에 합산하지 않았다.
