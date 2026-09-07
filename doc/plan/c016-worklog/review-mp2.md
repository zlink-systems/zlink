# review-MP-2 독립 리뷰

독자: MP-2의 채택과 수정 범위를 결정하는 감독자.

현재 diff는 채택할 수 없다. 같은 REPLY token의 중복 제출 결과가 확정 계약과 다르며,
만료 sequence 정리의 잠금·socket 수명, 할당 실패와 RID 제거 후 slot 반환에 차단 항목이 있다.
Caller별 조립과 기존 physical admission을 결합하는 A안 자체는 유지할 수 있다.

## 범위와 판정 기준

- 구현: `/home/hep7hep7/project/zlink-work/mp2`의 `git diff HEAD`, 기준 commit
  `3a7db427bce26bb155cdc7c5f45b3f52edb5f2c3`, 24개 파일 +1512/−509.
- 계약: **main 작업 트리** `/home/hep7hep7/project/zlink/core/doc/spec`의 미커밋 수정까지 포함.
- 설계: `core-rf-MP-1-design.md` §4.1–4.3·§7, `decisions.ko.md:1911`의 D-B197 및 `:1919`의 D-B198.
- 구현 보고서: `core-rf-MP-2-report.md`. 그 파일의 실행 결과는 구현자의 보고이며 독립 재실행 결과가 아니다.
- 소스·스펙·테스트를 수정하지 않았고 빌드, 테스트, benchmark, sanitizer를 실행하지 않았다.
  아래 재현 시나리오는 **코드로 도출한 실행 순서 및 후속 검증 제안**이다.
- 아래 `core/src/...`, `core/tests/...` 행 번호는 **mp2**, `core/doc/spec/...`와 `doc/plan/...` 행 번호는 **main**이다.
  B는 채택 전 수정이 필요한 항목, W는 비차단 관찰·검증 한계, S는 정리 제안이다.

## 차단 항목

### B01 — 같은 REPLY token의 중복 checkout 결과 변경

- 근거: `core/src/api/socket/socket_request_reply_submit_api.cpp:318`은
  `take_router_reply_target_locked`의 모든 false를 `ENOENT`로 바꾼다.
  `core/src/api/socket/socket_request_reply_runtime_io.cpp:680`은 이미 checked-out인 token도 false로 반환한다.
  `core/tests/integration/test_phase3_request_reply_contract.cpp:1658`은 **첫 token과 같은 token**을
  다른 thread에서 제출하고, `:1665`에서 기대값을 `NOT_FOUND/ENOENT`로 바꾸었다.
- 계약: main `core/doc/spec/core/socket/README.ko.md:1131`은 같은 token의 두 번째 sequence에
  **`INVALID_STATE/EBUSY`, 현재 part만 소비, 첫 sequence 유지**를 요구한다.
  D-B198의 D1–D5는 이 오류 결과를 변경하지 않는다. 구현 보고서의 “REPLY 두 번째 token”이라는
  설명은 해당 테스트 입력과도 다르다.
- 재현 시나리오: A가 token T로 REPLY MORE 성공 후 대기한다. B가 같은 RID/T로 FINAL을
  제출한다. 현재 구현은 NOT_FOUND/ENOENT, 계약은 INVALID_STATE/EBUSY다. A의 FINAL은 계속 성공해야 한다.
- 수정 방향: registry를 유일한 checkout 소유자로 유지하면서 이미 checkout된 token과
  없는·소비된·revoked token을 구별한다. 중복 token 테스트의 EBUSY 계약을 복구하고,
  **서로 다른** token의 동시 성공 테스트와 구분한다. 스펙을 구현에 맞춰 바꾸면 안 된다.

### B02 — DONTWAIT FINAL의 만료 payload 해제가 physical sync 안에서 실행

- 근거: REQUEST의 `core/src/api/socket/socket_request_reply_submit_api.cpp:831`은 complete scope를
  먼저 얻고 `:852`에서 helper prepare를 호출한다. `core/src/api/socket/part_helper_api.cpp:738`의
  만료 회수 루프는 helper mutex만 풀고 `:744`에서 sequence를 정리한다.
  `:633`의 `zlink_msg_close`가 실행될 때 호출자의 complete scope는 여전히 존재한다.
  SEND도 `core/src/api/socket/socket_message_send_api.cpp:414` → `:428`에 같은 경로가 있다.
