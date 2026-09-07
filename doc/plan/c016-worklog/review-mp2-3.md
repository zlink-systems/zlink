# review-MP-2-3 독립 리뷰

독자: MP-3~8 누적 구현의 차단 해소와 채택 여부를 결정하는 감독자.

**누적 diff는 현재 채택 불가다.** B201~B206의 2차 재현 경로와 1차 B02·B05·B06의 송신 쪽
잔여는 코드상 해소됐다. 그러나 MP-8이 무등록 `NONE` completion pull까지 확대한 drain에서
zero-copy callback이 physical sync를 다시 획득하는 정지 경로가 있다(B301). Req/rep 명령 수
감소는 함수 수준의 경로 변경까지 확인했지만, 감소량 전부의 실측 귀속은 미확정이다(W302).

## 범위와 증거

- 구현: `/home/hep7hep7/project/zlink-work/mp2`, detached HEAD
  `3a7db427bce26bb155cdc7c5f45b3f52edb5f2c3`의 미커밋 diff. Tracked 27개 파일
  +3313/−611 및 untracked
  `core/tests/integration/test_writable_resubmit_from_other_thread_while_sequence_open.cpp`를 포함한다.
- 계약: `/home/hep7hep7/project/zlink` main 작업 트리의 미커밋 `core/doc/spec`.
  README completion pull 명료화 문장까지 포함했다. 스펙을 수정하지 않았다.
- 입력: `_common-rules.md`, `review-mp2.md`, `review-mp2-2.md`,
  `core-rf-MP-8-report.md`, `decisions.ko.md:2076`의 D-B211. MP-1 §4.3과 설계 원칙도 대조했다.
- MP-8만의 변경은 보존된 `mp7-before-mp8.patch`를 HEAD 원문에 **메모리에서** 적용해 현재
  원문과 비교했다. 파일을 checkout하거나 소스를 고치지 않았다. Patch SHA-256은 보고서와 같은
  `96f4c07e0e38811a1e440649f7f7c9148f54fad62e632019cbb0c92cefa74221`이다.
  MP-8 production 변경은 보고서에 적힌 8개 source 파일과 일치한다.
- 아래 `core/src`·`core/tests`는 **mp2**, `core/doc/spec`·`doc/plan`은 **main** 기준이다.
  `README`는 `core/doc/spec/core/socket/README.ko.md`, `05-polling`은
  `core/doc/spec/core/05-polling.ko.md`, Message는 `core/doc/spec/core/02-message.ko.md`,
  ROUTER는 `core/doc/spec/core/socket/07-router.ko.md`, ZMP는
  `core/doc/spec/core/protocol/01-zmp.ko.md`다. MP-1은
  `doc/plan/c016-worklog/core-rf-MP-1-design.md`다. 같은 이름의 source 파일은 해당 mp2 경로를 뜻한다.
- B3xx는 채택 차단, W3xx는 검증 한계·비차단 관찰, S3xx는 정리 제안이다.
  모든 실행 순서는 정적 추적 결과다. **빌드·제품 실행·테스트·benchmark·sanitizer 실행은 0회**다.
  MP-8의 209/209·sanitizer PASS는 구현자 보고이며 이번 리뷰의 재검증 수치가 아니다.

## 항목별 판정

### 2차 차단 항목

