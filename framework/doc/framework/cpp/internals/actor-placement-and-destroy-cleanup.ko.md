# Actor 배치와 destroy 정리 경계

이 문서는 C++ Framework의 Actor 배치·destroy 경계를 유지보수자가 확인할 때 사용하는
구현 설명이다. Actor의 생성·조회·메시지·destroy에 대한 호출자 계약은 공통 Framework
spec과 C++ public contract가 기준이며, 이 문서는 그 계약을 구현하는 내부 책임만 설명한다.

## Destroy 순서

같은 `ActorId`를 다시 생성할 수 있으려면 이전 incarnation의 authority, reserved capacity,
creation reservation과 process-local Actor 상태가 함께 정리되어야 한다. authority만 먼저
삭제하면 다음 생성이 이전 reservation을 남은 용량으로 판단하여 `capacity_exceeded`가 될 수
있다. 따라서 destroy 경로는 local state 정리를 먼저 수행하고, Location Store의 authority
삭제는 provider가 capacity 감소와 해당 creation reservation 삭제를 하나의 conditional write로
처리하도록 구성한다.

`provider_location_repository_t`는 destroy CAS에서 authority row, target capacity와 reservation
record를 같은 store version 조건으로 갱신한다. CAS 충돌이면 어떤 항목도 부분적으로 제거하지
않으며, 재시도는 현재 authority와 target descriptor를 다시 확인한 뒤 수행한다. 오래된 target
descriptor가 owner lease와 맞지 않아 실패한 경우에는 현재 descriptor를 한 번 갱신하여
현재 lifecycle generation과 lease token으로 다시 판정한다.

local Actor state를 정리하는 callback은 authority 삭제보다 먼저 실행된다. 정리 실패를
authority 삭제 뒤에 발견하면 logical authority는 사라졌지만 process-local capacity와 gateway
상태가 남을 수 있으므로, 실패를 호출자에게 반환하고 authority 삭제를 진행하지 않는다.

## Callback 수명과 lock 경계

Spot runtime은 Actor 상태를 보호하는 node mutex를 잡은 상태에서 소유권 검증과 local route
제거만 수행한다. Mesh host cleanup callback은 이 mutex를 해제한 뒤 실행한다. callback이
Spot runtime 또는 Location Store를 다시 호출할 수 있으므로 외부 callback을 node mutex 안에서
호출하면 재진입 deadlock과 temporal coupling이 발생한다.

Mesh host의 destroy callback은 shared gate를 통해 수명을 관리한다. callback 진입 전에 gate가
정지 상태인지 확인하고 active count를 증가시키며, host stop은 새 진입을 막은 뒤 active
callback이 모두 반환할 때까지 기다린다. callback은 gate 진입에 성공한 뒤에만 host의 `this`를
사용한다. 이 순서로 stop 이후 Spot에 남은 callback이 해제된 host를 참조하지 않도록 한다.

## E2E 시나리오와 fixture 경계

공통 ToActorMessaging TA-B2는 ID-only 메시지와 exact `ActorRef` lifecycle operation의 세대
규칙을 함께 확인한다. C++ fixture는 생성·메시지·destroy에 `actor_manager_t`와
`actor_client_t` public API를 사용하며, 이전 `ActorRef` destroy는
`invalid_operation`으로 확인한다.

TA-B3의 route 장애는 caller와 current owner 사이의 network block을 요구한다. C++의
`mesh_peer_connections_t`는 구성 시점 목록만 보관하는 표면이 아니라, 실행 중 runtime의
connect/disconnect operation으로 연결되는 semantic Port다. `connect`와 `disconnect`는
목록을 갱신한 뒤 runtime callback을 호출하며, callback은 expected routing ID를 보존해
실제 Mesh transport operation을 수행한다. 따라서 fixture는 내부 registry를 읽지 않는다.
runner는 actor-b endpoint를 사용하는 다른 peer를 caller 연결로 잘못 세지 않도록 caller
process가 소유한 transport만 확인하고, route 상태는 `route_mesh_runtime_t`의 public peer
snapshot으로 확인한다.