- 계약·설계: MP-1 `:122`, `:145`는 zero-copy free 함수의 재진입을 고려하여 payload를
  **helper mutex와 physical sync 모두의 밖**에서 해제하도록 요구한다.
  실제 free callback 호출은 `core/src/runtime/core/msg.cpp:430`이다.
- 재현 시나리오: A/B가 같은 DEALER에서 각각 MORE를 보관한다. A의 zero-copy prefix free
  callback은 같은 socket의 option API를 호출하도록 한다. A가 종료한 뒤 B가 DONTWAIT REQUEST
  FINAL을 제출하면, B가 complete sync를 얻은 상태에서 A의 만료 prefix를 닫는다.
  callback의 `setsockopt`는 `core/src/runtime/sockets/common/socket_base_api.cpp:674`에서 같은
  sync를 다시 얻으려 하고 `socket_lifecycle_runtime.cpp:391`의 대기에서 진행하지 못한다.
  SEND는 사전 active 조회와 complete-scope 획득 사이에 A가 종료하면 같은 문제가 된다.
- 수정 방향: 잠금 아래에서는 만료 slot의 소유권만 분리하고, complete scope를 해제한 뒤
  payload/context를 닫는다. “helper unlock 후 해제”만으로 충분하지 않다.
  DONTWAIT FINAL에 generic scope를 추가로 중첩하여 RMW를 늘리는 방식도 피한다.

### B03 — 첫 TLS identity 할당 실패가 C API 밖으로 전파

- 근거: `core/src/api/socket/part_helper_api.cpp:25`의 TLS 초기화는 `make_shared`로 할당한다.
  `:44`의 조회가 이를 호출하며, `:256`의 최초 caller 조회는 `:271`의 try **밖**이다.
  `current_send_sequence_active`의 `:782`와 REQUEST의
  `core/src/api/socket/socket_request_reply_submit_api.cpp:1018` → `:1020`에도 이를 감싸는 catch가 없다.
  초기 validation abort 역시 같은 조회를 사용한다(`part_helper_api.cpp:960`).
- 계약: main `core/doc/spec/core/socket/README.ko.md:1036`의 allocation failure는
  `OUT_OF_MEMORY/ENOMEM`, ID 0, part 소비다. REPLY도 `:1123`에 같은 결과를 규정한다.
- 재현 시나리오: A가 MORE로 socket map을 비어 있지 않게 유지한다. 이 library의 send helper를
  처음 호출하는 B에게 미리 초기화한 part를 전달하고 B의 TLS `make_shared` 할당을 실패시킨다.
  B의 MORE 또는 single FINAL에서 `std::bad_alloc`이 public 함수 밖으로 전파하며 part도 소비되지 않는다.
  C/FFI caller 또는 예외를 잡지 않는 worker에서는 프로세스 종료로 이어질 수 있다.
- 수정 방향: identity의 최초 생성 실패를 기존 ENOMEM 결과로 수렴시키고, 실패 정리 자체가
  identity 할당을 다시 시도하지 않도록 한다. 새 caller의 조회는 아직 생성되지 않은 identity를
  “자기 sequence 없음”으로 판정할 수 있다. 생성·조회·abort의 예외 경계를 한 소유자에서 정한다.

### B04 — close가 seal 이후 새 allocation에 의존

- 근거: `core/src/api/socket/part_helper_state.cpp:57`에서 helper state를 비공개로 만든 뒤,
  `:68`에서 slot 수만큼 vector를 `reserve`한다. 예외 처리와 close rollback이 없다.
  이 호출 전에 `core/src/api/core/zlink.cpp:144`·`:146`은 public handle과 lifecycle을 seal했다.
  실제 close 완료는 cleanup 뒤의 `:182` 및 같은 파일 `:113`에서 수행된다.