| ID | 판정 | 현재 코드로 다시 따른 시나리오와 근거 |
|---|---|---|
| B201 | **해소** | X/T를 checkout한 뒤 X를 두 번 제거한다. `socket_request_reply_runtime_io.cpp:853`~`:868`은 첫 live→revoked에서만 checkout·slot을 감소시키고 두 번째는 `:854`에서 건너뛴다. 늦은 restore `:727`~`:730`, commit `:771`~`:774`는 tombstone만 지운다. 같은 RID의 새 live token은 별도 entry라 새 전이를 한 번 계산한다. 테스트는 `unittest_phase3_request_reply_owners.cpp:891`~`:1024`; replacement는 실제 재연결 대신 registry를 구성한 unit 시나리오다. |
| B202 | **해소** | DONTWAIT SEND FINAL은 `socket_message_send_api.cpp:400`에서 admission을 얻고 `:447`~`:453`의 append 실패·unlock·abort·현재 part 소비가 끝날 때까지 유지한다. REQUEST는 `socket_request_reply_submit_api.cpp:1044`에서 사전 조회 전에 얻은 admission을 `:1130`으로 넘기며 `:896`~`:905`의 staging 실패에도 놓지 않는다. 따라서 close가 raw sequence를 먼저 파괴하던 순서가 막힌다. Admission 이후의 분기는 다음 절에서 구분한다. 테스트 `unittest_phase3_request_reply_owners.cpp:770`~`:813`은 REQUEST OOM callback 중 다른 thread close의 BUSY를 검사한다. |
| B203 | **해소** | REPLY MORE 뒤 publish invalid flags의 경로는 여전히 `socket_message_send_api.cpp:777`에서 시작하지만, 공용 abort의 local handle이 `part_helper_api.cpp:1047`~`:1078`까지 유지된다. `:1076`에서 payload와 context를 모두 닫은 뒤 pin을 놓는다. Callback 중 close가 accepted되어도 socket 파괴는 context restore 뒤다. 테스트 `unittest_phase3_request_reply_owners.cpp:815`~`:889`. |
| B204 | **해소 — 기존 lost wake·budget 경로** | `socket_base_lifecycle.cpp:618`의 epoch 관찰→`:621` drain→`:627` queue 재확인→`:647`~`:652` 동일 epoch 대기가 연결된다. 다른 command owner가 중간에 command를 소비해도 mailbox `send/signal`이 관찰 epoch를 바꾼다(`runtime/core/mailbox.cpp:64,101,259`). Registration 수에 따른 `return 0`과 두 번째 queue budget은 없어졌다. Monitor 대기도 같은 epoch를 쓴다. 새 drain 범위의 별도 결함은 B301, 테스트 판별력은 W301이다. |
| B205 | **해소 — 지적된 송신 해제 경로** | REPLY attempt는 `socket_request_reply_runtime_io.cpp:1408`~`:1424`에서 shallow copy를 만들고 rollback은 `:1483`에서 physical/transport scope 안에 완료한다. 원본은 `socket_request_reply_submit_api.cpp:697`~`:706`에서 complete scope 해제 후 닫는다. Blocking REQUEST fast success도 `socket_send_submit.cpp:645`·`:699`에서 local scope를 reset한 뒤 consume한다. Slow success는 `:814` unlock 뒤 `:817` consume이다. 테스트 `unittest_phase3_request_reply_owners.cpp:1027`~`:1164`. |
| B206 | **해소** | 다른 caller가 Y/T(X≠Y)를 제출하면 `socket_request_reply_runtime_io.cpp:695`에서 RID/capability 불일치를 먼저 판정해 ENOENT로 간다. 같은 X/T 중복만 `:701`의 busy→`socket_request_reply_submit_api.cpp:313`의 EBUSY다. 자기 sequence의 RID/token 변경은 helper spec mismatch EINVAL이다. 세 입력과 original 유지·재시도를 `test_phase3_request_reply_contract.cpp:1651`~`:1728`이 구분한다. |

### 1차 부분 해소 항목

| ID | 판정 | 잔여 확인 |
|---|---|---|
| B02 | **해소 — 기존 송신 시나리오** | 만료 node는 `part_helper_api.cpp:817`~`:823`에서 helper unlock 뒤 정리한다. SEND/REQUEST FINAL의 physical scope는 그 뒤이며 B202 수명 공백과 B205 송신 원본 해제도 해소됐다. 단, 이를 **모든 completion drain의 payload 해제까지 안전하다**는 주장으로 확대할 수 없다(B301). DONTWAIT SEND의 중첩 admission 비용은 S301이다. |
| B05 | **해소** | 논리 RID revoke의 즉시 capacity 반환, 반복 revoke, 늦은 restore/commit의 재차감 방지가 모두 같은 registry entry에서 완결된다(`socket_request_reply_runtime_io.cpp:727,771,853`). |
| B06 | **해소** | 원래 validation entry의 pin에 더해, 빠져 있던 publish invalid-flags 경로도 공용 abort 내부 pin이 context 해제까지 보호한다(`part_helper_api.cpp:1047,1076`). |

### 2차 비차단·정리 항목

| ID | 판정 | 근거와 남은 범위 |
|---|---|---|
| W201 | **해소 — 실패 record의 thread 이전** | 최초 MORE/실패 FINAL은 `test_writable_resubmit_from_other_thread_while_sequence_open.cpp:569`의 worker, 재제출은 `:729`의 별도 worker다. A의 열린 sequence는 `:769`까지 유지한다. 최초 worker가 계속 존재하며 다른 새 record를 여는 추가 변형까지 관찰하는 것은 아니다. |
| W202 | **해소** | 같은 파일 `:495`~`:501`에서 공개 PAUSED event를 확인하고, 그 상태에서 filler와 B를 거절시킨 뒤 `:590`에서 RUNNING으로 전환한다. `:534`의 즉시 poll만으로 포화를 추론하는 구조가 아니다. 20 ms quiet window는 없다. |
| W203 | **부분** | `test_helper_ownership.cpp:58`~`:66`은 실제 physical sync를 잡는 SNDHWM setter로 바뀌었고 fresh worker를 사용한다. 그러나 만료 회수는 새 worker의 MORE(`:581`)에서 이미 일어나므로 FINAL 해제 경계는 검증하지 않는다. Helper mutex 재진입도 setter 자체는 검사하지 않는다(W303). |
| W204 | **해소 — 지적된 초기화 실패** | `unittest_complete_record_admission.cpp:73`~`:80`에서 init 실패도 `more_done`을 발행하고 `:120` join 뒤 실패를 검사한다. MORE/FINAL handle close도 `:87,104`에 있다. 다른 fixture의 Unity assertion/예외 전체 종료를 보장한다는 뜻은 아니다. |
| W205 | **해소 — 비용·회수 범위 명시** | 2-part SEND helper lock 2회, REQUEST/REPLY 3회와 O(k) scan은 현재 코드에 맞는다(`part_helper_api.cpp:95,804`~`:825`; REQUEST submit `:1058`; REPLY submit `:510`). Identity 없는 cold FINAL은 만료 회수도 생략하며 다음 identity 보유 접근/close가 정리한다. 별도 warm/혼합 성능 개선을 입증한 판정은 아니다. |
| W206 | **해소 — 코드상 지속 busy loop** | `socket_base_lifecycle.cpp:647`~`:649`은 async owner가 있을 때 `process_commands()`의 즉시 반환 대신 mailbox CV 대기를 택한다. Owner 전환으로 한 번 재확인하는 것과 기존 timeout 전체 spin을 구별한다. 테스트는 CPU/대기 진입을 확인하지 않는다(W301). |
| S201 | **해소 — 두 abort의 정리 중복** | `part_helper_api.cpp:123`의 공용 해제 함수가 buffer→context 순서를 소유하고 `:1039,1076`이 재사용한다. 공용 validation abort의 pin도 그 호출을 감싼다. Raw sequence를 받는 `abort_send_step`의 수명 전제는 caller admission이며 B202에서 확인했다. |
| S202 | **해소** | `socket_base.hpp:581`의 반환 설명은 ready head/실패로 맞춰졌고 prepare의 timeout-zero 분기는 제거됐다. `unittest_phase3_request_reply_owners.cpp:1867`도 physical drain 부재를 DONTWAIT로 한정한다. |

