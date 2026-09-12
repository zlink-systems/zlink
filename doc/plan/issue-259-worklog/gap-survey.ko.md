# #259 언어별 gap 조사 — completion 예약 상한

조사 2026-09-12, `271687cc25` 기준. 정적 대조만 했다.
인계 문서 `handoff-2026-09-12-framework-perf-and-hwm.ko.md` §3.0의 전수 조사를 `file:line`까지
다시 확인한 결과이며, 한 가지가 달랐다 — **언어마다 상한이 하나가 아니라 둘이다.**

## 1. 무엇을 지우는가

지우는 것은 **completion 예약 상한과 그 상한에 걸렸을 때의 거부**뿐이다.
`MaxQueuedApplicationJobs`, 80 %/60 % 경계, profile 계산(processor 수 × profile별 job 수,
`Balanced` = 128)은 건드리지 않는다. 그 둘은 세는 방향이 다르다 — job queue는 이 host가 **받아서**
handler를 기다리는 job을 세고, completion 예약은 이 host가 **보낸** 호출을 센다.

## 2. 언어별 현황

각 언어가 상한을 **두 층**에 둔다. Node만 두 번째 층이 없다.

| 언어 | ① 인스턴스별 pending 표 상한 | ② process 공유 dispatcher 예약 상한 |
|---|---|---|
| C++ | `operation_registry.cpp:311` `_pending.size () >= _capacity`. `_capacity`는 생성 인자이며 `raw_mesh_node_owner.cpp:616-617`이 `default_operation_capacity`(=4096, `operation_registry.hpp:21`)를 넘긴다 | `operation_registry.cpp:145` `_state->reserved >= default_operation_capacity` (`try_admit`) |
| Java | `ZLinkServiceOperationRegistry.java:341` `entries.size() >= maxPendingOperations`. 기본값은 같은 파일 `:23` `DEFAULT_MAX_PENDING_OPERATIONS = 4_096` | `ZLinkServiceCompletionDispatcher.java:11` `CAPACITY = 4_096`, `:36` `reserved >= CAPACITY` (`tryReserve`) |
| .NET | `ZLinkMeshCompletionTable.cs:57` `_outstandingOperations >= _capacity`. 기본값은 같은 파일 `:10` `DefaultCapacity = 4_096` | `ZLinkCompletionDispatcher.cs:55` `_reservations >= _capacity`, 기본값 `:8` `DefaultCapacity = 4_096` |
| Node | `operation-registry.ts:74` `entries.size >= maxPendingOperations` (기본값 `:53` `DEFAULT_MAX_PENDING_OPERATIONS = 4_096`)와 `mesh-completion-table.ts:69` (기본값 `:14` `ZLINK_MESH_COMPLETION_CAPACITY = 4_096`) | **없다** |

## 3. 거부 지점

| 언어 | 거부 |
|---|---|
| C++ | `register_operation`이 `false` 반환 → `raw_mesh_node_owner.cpp:1389` request throw. send는 `:1475`가 `send_start_result_t::capacity_exceeded`를 반환하고 `:1530`·`:1562`가 throw. `:1582`·`:3280`·`:3328`·`:3361`·`:3371`·`:3452`도 같은 값을 분기한다 |
| Java | `ZLinkServiceOperationRegistry.java:298-302` `capacityExceeded()` → `ZLinkFrameworkException(CAPACITY_EXCEEDED)`. `:341`(표)과 `:356`(dispatcher) 두 곳에서 던진다 |
| .NET | `ZLinkMeshCompletionTable.cs:58-60`(표)과 `:72-77`(dispatcher) 두 곳에서 `ZLinkFrameworkErrorKind.CapacityExceeded` |
| Node | `operation-registry.ts:75` `OperationCapacityExceededError`, `mesh-completion-table.ts:68-71` `ZLinkFrameworkErrorKind.CapacityExceeded` |

## 4. 소유 단위

| 언어 | RouteMesh | ClientServer |
|---|---|---|
| C++ | MeshNode별 registry(`raw_mesh_node_owner.cpp:616`) + process 공유 dispatcher | `raw_client_server_owner.hpp:10`이 같은 header를 include한다. 실제 사용 여부는 담당 job이 확인한다 |
| Java | `ZLinkJavaRawMeshNode` | `ZLinkChannelCallRuntime`이 같은 registry를 쓴다 — **네 언어 중 유일** |
| .NET | `ZLinkDotNetBackendRuntimeContext.cs:123`, `ZLinkBackendSpotNodeWrapper.cs:49`가 표를 만든다 | `ZLinkClientServerMessageBound.cs:24`의 `CapacityExceeded`가 이 상한에서 오는지는 담당 job이 확인한다 |
| Node | `spot-node-runtime-manager.ts:322` `new ZLinkMeshCompletionTable()`, `raw-service-mesh-runtime.ts:156` `new OperationRegistry()` — 인스턴스별 | `service-stateful-registry.ts:618`도 `OperationRegistry`를 받는다 |

## 5. 이 조사가 정하지 않은 것

각 언어 job이 자기 언어 안에서 전수로 확인한다.

- 같은 성격의 상한이 위 표 밖에 더 있는지
- ClientServer 경로가 이 상한을 실제로 지나는지
- 상한을 지운 뒤 남는 `CapacityExceeded` 사용처가 모두 다른 자원(Spot placement, 실행 객체별
  FIFO, worker queue)인지