- 계약: main `core/doc/spec/core/socket/README.ko.md:961`은 socket close가 모든 미완성 sequence를
  폐기한다고 규정한다. MP-1 §4.3도 owner의 FINAL에 의존하지 않는 정리를 요구한다.
- 재현 시나리오: MORE slot이 하나 이상 있는 socket을 메모리 부족 상태에서 close하고
  `reserve` 할당을 실패시킨다. 예외가 `zlink_close` 밖으로 나가며 close handoff·endpoint 정리가
  끝나지 않는다. 이후 public handle은 이미 닫힘 상태라 정상 재시도도 할 수 없다.
- 수정 방향: 기존 `send_sequence_store_t` 전체를 로컬 컨테이너로 swap/move하여 할당 없이
  분리하고 잠금 밖에서 닫는다. OOM을 catch하여 미정리 상태로 성공 반환하는 방식은 안 된다.

### B05 — logical RID 제거 후 staged REPLY의 registry capacity 미반환

- 근거: `core/src/runtime/sockets/router/router.hpp:82`의 logical RID 제거는 reply registry를 revoke한다.
  `core/src/api/socket/socket_request_reply_runtime_io.cpp:826`은 checked-out entry에 revoked 표시만 하고
  checkout·slot을 감소시키지 않는다. 실제 반환은 caller context의 abandon/commit
  (`socket_request_reply_submit_api.cpp:375`, `:385`)까지 지연된다.
  diff는 기존 public staged reply의 checkout·slot을 revoke 시 반환하던 블록을 삭제했다.
- 계약: main `core/doc/spec/core/socket/README.ko.md:1137`·`:1140`은 logical RID 제거를
  token 및 보유 checkout·slot 수명의 끝으로 명시한다. `:1142`의 한도는 socket당 65,536개다.
- 재현 시나리오: A가 RID X의 token으로 REPLY MORE를 성공시키고 thread는 종료하지 않은 채
  FINAL 없이 대기한다. 다른 thread가 X를 명시적으로 제거해도 해당 slot은 남는다.
  X의 token 하나와 다른 RID Y의 token 65,535개로 registry를 채운 상태라면 X를 제거해도
  Y의 다음 REQUEST를 public receive로 꺼낼 capacity가 회복되지 않는다.
  한도 판정은 `socket_request_reply_runtime_io.cpp:63`이다.
- 수정 방향: registry가 logical revoke에서 capacity를 반환하도록 한다. 진행 중 physical FINAL의
  pipe lifetime pin과 public token slot 반환을 구별하고, 늦게 오는 context restore/commit은
  이미 반환된 token을 다시 감소시키지 않아야 한다. socket-wide active owner를 되살릴 필요는 없다.

### B06 — validation abort가 REPLY context 해제 전에 socket pin을 놓음

- 근거: `core/src/api/socket/part_helper_api.cpp:950`의 지역 handle은 `:954`에서 파괴된다.
  이후 `:964`로 분리한 REPLY context는 payload를 닫은 뒤 `:972`에서 해제된다.
  `router_reply_sequence_context_t::abandon`은 registry를 restore하고
  (`socket_request_reply_submit_api.cpp:389`), closing/revoked entry이면
  `socket_request_reply_runtime_io.cpp:716` → `:743` → `:20`에서 raw `state_->socket`을 사용한다.
  `socket_request_reply_state_t`는 socket을 shared ownership으로 보유하지 않는다
  (`socket_request_reply_internal.hpp:504`).
- 재현 시나리오: A가 REPLY MORE를 성공한 뒤 NULL part 또는 잘못된 flags로 호출하여
  public-handle guard가 없는 초기 validation abort에 들어간다
  (`socket_request_reply_submit_api.cpp:1218`, `socket_message_send_api.cpp:605`).
  A가 slot/context를 분리한 후 zero-copy free callback에서 잠시 대기하게 하고, B가 socket을 close한다.
  cleanup map은 이미 비어 있으므로 close는 완료될 수 있다. reaper가 socket을 파괴한 뒤 A를
  진행시키면 context restore의 slot-release notification이 해제된 socket에 접근한다.