## FINAL 수명과 physical 해제 상세

### B202 admission 경계

| 분기 | Admission과 해제 순서 |
|---|---|
| entry validation 또는 admission 획득 실패 | Admission은 획득되지 않을 수 있다. 공용 abort가 pin을 유지하고 helper mutex 안에서 현재 slot을 찾아 분리한다. 이미 close가 slot을 정리했으면 찾지 못하고 끝나므로 unlocked raw sequence를 사용하지 않는다(`part_helper_api.cpp:1047`~`:1076`). |
| SEND/SEND_RID FINAL prepare·append 실패 | `socket_message_send_api.cpp:400`의 staging admission이 `:424`·`:447` 실패 cleanup과 callback을 끝까지 감싼다. Helper는 먼저 unlock한다. Physical scope는 아직 없다. |
| REQUEST FINAL 사전 group·reservation·lookup·metadata·append 실패 | `socket_request_reply_submit_api.cpp:1044`의 admission이 사전 검사(`:1075,1088,1103`)와 `submit_buffered_request_step`의 `:838,881,896` 실패까지 이어진다. `complete_scope.reset()`은 이 시점에는 빈 optional 정리다. Raw sequence는 admission과 helper mutex로 보호된다. |
| record 분리 실패 | SEND `:458`·REQUEST `:912`의 오류에서도 staging admission은 남는다. 실제 정상 caller의 valid slot에서는 take/erase가 무할당이며 실패 전제는 invariant 위반이다(`part_helper_api.cpp:963`~`:989`). 이 분기를 OOM 처리라고 설명하면 부정확하다. |
| REQUEST 분리 성공→physical 진입 실패 | Helper lock에서 record를 옮기고 slot을 지운 뒤 `:925`에 staging admission을 놓는다. 그 뒤 close가 이겨도 sequence는 이미 없고 local record가 원본을 소유한다. Scope 실패 시 `:930`에서 닫으며 public handle pin이 socket을 보호한다. |
| SEND 분리 성공→physical attempt/실패 | `socket_message_send_api.cpp:467`에서 helper를 놓는다. Complete scope 실패 `:469` 또는 attempt 실패 `:475` 뒤 원본을 닫는다. Complete scope는 `:478`에서 먼저 해제한다. **이 분기는 staging admission도 계속 보유한다**(S301). |
| REQUEST DONTWAIT physical 실패 | `:948`에서 complete scope를 해제한 뒤 `:952`에서 원본을 닫고 기존 pending/wait-token 실패 처리를 수행한다. Raw sequence 접근은 없다. |
| REPLY FINAL | Active 여부의 선행 조회는 mutex 아래서 bool만 반환한다. Active이면 `:508`에서 staging admission을 얻고 실패 정리를 보호한다. `:670`에서 record를 분리하고 `:680`에서 admission을 놓는다. 이후 context·record는 local owner와 public handle pin이 보호한다. |

Admission은 lifecycle 진입 허가이고 physical sync는 배타 잠금이다. Callback 동안 admission이
남아 close가 BUSY인 것과 callback이 physical sync를 재획득하다 정지하는 것은 다르다.
위 표의 validation·close 경합에서 admission을 무조건 유지한다고 주장하지 않는다. 획득이
실패한 경로에는 pin+mutex로 분리한 소유권이 대신 필요하며 현재 공용 abort가 그 조건을 충족한다.

### B205 복사·rollback·소비

**REPLY shallow copy 생성 자체는 physical scope 안이다.** Caller는 complete scope를 잡은 채
`send_public_router_reply_with_wait()`→`send_completion_staged_frames_on_pipe()`를 호출한다
(`socket_request_reply_submit_api.cpp:541,561,682,690`). 다만 **마지막 원본 payload 해제**는
physical scope 밖이다. 이를 구분해야 한다.

- Attempt 생성은 빈 handle을 init한 뒤 원본을 shallow-copy한다
  (`socket_request_reply_runtime_io.cpp:1408`~`:1424`). 할당/복사 실패는 만든 attempt만 닫고
  ENOMEM으로 반환한다(`:1426`~`:1432`). 원본은 계속 caller에 있다.