explicit disconnect가 이미 admitted된 peer를 대상으로 실행되면 runtime은 해당 peer의
`connection_id`를 먼저 확보한 뒤 endpoint를 닫고 topology와 liveness registry에서 같은
connection을 함께 제거한다. socket만 닫고 registry를 남기면 admission이 stale peer를
선택하여 `request_failed`가 발생할 수 있으므로, 이 두 상태의 갱신은 하나의 disconnect
경계에서 수행한다.

topology entry가 monitor 또는 liveness 처리로 먼저 제거된 뒤 explicit disconnect가 호출될
수도 있다. 이 경우 endpoint를 기준으로 아직 후보 registry에 남은 physical connection을
모두 찾고, 각 후보의 routing ID와 `connection_id`를 사용해 후보·topology·liveness 항목을
함께 제거한다. endpoint 연결만 끊고 후보 registry를 유지하지 않으므로, 이후 같은 endpoint의
reconnect가 이전 physical connection을 다시 admission 대상으로 선택하지 않는다. 이 정리는
admitted peer가 이미 확인된 경우와 별도의 no-admitted 경로에서도 idempotent하게 적용된다.

Mesh liveness는 `public_host_runtime_t::dispatch_ready()`의 공통 유지보수 단계에서 평가한다.
timeout된 peer는 topology에서 제거되어 public snapshot에 `not_connected`로 반영된다. 다만
snapshot이 `not_ready`가 된 뒤에도 request admission이 deadline으로 끝나면 공통 계약의
`unavailable` terminal이 아니므로 TA-B3를 통과로 판정하지 않는다.

Actor request completion은 이 경계를 위해 request 대상 routing ID를 함께 관찰한다.
기존 요청의 deadline이 먼저 만료되거나 transport가 non-zero terminal record를 반환하더라도
그 시점에 대상 RouteMesh peer가 더 이상 admitted serving 상태가 아니면 `deadline_exceeded`
또는 일반 transport failure를 `unavailable`로 변환한다. 대상 peer가 계속 serving 상태라면
일반적인 처리 지연이나 application failure로 보고 원래 error 의미를 보존한다. 이 변환은
호출부가 transport 상태를 검사하게 만드는 우회가 아니라 actor client와 Mesh runtime 사이의
semantic error mapping이다.

현재 TA-B3는 차단 중 request의 `Unavailable` 판정, public `not_ready` 전파, 양방향 peer
reconnect 이후의 `ready` 전파와 reply routing을 함께 확인한다. runner는 상태 polling에
HTTP timeout을 적용하므로 transport나 HTTP endpoint가 응답하지 않아도 정리 절차가 무한히
대기하지 않는다.

Actor request의 wire terminal은 `request_failure_mapper_t::reply_header_exception()`에서
Framework public error kind로 변환한다. 호출부와 E2E fixture는 이 변환을 위해 `detail` 상태를
읽지 않으며, route를 사용할 수 없는 경우에는 공통 계약의 `unavailable`을 검증한다.

원격 Actor send/request의 admission은 topology에 peer가 있다는 사실만으로 수락하지 않는다.
`route_mesh_runtime_service_t`가 public peer snapshot에 적용하는 location state와 lifecycle
generation 비교를 내부 readiness resolver로 공유한다. topology peer가 없어졌거나 Location
descriptor가 serving 상태가 아니거나 generation이 일치하지 않으면 operation을 remote
handler에 전달하지 않고 `not_connected`로 거부한다. 이 결과는 상위 Actor client에서
`unavailable`로 변환되며, 이미 handler가 실행된 뒤 public snapshot을 다시 읽어 실패로
바꾸는 경로와 구분된다.

Actor client가 받는 `framework_exception_t`는 먼저 typed `kind()`를 보존한다. 따라서
Framework 내부 오류 메시지에 `stale`이나 `not found`라는 단어가 포함되어도 다른 public
오류로 바뀌지 않는다. typed kind가 없는 native `std::exception`은 binding transport가
제공하는 legacy 오류 경로로 한정하며, 이 경로에서만 호환성을 위해 알려진 transport 오류
문구를 해석한다.