- 수명 확인: `socket_base_lifecycle.cpp:1543`·`:1583`은 public pin이 없으면 socket finalization을
  허용한다. context의 pipe pin은 socket pin이 아니다. `pipe.cpp:2940`은 남은 pipe lifetime ref와
  별개로 socket에 termination 완료를 알리고, pipe 자체 삭제만 늦춘다.
- 수정 방향: 기존 public-handle pin을 abort 함수 전체, 특히 detached payload와 context의
  해제 완료까지 유지한다. helper state의 shared_ptr만 유지하는 것으로 대체할 수 없다.
  해제는 계속 helper mutex 밖에서 수행한다.

## 비차단 항목

### W01 — 동시 SEND 테스트의 payload 검증 생략

- 근거: `core/tests/integration/test_public_inproc_multipart_send.cpp:1041`은 두 part의 크기가
  정확할 때만 내용·caller·sequence·중복을 검사한다. 크기가 다르면 오류 assertion 없이 건너뛰며,
  `:1063` 뒤에 `seen` 전체 검증도 없다.
- 재현 시나리오: receiver에 400개의 2-part record가 도착하되 한쪽 part의 길이가 틀리거나 전부
  빈 part인 회귀를 가정하면 이 테스트의 핵심 검사가 생략된다.
- 수정 방향: part 크기를 먼저 무조건 assert하고 내용을 검사한다. caller/sequence의 정확한 집합을
  확인한다. 보고서의 “400 record 전수 확인, 누락 없음”은 현재 assertion보다 강한 주장이다.

### W02 — REQUEST completion과 제출 ID의 대응 검증 누락

- 근거: `core/tests/integration/test_phase3_request_reply_contract.cpp:1805`에서 반환 ID를
  nonzero로만 검사하고 버린다. `:1899` 이후 completion은 reply payload만 검사한다.
  `:1905`의 memcpy 앞에 part 길이 검사, `:1913`의 배열 접근 앞에 범위 검사도 없다.
- 재현 시나리오: 서로 다른 caller의 completion ID 또는 user context가 뒤바뀌어도 reply payload
  집합만 보존되면 통과할 수 있다. 손상된 payload는 assertion보다 먼저 범위 밖 접근을 유발할 수 있다.
- 수정 방향: `(caller, sequence)`별 반환 ID·context를 보관하고 completion ID·context·payload를
  함께 대조한다. 길이와 index 범위를 먼저 검사한다. “80개 고유 completion”은 현재 테스트로
  ID의 고유성까지 확인한 결과가 아니다.

### W03 — OOM expectation 이동으로 checkout 이후 실패 coverage 축소

- 근거: `core/tests/unittest/unittest_phase3_request_reply_owners.cpp:478`은 첫 MORE 전에
  `request_reply_allocation_reply_key` failpoint를 건다. 구현의 failpoint는
  `socket_request_reply_submit_api.cpp:308`이며 실제 checkout `:318`보다 앞이다.
- 재현 시나리오: 이미 checkout된 token을 OOM abort가 restore하지 못하는 회귀가 생겨도,
  새 테스트는 checkout 전 실패하므로 이를 발견하지 못한다.
- 수정 방향: 첫 진입 allocation의 위치 변경 자체는 타당하다. 다만 첫 MORE 성공 후의 staging
  allocation 실패, checkout 직후 context allocation 실패를 별도로 검증하여 token 반환과 다른
  caller의 재제출을 확인한다. 기존 FINAL runtime failure 테스트는 OOM 경계를 대신하지 않는다.

### W04 — helper 잠금·탐색 비용이 설계 표와 다름

- 근거: SEND FINAL은 `socket_message_send_api.cpp:645` → `part_helper_api.cpp:774`에서 mutex를
  얻고 다시 `socket_message_send_api.cpp:428` → `part_helper_api.cpp:739`에서 얻는다.
  REQUEST는 `socket_request_reply_submit_api.cpp:1018`의 사전 조회가 추가된다.
  만료 검사는 `part_helper_api.cpp:69`에서 map 전체를 훑으며, 만료 entry마다 처음부터 반복한다.