- Multipart는 generation gate를 전체 write/rollback 동안 유지한다(`:1438`). 중간 실패의
  `:1483` rollback은 pipe `_out_sync` 아래 prefix를 지우고 provisional accounting을 되돌린다
  (`runtime/core/pipe.cpp:2628,3878,3884`). 여기서 닫는 payload에는 원본 참조가 남으므로 user
  free callback의 마지막 참조가 아니다. Single-part의 connection 검증과 write/flush도
  `pipe.cpp:2395`~`:2412`의 같은 `_out_sync` 아래다. 이 single-part 최적화는 MP-7에도 있었다.
- 성공·실패 원본 consume은 REPLY single `socket_request_reply_submit_api.cpp:567,573` 뒤,
  multipart `:697,704` 뒤다. Wrapper `socket_request_reply_runtime_io.cpp:1547`~`:1555`도
  변경된 callee의 미소비 계약에 맞춰 소비를 추가했다. Production caller는 public reply 경로다.
- REQUEST fast success는 local scope reset→original consume, scoped DONTWAIT는
  `consume_multipart_on_success_=false`→상위 scope reset→consume이다
  (`socket_send_submit.cpp:594,638`~`:646,695`~`:700`). Slow success는 sync unlock 뒤 consume한다
  (`:814`~`:817`). Single REQUEST는 기존 direct move이며 consume을 새로 생략한 것이 아니다.

## B204 관찰·대기·종료 분기 전수 대조

아래 행 번호는 `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`다.
관찰을 시작한 모든 정상 반환·오류 반환 분기는 관찰을 끝낸다.

| 분기 | 연결과 판정 |
|---|---|
| 진입 시 queue ready | `:611`에서 1을 반환한다. 아직 관찰을 시작하지 않았으므로 종료할 observer도 없다. |
| drain 오류 | `:618` 관찰→`:621` drain 실패→`:623` 관찰 종료→원래 errno로 반환한다. 실패 후 대기를 시작하지 않는다. |
| drain 중 또는 직후 publish | `:627` ready 재확인→`:628` 관찰 종료→1 반환이다. Poller/async owner가 대신 게시해도 public queue에서 확인한다. |
| 남은 budget 없음 | `:637`에서 같은 entry budget을 검사하고 `:638` 관찰 종료→EAGAIN이다. 원래 timeout으로 queue wait를 다시 시작하지 않는다. |
| async owner·monitor 있음 | `:647`~`:649`에서 관찰했던 epoch를 그대로 mailbox wait에 전달한다. Wait mutex 아래 epoch·pending hint를 재확인하므로 마지막 ready 검사 뒤 소비된 command도 놓치지 않는다(`mailbox.cpp:259`~`:288`). |
| 직접 command 처리 가능 | `:650`~`:652`에서 동일 epoch를 `process_commands()`로 전달한다. 도중 async owner가 생겨 즉시 반환해도 다음 loop에서 owner를 다시 확인하며 새 budget을 만들지 않는다(`:402,430`). |
| wait 성공 또는 EAGAIN | `:654`에서 관찰을 종료하고 같은 `wait_budget`을 가진 loop로 돌아간다. Queue ready를 먼저 보고, 비어 있으면 새 관찰→drain→남은 budget 확인 순서다. |
| wait의 다른 오류 | `:654` 관찰 종료 후 `:655`~`:657`에서 오류를 반환한다. ETERM 등 종료 errno를 EAGAIN으로 덮지 않는다. |
| registration 1→0 또는 0→1 | 위 loop에는 refs별 반환·budget 생성 분기가 없다. Remove 후 기존 owner가 있으면 실제 command로 깨운다(`socket_base_api.cpp:936`~`:950`). Drain은 같은 owner gate로 직렬화된다. 전환 전 소비한 시간은 `:608`의 budget에 남는다. |

Public entry는 `socket_message_handler_api.cpp:137`에서 RCVTIMEO를 읽으며 NONE이고 0이 아닐 때만
이 준비 함수를 호출한다(`:140`). 준비가 ready를 반환하면 queue는 DONTWAIT로 소비한다(`:149`).
따라서 0은 즉시 queue 조회, 음수는 무한 budget, 양수는 동일한 유한 budget이다. Close handoff는
`socket_base_control.cpp:38`~`:42`에서 mailbox까지 깨우며 context stop도
`socket_base.cpp:354`~`:356`에서 signal한다. 다만 **drain 내부 callback이 돌아오지 않는 B301은
이 정상 대기·종료 분기까지 도달하지 못한다.**

## 신규 차단 항목

### B301 — 무등록 NONE completion drain의 zero-copy callback 재진입 정지

**MP-8은 no-poller `NONE`에서 늦은 reply를 폐기할 때 free callback을 physical sync 안에서
실행할 수 있다.** Callback이 같은 requester socket의 option을 설정하면 무한 대기한다.

근거와 가능한 공개 호출 순서는 다음과 같다.