- 재현 시나리오: k개의 살아 있는 MORE slot을 유지하고 하나의 caller가 MORE/FINAL 또는 single
  FINAL을 반복한다. part 처리마다 O(k) 만료 검사가 들어가며, SEND 2-part는 helper mutex가
  설계의 2회가 아니라 **3회**, helper가 이미 있는 REQUEST 2-part는 통상 **4회**다.
- RMW 판정: DONTWAIT buffered FINAL의 **lifecycle** scope 중첩에 의한 +2 RMW는 없다.
  MORE enter/leave 2, FINAL complete enter/leave 2는 MP-1 §7과 맞는다.
  다만 `owner_less<weak_ptr<...>>` map에 shared_ptr로 find하는 `part_helper_api.cpp:44`·`:55`는
  임시 weak_ptr을 만든다. 현재 libstdc++13의 `shared_ptr_base.h:1146`·`:1163` 및 `:203`·`:208`에
  weak count 증가·감소가 있어 각 lookup에 추가 refcount RMW가 생긴다. 이는 lifecycle RMW와 별도다.
- Single 판정: helper 미생성 SEND는 기존 빠른 경로를 유지한다. helper가 생성된 뒤에는 빈 map이어도
  mutex를 얻으며, 다른 caller slot이 있으면 TLS 최초 할당과 map 조회까지 발생한다.
  single REPLY에도 `socket_request_reply_submit_api.cpp:548`의 context allocation이 새로 들어간다.
  TLS 함수의 최초 초기화 guard와 주소 접근 비용도 남으며, 실제 TLS instruction 비용은 이 리뷰에서
  계측하지 않았다. 초기화 후 비용을 단순한 thread ID 읽기와 같다고 볼 근거는 없다.
- 수정 방향: 조회와 prepare를 한 helper 임계 구간으로 합치고, 불필요한 임시 weak ownership 및
  반복 전체 scan을 줄인다. 새 TLS raw socket cache나 이중 active map은 만들지 않는다.
  MP-1 `:251`이 요구한 single-after-multipart와 4-thread multipart/혼합 측정은 구현 보고서에 없으므로
  기존 5셀 PASS만으로 이 비용을 판정하지 않는다. 이번 리뷰에서는 측정하지 않았다.

### W05 — MORE의 context termination 검사 누락

- 근거: `socket_message_send_api.cpp:654`, `socket_request_reply_submit_api.cpp:1187`·`:1248`의
  MORE는 새 generic admission 이후 staging한다. `socket_base_msg.cpp:279`와
  `socket_runtime.hpp:851`의 generic scope는 closing만 검사하며 context termination을 검사하지 않는다.
  `socket_base.cpp:346`의 stop은 `_ctx_terminated`를 직접 설정한다.
- 재현 시나리오: context shutdown 후 SEND MORE 또는 REQUEST MORE를 호출하면, physical submit이
  없는 경로는 MORE OK를 반환하며 payload를 더 보관할 수 있다. FINAL과 close에서야 정리된다.
- 판정·수정 방향: MP-1 `:140`은 MORE도 `is_ctx_terminated()`를 확인하도록 요구한다.
  이전 staging 코드에도 같은 누락이 있어 MP-2가 새로 만든 회귀로 세지는 않았다.
  staging admission 소유 경로에서 기존 ETERM 결과를 유지하고 staged caller+blocked FINAL+shutdown을
  함께 검증한다. 이 finding만으로 ctx term hang을 주장하지 않는다.

### W06 — TLS destructor 안에서의 FINAL 경계

- 근거: `part_helper_api.cpp:25`의 identity는 일반 TLS destructor 순서를 따른다.
  `:72`의 만료 판정은 API가 실행 중인지를 별도로 보지 않는다.
  새 identity 테스트 `test_helper_interleave.cpp:140`은 thread를 join한 뒤 새 thread가 FINAL하는 경우다.
- 재현 시나리오: application TLS finalizer를 먼저 초기화하고 첫 MORE로 library identity를 나중에
  초기화한다. thread 종료 시 identity가 먼저 파괴되고 application finalizer가 FINAL을 호출할 수 있다.
  이때 자신의 slot도 expired로 회수된다. map이 비면 FINAL이 single로 제출될 수 있고, 다른 slot이
  남아 있으면 이미 파괴된 identity 객체를 다시 참조하는 경계까지 생긴다.