1. Inproc DEALER→ROUTER REQUEST의 timeout completion을 먼저 receive·close한다. Responder의
   reply token은 requester timeout으로 무효화되지 않으므로 늦은 REPLY 제출은 허용된다
   (README `:1140`~`:1142`). Responder가 zero-copy REPLY를 제출하며 free callback은 같은
   requester DEALER에 `zlink_set_option(..., ZLINK_OPT_RCVTIMEO, ...)`을 호출하도록 한다.
   Socket과 callback hint의 수명은 유지하고 별도 payload 복사본은 두지 않는다.
2. Responder FINAL 반환 후 original ref는 이미 소비됐다. Async completion owner가 늦은
   reply를 drain하기 전에 유일한 queue consumer가 `zlink_completion_recv(NONE)`에 들어가
   direct drain을 먼저 얻는 순서를 택한다. Public completion queue는 비어 있다.
3. MP-8 `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:621`은 poller가 없어도
   `get_events()`를 호출한다. `socket_base_api.cpp:783`의 physical API sync와 `:788`의
   completion owner gate를 잡은 채 `:792`→`:1360`→`:1544`로 transport reply를 읽는다.
4. Pending은 timeout resolver가 이미 제거했다. `socket_request_reply_pending_api.cpp:100`~`:103`
   조회가 실패하고, `socket_request_reply_dispatch.cpp:153`~`:159`는 **registry mutex까지
   보유한 채** `zlink_multipart_close()`를 호출한다. `runtime/core/msg.cpp:425`~`:431`은
   마지막 payload ref에서 user callback을 즉시 호출한다.
5. Callback의 setter는 `socket_base_api.cpp:674`에서 같은 physical sync를 다시 획득한다.
   `socket_runtime.hpp:872`~`:877`은 기본 `lock_=true`이며 같은 thread 예외가 없다.
   `socket_lifecycle_runtime.cpp:391`~`:409`는 자신이 보유한 sync bit를 기다린다.
   Timeout 확인 `socket_base_lifecycle.cpp:637`까지 복귀하지 못한다.

이는 두 completion consumer를 사용하거나, 닫힌 socket에 접근하거나, REPLY token을 조작한
시나리오가 아니다. 정적 코드로 도출했으며 실제 재현을 실행하지 않았다.

MP-7의 무등록 분기는 `ensure_completion_processing()` 후 `return 0`으로 queue wait에 위임했다.
Async drain은 `socket_base_lifecycle.cpp:1502`의 command sync를 놓은 뒤 `:1507`에서 owner gate를
잡으므로 이 **direct pull의 physical sync 보유**는 없었다. `get_events()` 자체의 폐기 조건은
기존 poller 경로에도 존재했다. 따라서 원래 helper를 MP-8이 새로 작성했다고 주장하지 않는다.
**기존 잠금 전제가 다른 helper를 무등록 NONE 경로로 확대한 결함**이다. Payload export OOM의
폐기(`socket_request_reply_dispatch.cpp:115,166`)도 같은 physical scope에 도달한다.

소유 계층은 Core completion drain·폐기다. 근거 계약은 README `:1205`~`:1208`의 직렬 drain과
RCVTIMEO 진행, Message `:215`~`:222`의 zero-copy 수명이다. MP-1 `:145`의 잠금 밖 payload
해제 원칙과도 일치해야 한다. 변경 분류는 **B — 기존 결함 노출/호출 범위 확대**다.

채택 전에는 기존 drain 소유자가 폐기 payload를 registry·physical 잠금 밖에서 해제하도록
수명 경계를 완결해야 한다. Callback 금지, recursive lock, timeout 증가, 무등록 async-only
분기 복구로 보상하면 안 된다. 늦은 reply 또는 payload-export OOM에서 마지막 zero-copy ref와
같은 requester option 재진입을 고정한 검증이 필요하다.

## Req/rep 감소 원인

| 측정값 | MP-7 | MP-8 | 차이 |
|---|---:|---:|---:|
| `dealer_router_reqrep_inproc` Ir/msg | 18,437.3152 | 16,241.043 | −2,196.2722 (−11.912%) |

### 함수 단위 변경과 필요한 작업

이 셀은 single REQUEST→ROUTER receive→single REPLY→`completion_recv(NONE)`을 같은 caller의
루프에서 수행하고 poller를 만들지 않는다(`core/tests/perf/hotpath_bench.cpp:603`~`:643,674`~`:725`).
MP-8 source 차이를 그 경로에 대입하면 **줄어드는 제어 흐름의 변경은
`socket_base_t::prepare_completion_pull()`의 무등록 async/queue wait 위임 제거**로 특정된다.