- 판정·수정 방향: 정상 thread 함수에서 실행 중인 FINAL과 TLS 소멸이 동시에 일어나는 것은 아니다.
  그러나 종료 중 TLS destructor 호출의 지원 범위는 현재 문서에 별도 설명이 없다. 이 경계를
  감독자가 확인하고 해당 시나리오를 검증해야 한다. 새 thread ID 재사용 테스트만으로
  “thread 종료 중 FINAL도 안전”이라고 확대 판정하지 않는다.

### W07 — 동시성 테스트의 미검증 경로와 실패 종료

- 근거: 새 SEND 공통 fixture는 `test_public_inproc_multipart_send.cpp:988`·`:1008`에서 NONE만
  사용하고 `:1066` 이후 연결은 inproc뿐이다. REQUEST도 `test_phase3_request_reply_contract.cpp:1807`이
  NONE이며 requester는 DEALER다. `test_helper_interleave.cpp:140`의 identity 검증은 일반 copied payload다.
- 재현 시나리오: DONTWAIT의 physical-sync→helper 경로, 3-part의 중간 MORE 실패, 5-part 이상 spill,
  다른 RID 두 개의 동시 ROUTER 제출, 같은 thread가 socket 두 개에 각각 보관한 sequence, 여러
  caller slot이 있는 close, MORE 실행 중 close, 종료와 blocked FINAL의 경쟁은 이 신규 fixture를
  실행해도 관찰되지 않는다. 기존 single close race는 MORE의 generic admission을 검증하지 않는다.
- 동기화 판정: MORE 이후 barrier는 동시 조립을 실제로 강제하므로 적절하다. REQUEST fixture의
  `:1771`에 있는 settle sleep은 경쟁 구간의 barrier를 대신하지는 않지만 readiness 보장도 아니다.
  SEND의 `:983`·`:1002`에서 한 worker가 allocation 실패로 return하면 나머지는 barrier에서 멈춘다.
  `unittest_complete_record_admission.cpp:72`의 init 실패도 `more_done`을 발행하지 않는다.
- 수정 방향: 위 경계를 기존 hook/barrier와 free counter로 고정하고 실패 시 모든 worker가 빠져나오도록
  한다. 같은 caller의 RID 불일치 뒤 기존 RID로 single FINAL을 보내는 경우는
  `test_helper_interleave.cpp:537`이 검증한다. 바뀐 RID로 새 MORE→FINAL을 시작하는 경우와
  **다른 caller의 열린 sequence가 함께 유지되는 조건**은 추가 확인이 필요하다.

### W08 — Message 문장의 socket 적용 범위

- 근거: main `core/doc/spec/core/02-message.ko.md:97`은 multipart 지원 socket에 적용한다고 하며,
  `:108`의 새 문장은 같은 socket에 여러 thread가 multipart를 동시에 보낼 수 있다고 한정 없이 적는다.
  반면 socket README `:49`의 신규 보장은 PAIR·DEALER·ROUTER에 한정되어 있고,
  `part_helper_api.cpp:258`은 PUB/XPUB의 다른 caller 진입을 계속 거절한다.
- 시나리오: Message 절만 읽은 PUB/XPUB 사용자가 두 thread의 MORE를 동시에 제출하면 해당 문장의
  일반적인 해석과 달리 두 번째 caller는 EINVAL을 받는다.
- 제안: 감독자가 보호 문서의 동시 multipart 문장에 P/D/R 적용 범위를 명시한다.
  이 리뷰에서 스펙은 수정하지 않았다. PUB/XPUB의 계약을 확대하는 구현 변경을 제안하는 것이 아니다.

## 코드 정리 제안

### S01 — 해제 책임의 중복 정리

- 근거: `part_helper_api.cpp:627`, `:919`, `:946` 및 `part_helper_state.cpp:75`에 payload close와
  context reset 순서가 반복된다. `socket_request_reply_submit_api.cpp:548`·`:617`에는 context 생성과
  실패 시 token/pipe 복구가 반복된다.