| 함수·경로 | MP-8에서 달라진 작업 | 판정 |
|---|---|---|
| `prepare_completion_pull()` (`socket_base_lifecycle.cpp:606`~`:659`) | MP-7의 refs=0→`ensure_completion_processing()`→`return 0`을 제거했다. 지금은 관찰→기존 `get_events()` drain→ready 확인으로 곧바로 queue dequeue에 도달할 수 있다. | 정상 경로에서 제거된 대기 위임의 소유 함수다. 전체 감소량의 귀속은 W302다. |
| `zlink_completion_recv()` (`socket_message_handler_api.cpp:140`~`:154`) 및 `socket_completion::recv()` (`socket_completion_queue_internal.cpp:453`~`:466`) | 위 준비 결과가 1이면 public queue CV의 blocking wait를 건너뛰고 DONTWAIT dequeue한다. Ready가 이미 있으면 MP-7도 위임 없이 끝났으므로 모든 record가 고정 비용을 같은 만큼 줄이는 변화는 아니다. | **필요 없는 scheduling/wait 왕복을 줄일 수 있는 변경**이다. Async executor 자체를 삭제한 것은 아니다. |
| `ensure_socket_pull_pending_request()` (`socket_request_reply_pending_api.cpp:311`~`:321`) | REQUEST는 여전히 completion owner 확보와 reservation을 수행한다. | **owner·completion slot 회계 생략 아님.** |
| `process_ready_completion_pipes()`→`process_completion_pipe()`→`complete_reply_from_transport()` (`socket_base_api.cpp:1321,1544`; dispatch `:127`) | 동일한 pipe read, wire/pair/correlation 확인, pending 제거와 payload export/publish를 호출한다. | **drain·검사 생략 아님.** Publish는 `socket_request_reply_internal.cpp:455`~`:466`, dequeue의 outstanding 제거·reservation 반환은 queue `:487`~`:493`에 남는다. |
| `send_completion_staged_frames_on_pipe()` (`socket_request_reply_runtime_io.cpp:1408`~`:1424`) 및 `public_router_reply_submit()` (`submit_api.cpp:573`~`:575`) | REPLY shallow-copy와 scope 밖 원본 consume을 **추가**했다. | Copy 제거에 의한 감소가 아니다. 소비 책임은 호출자로 이동했으며 유실되지 않았다. |
| `try_request_admission_submit_fast()` (`socket_send_submit.cpp:638`~`:646`) | Multipart original close의 위치가 바뀌었다. | 이 셀은 single-part라 해당 조건을 실행하지 않는다. |
| Token revoke, abort pin, helper staging admission | 정상 warm single request/reply에는 revoke·abort·열린 send sequence가 없다. | 감소의 직접 원인으로 삼을 근거가 없다. |

위 첫 행의 변경은 **정당한 대기 왕복 제거의 후보**이며, 정상 왕복에서 payload consume·token commit·
correlation·slot 회계를 생략한 diff는 찾지 못했다. `get_events()`가 API lock과 drain gate를
계속 취하므로 physical scope 제거를 감소 원인으로 설명해서도 안 된다. 다만 정상 경로의
작업 보존과 B301의 오류/폐기 경로 안전성은 별개다.

### W302 — 감소량의 실측 귀속과 gate 판정

**−2,196.2722 Ir/msg 전부가 위 함수 변경의 효과라는 계량적 증거는 없다.** MP-7 기존 log는
총 명령 수 `92,186,576 / 5,000`만 남겼고 MP-8 보고서도 셀 합계만 제공한다. Gate는
`core/tests/perf/hotpath_gate.py:128`의 `TemporaryDirectory` 안에 callgrind 원문을 만들고
`:149`~`:150`에서 합계만 반환한 뒤 지운다. 지정 scratch 및 `/tmp`에서 두 시점의 req/rep
함수별 profile은 확보하지 못했다. 새 측정은 실행하지 않았다.

또한 collection은 전체 왕복 루프를 감싸고(`hotpath_bench.cpp:715`), completion·message의 최종
close는 loop 밖이다(`:727`~`:731`). Async task 중 일부가 collection 종료 뒤에 실행되는 경우까지
합계만으로 분리할 수 없다. 그러므로 이 결과를 처리량 개선이나 전체 lifecycle 비용 감소로
확대하지 않는다. Bench는 ID/result/part count를 검사하지만 payload 전체와 callback count까지
검증하는 도구도 아니다(`:636`~`:639`).

판정: **함수 수준의 원인 범위 특정은 해소, −11.9% 전량 귀속은 부분**이다. 원본 before/after의
함수별 self/inclusive Ir와 호출 수가 확보되어 async post/dispatch·queue wait 감소, 새 direct drain·
copy 증가, collection 밖 작업을 분리하기 전에는 **공식 양방향 gate FAIL을 유지**한다.
Reference 변경 승인을 권고하지 않는다. 성능 evidence 부족을 runtime 작업 누락이 입증된
차단 항목으로 바꿔 세지는 않는다.

## 신규 비차단 항목과 정리 제안

MP-8에서 추가·이동한 코드의 경계를 다음과 같이 대조했다. 기존 누적 diff의 문제와 새로 확대한
호출 경로의 문제를 구분했으며, 정적 검토에서 발견하지 못한 결함의 부재를 보증하지 않는다.

| 점검 대상 | 결과 |
|---|---|
| 중복 상태·소유자 | Token의 별도 revoked 표나 completion queue를 추가하지 않았다. 기존 entry·mailbox epoch를 사용한다. DONTWAIT SEND의 중첩 admission은 S301에 남는다. |
| 잠금 순서 | 수정된 송신 오류 경로는 helper unlock→physical 진입 순서다. 새 무등록 completion 경로의 physical→owner→registry→callback→physical 순환은 B301이다. |
| 예외 경계 | 새 REPLY attempt 준비는 `socket_request_reply_runtime_io.cpp:1409`~`:1432`의 catch에서 생성된 attempt와 pipe ref를 정리하고 ENOMEM을 반환한다. Original은 caller가 유지한다. `msg.cpp:486`~`:518`의 copy 대상은 init한 빈 handle이라 copy 자체가 기존 user payload를 마지막 해제하지 않는다. Catch가 넓지만 해당 블록은 reserve·init·copy이며 새로운 user callback/transport 호출을 포함하지 않는다. 이 범위에서 새 예외 누출·원본 유실은 찾지 못했다. |
| 죽은 코드·주석 | Buffered REQUEST MORE 분기는 기존부터 남아 있던 S302다. SEND scope 해제 설명은 현재 코드와 어긋난 S301, completion 반환·DONTWAIT 설명 수정은 S202다. |

### W301 — B204/W206 테스트의 판별력

- Lost-wake 테스트는 `unittest_phase3_request_reply_owners.cpp:1174`에서 RCVTIMEO=1000을 쓰고
  `:1216`~`:1223`에서 결과만 검사한다. Epoch 전달이 빠져도 mailbox timeout 후 다음 drain에서
  성공하는 회귀를 구분하지 못한다. Poller가 미리 게시한 변형(`:1275`~`:1287`)도 같다.
  테스트 hook으로 문제의 순서는 고정했지만, 충분히 이른 반환 또는 무한 대기에서 독립 watchdog을
  통한 실패 검출은 없다. 실행 PASS가 lost wake 부재의 충분한 증거는 아니다.
- Registration 테스트는 hook에서 consumer를 멈춘 채 remove와 add를 **모두** 완료한 다음
  재개한다(`:1322`~`:1333`). Consumer가 refs=0인 구간에 진행하거나 그 상태로 mailbox/queue
  대기를 전환하는 실행은 관찰하지 않는다. 한 deadline을 유지하는 현재 코드는 확인했지만
  1→0과 0→1 각각의 대기 전환 회귀를 고정한 테스트라는 보고는 과하다.
- Monitor 검증은 wall time 150~300 ms만 검사한다(`:1339`~`:1340`). 같은 시간 동안 CPU를 계속
  쓰는 구현도 통과할 수 있다. 기존 mailbox waiter count 또는 대기 hook으로 실제 잠든 구간을
  확인해야 W206 회귀를 직접 검출한다. Production wait의 수정 자체를 미해소로 판정하지는 않는다.

### W303 — W203 회귀 테스트의 FINAL 경계 부재

`test_helper_ownership.cpp:563`의 abandoned worker를 join한 뒤, fresh worker가 `:581`에서 MORE를
보낸다. `prepare_send_step_locked()`는 그 첫 MORE에서 identity를 생성하고 만료 scan을 수행하므로
callback은 FINAL `:587` 전에 실행된다. 따라서 FINAL이 physical scope를 너무 일찍 얻는 원래
B02 회귀는 이 테스트만으로 잡히지 않는다. Setter는 helper mutex를 얻지 않아 helper lock 부재의
직접 검증도 아니다. B02의 현재 수정은 코드로 확인했으며, 테스트는 **fresh TLS+MORE 만료 회수**의
검증으로 범위를 적어야 한다. FINAL 경계를 검사하려면 B가 자기 MORE를 먼저 연 뒤 A 종료를
고정하고 B FINAL이 만료 회수를 수행하도록 해야 한다.

### S301 — DONTWAIT SEND의 중첩 admission과 주석·보고서 불일치

`socket_message_send_api.cpp:400`의 staging admission은 DONTWAIT에서 `:469`의 complete scope와
겹치며, `:487`의 payload close까지 남는다. 두 scope는 모두 lifecycle inflight를 소유한다
(`socket_runtime.hpp:851`; `socket_lifecycle_runtime.cpp:443`). 이는 B202 수명 수정에는 안전하지만
원래 B02가 피하라고 한 중첩 RMW 비용을 남긴다. `:484`~`:486`의 “all helper/socket scopes”가
해제됐다는 주석 및 MP-8 보고서의 “physical submit 전 staging admission 해제” 설명은 SEND에는
맞지 않는다. **Locks는 해제됐고 staging admission은 남는다**고 구분해야 한다. 정리가 필요하면
분리 전 보호를 없애지 말고 기존 admission의 인계/해제 지점을 일관되게 정해야 한다.

### S302 — Buffered REQUEST의 도달 불가 MORE 분기

`request_part_common()`은 `socket_request_reply_submit_api.cpp:1007`~`:1038`에서 모든 MORE를
처리하고 반환한다. `submit_buffered_request_step()`의 유일한 호출은 그 뒤 `:1130`이다.
따라서 `:852`~`:875`의 MORE 처리와 `:808`의 MORE 설명은 현재 호출 경로에 맞지 않는다.
MP-8에서 새로 만든 분기는 아니지만 이 함수의 admission 인계를 바꾸면서 남은 중복이다.
같은 사실을 다시 구현하는 분기이며 차단 결함으로 세지 않는다.

## 확정 계약·구현·테스트 대조