- 시나리오: 해제 순서나 errno 보존을 한 경로에서 바꿔도 다른 경로가 남아 B02/B06 같은 차이가 생긴다.
- 제안: 기존 sequence 소유자를 분리한 뒤 닫는 동작으로 정리하여 callback의 잠금·pin 조건을
  한 곳에서 확인할 수 있게 한다. 추가 관리 계층이나 같은 token 상태의 두 번째 registry는 불필요하다.

### S02 — 남은 marker 주석의 적용 범위

- 근거: `socket_send_complete.cpp:358`은 complete PAIR 전송 설명에서 “multipart marker”가
  다른 physical sender를 배제한다고 적지만 실제로는 complete scope의 public sync가 배제한다.
  `part_helper_internal.hpp:199`의 “suspend/reset one helper step”도 P/D/R까지 읽으면 맞지 않는다.
- 시나리오: 후속 변경자가 P/D/R FINAL의 원자성을 marker에 의존한다고 오해할 수 있다.
- 제안: complete scope와 PUB/XPUB incremental marker의 소유 범위를 주석에 구분한다.
  `send_scope`, `owner_thread`, suspend/resume, boundary 함수 자체는 PUB/XPUB에서 여전히 사용하므로
  죽은 코드로 삭제하면 안 된다. production의 `prepare_send_step` 호출은 publish에 남는다
  (`socket_message_send_api.cpp:298`).

## 관점별 최종 판정

| 관점 | 판정과 근거 |
|---|---|
| 1. caller identity·만료·수명 | `owner_less`와 socket-owned map(`part_helper_internal.hpp:84`)은 raw thread ID 재사용을 피한다. 현재 caller의 일반 FINAL 중에는 TLS shared owner가 유지된다. 성공 FINAL의 slot erase(`part_helper_api.cpp:887`), 오류의 buffer/context 분리(`:936`), close의 전체 map 회수는 소유자를 구분한다. **B03/B04/B06 차단**, 종료 경계 W05/W06. |
| 2. lock 순서 | P/D/R MORE는 lifecycle→helper이고 physical sync를 취득하지 않는다. DONTWAIT FINAL은 sync→helper→unlock→physical, blocking FINAL은 helper를 놓고 physical에 진입한다. publish의 helper→multipart sync는 다른 socket type 경로로 남는다. **만료 해제의 B02는 이 규칙을 위반한다.** |
| 3. physical 원자성·registry | SEND의 `try_send_parts_scoped_once`(`socket_send_submit.cpp:208`)가 기존 전체 record admission을 사용하며 `socket_send_complete.cpp:413`은 scope 해제 전 rollback한다. REQUEST observer는 `socket_request_reply_submit_api.cpp:91`에서 registry mutex를 잡고 pipe의 `_out_sync` 이전 prepare→commit-before-flush→finish를 유지한다(`pipe.cpp:2574`, `:2601`, `:2608`, `:2616`). REPLY도 하나의 complete sync와 `transport_sync` 아래 write/rollback한다(`socket_request_reply_runtime_io.cpp:1378`, `:1429`). 일반 성공 record가 다른 caller part와 섞이는 새 경로는 찾지 못했다. **token 오류·revocation은 B01/B05**, 실패 정리는 B02/B06. |
| 4. 확정 계약·control·HWM | main README:49·:950, ROUTER:54의 thread별 family/RID 독립성에 해당한다. MORE는 buffer에만 move하며 HWM·MAXMSGSIZE는 FINAL의 기존 frame admission을 사용한다(`pipe.cpp:3550`, `:3595`); total-known 예외 확대는 없다. P/D/R는 marker/boundary를 설정하지 않아 D3 control이 진행한다(`part_helper_api.cpp:162`, `:854`; `socket_base_flow_state.cpp:73`). ZMP:244·:474 및 ROUTER:420과 부합한다. **README:1131의 중복 token 오류는 불일치(B01).** Message:108의 socket 범위는 W08. |
| 5. 성능 | lifecycle scope +2 RMW 중첩은 없다. helper 추가 mutex, 전체 map scan, 임시 weak count 및 single REPLY allocation은 **W04**. MP-1의 warm/혼합 측정 증거가 부족하다. |
| 6. 테스트 | barrier 기반 정상 동시 조립은 관찰한다. payload·ID 검증의 한계 W01/W02, OOM coverage W03, 미검증 경로 W07이 남는다. 변경 expectation 중 중복 token 오류는 승인 계약에 반한다. |
| 7. 코드 품질 | public owner 필드 제거와 token registry 재사용은 단순화 방향에 맞는다. 실제 payload 해제 조건이 경로마다 달라 S01 정리가 필요하다. marker/suspend/resume 전체 삭제는 PUB/XPUB를 깨뜨리므로 S02 범위로 주석을 정리한다. |