| 계약 | 최종 판정과 근거 |
|---|---|
| README `:49`~`:59`, `:947`~`:966` part send | **일치.** P/D/R caller별 map, 동일 thread의 family/target/flags 검증, FINAL 분리, 실패 caller만 폐기한다(`part_helper_api.cpp:174,240,780,963,1015`). close는 map swap 뒤 전체 정리(`part_helper_state.cpp:59`~`:74`). SEND/REQUEST DONTWAIT FINAL의 staging 수명은 B202로 확인했다. |
| REPLY token `:1120`~`:1149` | **지적 항목 일치.** Capability 오류 우선순위, 중복 EBUSY, 실패 restore·성공 commit, 반복 revoke 시 slot 1회 반환을 확인했다. `test_phase3_request_reply_contract.cpp:1651`~`:1728` 및 owner unit `:891`~`:1097`이 해당 입력을 검사한다. |
| Completion pull 명료화 `:1203`~`:1219` | **부분.** Poller wait 비소비, 등록 상태와 무관한 NONE 진행, 하나의 physical drain gate, DONTWAIT queue-only, epoch/단일 budget은 일치한다. B301에서 finite NONE가 자체 callback 안에 멈출 수 있으므로 전체 경로 일치로 닫지 않는다. DONTWAIT·finite batch 검사는 owner unit `:1822`~`:1891`에 유지된다. |
| 05-polling `:100`~`:126` | **Owner·비소비 일치.** Registration add/remove는 `socket_base_dispatch.cpp:232,278`, direct/registered/async drain은 `socket_base_api.cpp:788,867`, lifecycle `:1507`의 동일 gate를 쓴다. Public queue dequeue는 그 뒤 별도 호출이다. Registration 전환의 구현 budget은 유지되지만 테스트 범위는 W301이다. |
| ROUTER `:54`, 지정 `:419` 부근 | **일치.** 다른 caller의 family/RID를 독립 처리한다. 현재 실제 physical multipart/weight 문장은 `:420`~`:423`이다. Public MORE만으로 control을 보류하지 않고 물리 prefix부터 commit/rollback까지 경계를 보호한다. 테스트 `test_public_inproc_multipart_send.cpp:1126`, single-lane contract의 `test_sl_controls_progress_during_public_staging`을 대조했다. |
| ZMP control `:241`~`:255`, pending `:474`~`:479` | **일치.** P/D/R helper는 publish 전용 marker/control 경계를 설정하지 않는다(`part_helper_api.cpp:206`~`:222,941`~`:947`). Physical write/rollback과 HWM·provisional 회계는 기존 pipe 함수에 남으며 MP-8은 그 함수를 변경하지 않았다. REQUEST reservation은 FINAL의 기존 owner 경로다(`socket_request_reply_pending_api.cpp:319`). |
| Message `:108` | **일치.** 동시 multipart 범위가 P/D/R로 한정되어 있고 한 record를 여러 thread가 이어받지 않는다. 실패 뒤 새 sequence 재제출과 열린 sequence 계승을 구분한다. MP-6 thread 이전 검증은 W201 판정 범위다. |

## 설계·검증과 채택 조건

새 retry/poller/generation 표를 추가하는 대안보다, 현재 helper·token registry·completion gate가
각 소유권을 유지하며 잠금 밖 해제를 완결하는 대안을 택한다. B301을 별도 async-only 규칙으로
우회하면 B204의 owner 전환·budget 규칙을 다시 늘린다. Epoch 관찰/대기 연결 자체는 유지할 수 있다.

수정 전/후 규칙 수: **이번 리뷰는 코드·규칙 변경 0→0**. 구현 보고서의 “6→4”는 분류 묶음의
개수이며 실제 예외 경로가 모두 제거됐다는 증거로 채택하지 않는다(S301·S302·B301).

소유 계층: Core helper 수명, req/rep token registry, physical submit/rollback, completion drain·폐기.

Spec 근거: main README part send·REPLY·completion pull, 05-polling owner 절, ROUTER·ZMP·Message의
위 표에 인용한 조항. B301 때문에 “어느 문장도 다른 동작이 되지 않았다”는 전체 확인은 하지 않는다.

교차언어: Framework runtime 변경은 없다. 이번 리뷰는 Core C API의 구현 검토이며 언어별
runtime을 수정하거나 실행하지 않았다. 하위 결함을 binding·Framework의 retry/poller로 보상할 사안이 아니다.

변경 분류: B201~B206 수정은 **B — 기존 결함**으로 채택 가능한 방향이다. 신규 B301도 Core의
기존 폐기 조건을 확대한 결함(B)이며 계약 변경(C/D)으로 해결할 이유가 없다.

실행한 검증: 원문·diff·호출 경로·테스트 assertion 대조, 보존 patch hash 확인,
`git diff --check`(출력 없음), 공개 header/ABI 경로 diff 확인(출력 없음). 빌드·테스트 실행은 없다.

남은 실패: **B301 정적 차단 1건**, req/rep 공식 성능 gate FAIL과 W302의 계량적 원인 귀속 한계.
W301/W303은 테스트 보장 범위의 잔여다. 구현자 보고의 PASS를 독립 실행 결과로 바꾸지 않았다.

작성 파일: 이 보고서와 `progress-review-mp2-3.md`뿐이다. 소스·스펙·테스트 수정 및 commit 없음.

차단 항목 수 1 / 채택 가능 여부: 불가