REQUEST reservation은 MORE가 아니라 FINAL에서 기존 `ensure_socket_pull_pending_request`를 호출한다
(`socket_request_reply_submit_api.cpp:1035`, `:1082`). completion reservation과 nonzero sequence 발급은
`socket_request_reply_pending_api.cpp:319`·`:330`, 실패 반환은 `:257`에 유지된다.
Observer의 commit은 같은 registry lock 아래 수행되므로 `_out_sync`를 잡은 뒤 registry lock을
새로 얻는 역순으로 보지 않았다. 검토한 변경 경로에서 registry lock→helper lock의 새 직접 역순은
찾지 못했다. 이는 기존 monitor/ctx 전체 잠금 그래프나 미실행 TSan의 무결함 판정은 아니다.

## 기존 expectation 변경 판정

| 구현 보고서의 묶음 | 판정 |
|---|---|
| 다른 caller single FINAL | 타당. `test_helper_ownership.cpp:126`의 성공과 별도 single 수신은 D1에 해당한다. |
| complete admission과 열린 public staging의 공존 | 타당. `unittest_complete_record_admission.cpp:48`·`:570`은 물리 write 배제와 논리 조립을 구분한다. 다만 완료 record 집합 검증은 더 강화할 수 있다. |
| public staging 중 FLOW/WEIGHT 진행 | 타당. `test_dealer_router_single_lane_contract.cpp:2367` 이후 FINAL 전에 wire control을 읽으므로 D3를 직접 관찰한다. physical prefix 보호는 기존 pipe test의 책임으로 남는다. |
| REPLY 두 번째 token | **부당. B01.** 실제 입력은 다른 token이 아니라 같은 token이다. registry 단일 소유권은 EBUSY→ENOENT 변경 근거가 아니다. |
| REPLY OOM failpoint의 첫 MORE 이동 | failpoint 위치 이동은 타당하지만 checkout 이후 실패 검증을 대체하지 못한다. W03. |
| helper 내부 state assertion | 타당. `unittest_single_lane_accounting.cpp:552`, `unittest_zmp_contract_edges.cpp:787`은 caller slot 소유 구조에 맞춘 내부 검사다. |

`test_helper_ownership.cpp:388`의 invalid flag 값→유효하지만 sequence와 다른 DONTWAIT 변경은
같은 caller의 flags 불변 조건을 직접 검사한다. 다만 이 변경만으로 기존 unknown flag 경계의
검증까지 유지됐다고 볼 수는 없다.

## 채택 조건과 검증 한계

차단 항목은 현재 source 경로와 main 계약을 직접 대조하여 기록했다. 실행에 의한 재현 확인은
수행하지 않았다. 수정 후에는 B01의 공개 오류 계약, B02/B06의 callback·close 구간을 고정한 재현,
B03/B04의 실제 할당 실패, B05의 logical revoke 후 capacity 반환을 먼저 검증해야 한다.
이 리뷰는 새 gate나 측정을 실행하지 않았으므로 구현 보고서의 400회 통과·TSan 잔여 실패·5셀
PASS를 독립 검증한 것으로 해석하면 안 된다.

수정 파일은 이 보고서와 `progress-review-mp2.md`뿐이다. 소스·스펙·테스트 변경 및 커밋은 없다.

차단 항목 수 6 / 채택 가능 여부: 불가
