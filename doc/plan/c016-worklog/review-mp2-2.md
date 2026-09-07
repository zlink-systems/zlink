# review-MP-2-2 독립 리뷰

독자: MP-3~7 누적 구현의 채택 여부를 결정하는 감독자.

**현재 누적 diff는 채택 불가다. 차단 항목은 6건이다.** Caller별 조립, TLS identity와 C3
counter의 기본 구조는 유지할 수 있다. 그러나 DONTWAIT FINAL의 close 경쟁, 반복 RID revoke,
validation abort의 socket pin, physical payload 해제와 MP-7의 대기 연결에 결함이 남는다.
1차 보고서의 B01·B03·B04는 해소됐고 B02·B05·B06은 부분 해소다.

## 범위와 증거의 한계

- 구현: `/home/hep7hep7/project/zlink-work/mp2`, HEAD
  `3a7db427bce26bb155cdc7c5f45b3f52edb5f2c3`의 미커밋 누적 diff.
  Tracked 27개 파일 +2494/−577과 untracked MP-6 테스트 1개를 포함했다.
  `git diff HEAD`만으로는 신규 테스트 본문이 나오지 않으므로 그 파일도 별도로 읽었다.
- 계약: `/home/hep7hep7/project/zlink` **main 작업 트리의 미커밋** `core/doc/spec` 수정까지 포함.
  현재 README의 REPLY 오류·token 수명은 :1127~1149, completion pull은 :1151~1214다.
  요청에서 지정한 이전 행 번호 대신 이번 읽기 시점의 행 번호를 사용한다.
- 공통 규칙, `review-mp2.md`, `core-rf-MP-3/4/5/6/7-report.md`,
  `decisions.ko.md:1957` 이후 D-B203~D-B209와 D-BP15, MP-1 §4.1~4.3을 대조했다.
- 아래 `core/src`·`core/tests`는 **mp2**, `core/doc/spec`·`doc/plan`은 **main** 기준이다.
  B2xx는 채택 차단, W2xx는 검증 한계·비차단 관찰, S2xx는 정리 제안이다.
- 스펙의 `README`는 `core/doc/spec/core/socket/README.ko.md`, `05-polling`은
  `core/doc/spec/core/05-polling.ko.md`, synchronization model은
  `core/doc/spec/core/systems/11-synchronization-model.ko.md`다. `MP-1`은
  `doc/plan/c016-worklog/core-rf-MP-1-design.md`다. 소스·테스트의 축약 파일명은 해당 worktree의
  같은 이름 파일을 가리킨다(`socket_send_submit.cpp`는 `core/src/runtime/sockets/common/`).
- 모든 재현 순서는 **현재 코드로 도출한 실행 순서**다. 빌드·테스트·benchmark·sanitizer를
  실행하지 않았다. 구현 보고서의 PASS 수치와 성능 수치를 독립 검증한 것으로 해석하면 안 된다.
- 수정은 이 보고서와 `progress-review-mp2-2.md`뿐이다. 소스·스펙·테스트 수정, 커밋은 없다.

## 1차 항목별 판정

| 1차 ID | 판정 | 현재 코드에서 다시 따른 시나리오와 근거 |
|---|---|---|
| B01 | **해소** | A가 RID X/token T로 REPLY MORE를 보관하고 B가 **같은 X/T**로 FINAL을 보내면 `socket_request_reply_runtime_io.cpp:692`~`:695`가 busy를 반환하고 `socket_request_reply_submit_api.cpp:312`~`:315`가 EBUSY로 변환한다. B의 part만 소비하며 A의 slot은 건드리지 않는다. 테스트의 EBUSY 기대도 `test_phase3_request_reply_contract.cpp:1658`~`:1666`에 복구됐다. 서로 다른 RID를 잘못 붙인 token의 별도 오류는 B206이다. |
| B02 | **부분** | A의 만료 zero-copy prefix를 B의 DONTWAIT FINAL이 정리하는 원래 순서는 해소됐다. `part_helper_api.cpp:804`~`:810`에서 node만 분리한 뒤 helper mutex 밖에서 닫고, SEND는 `socket_message_send_api.cpp:469`, REQUEST는 `socket_request_reply_submit_api.cpp:922` 이후에 physical scope를 얻는다. 그러나 scope를 뒤로 옮긴 구간의 lifecycle 보호가 빠졌다(B202). REPLY physical 실패와 blocking REQUEST 성공에는 마지막 payload 해제가 physical sync 안에 남는다(B205). 따라서 **모든 해제 경로가 두 잠금 밖**이라는 판정은 불가하다. |
| B03 | **해소** | 다른 caller slot이 있는 socket에서 새 thread의 첫 MORE가 identity allocation에 실패하면 `part_helper_api.cpp:35`~`:46`에서 ENOMEM으로 수렴한다. abort 조회는 `:1047`의 `create=false`라 재할당하지 않는다. 새 thread의 single FINAL도 `:797`~`:802`에서 identity를 생성하지 않고 single로 처리한다. `unittest_phase3_request_reply_owners.cpp:524`의 failpoint 테스트는 현재 part 소비·ID 0·다른 caller prefix 유지를 검사한다. |
| B04 | **해소** | MORE slot 여러 개를 둔 close에서 `part_helper_state.cpp:59`~`:74`가 map 전체를 swap하고 잠금 밖에서 정리한다. 기존 seal 이후 vector reserve가 없어졌다. `clear_part_helper_state()`는 shared owner를 파괴하지 않고 공개 presence만 내린다(`socket_base_request_reply_bridge.cpp:87`). 테스트는 `test_helper_ownership.cpp:693`의 4개 caller/free count다. 실제 allocator 고갈을 실행해 확인한 판정은 아니다. |
| B05 | **부분** | RID X를 **한 번** revoke하면 `socket_request_reply_runtime_io.cpp:852`~`:864`가 checkout·slot을 즉시 반환한다. 늦은 restore(`:726`~`:729`)와 commit(`:770`~`:773`)은 tombstone만 지워 재차감하지 않는다. close도 checked-out tombstone을 남긴다(`:330`~`:337`). 하지만 같은 X를 다시 revoke하면 이미 반환된 counter를 다시 감소시킨다(B201). 신규 테스트 `unittest_phase3_request_reply_owners.cpp:667`~`:694`는 revoke 1회→실패 FINAL/restore만 검사한다. |
| B06 | **부분** | 원래 NULL part/invalid flags의 SEND·SEND_RID·REQUEST·REPLY entry는 각각 `socket_message_send_api.cpp:596,684`, `socket_request_reply_submit_api.cpp:1128,1217`에서 먼저 pin한다. 이 pin이 함수 반환까지 남으므로 detached context보다 먼저 파괴되지 않는다. 그러나 abort helper 자체의 지역 pin은 여전히 `part_helper_api.cpp:1036`~`:1040`에서 끝난다. 이를 외부 pin 없이 호출하는 publish invalid-flags 경로가 남았다(B203). |
| W01 | **해소** | `test_public_inproc_multipart_send.cpp:1045`~`:1069`가 part 수·크기를 무조건 검사하고 caller/sequence/part, 중복 및 전체 `seen` 집합을 확인한다. 크기가 틀린 record가 내용 검사를 건너뛰어 통과하던 경로는 없어졌다. |
| W02 | **해소** | `test_phase3_request_reply_contract.cpp:1825,1873`~`:1883`에서 ID와 context 주소를 저장하고, `:1988`~`:2013`에서 길이·범위를 먼저 검사한 뒤 ID/context/payload를 함께 대조한다. `:2021`~`:2023`이 completion 집합 누락도 검사한다. |
| W03 | **해소** | context allocation failpoint가 checkout 뒤인 `socket_request_reply_submit_api.cpp:599`~`:605`로 옮겨졌다. 실패 시 token restore와 pipe ref 반환이 있다. `unittest_phase3_request_reply_owners.cpp:481`~`:498` 및 `:582`~`:634`가 checkout 직후 OOM과 후속 MORE staging OOM을 나누고 다른 thread의 재제출을 검사한다. |
| W04 | **부분** | heterogeneous `owner_less<void>`(`part_helper_internal.hpp:87`)로 조회의 임시 weak refcount를 없앴고 TLS strong 복사도 없어졌다. SEND의 2-part helper mutex는 2회다. 다만 REQUEST/REPLY FINAL은 사전 조회와 prepare가 각각 남아 있고 만료 scan은 O(k)다. cold caller의 회수 생략과 측정 범위는 W205 참조. |
| W05 | **해소** | SEND·SEND_RID·REQUEST·REPLY MORE가 각각 `socket_message_send_api.cpp:649,746`, `socket_request_reply_submit_api.cpp:1180,1248`에서 ctx termination을 검사한다. 기존 prefix 정리 후 ETERM으로 반환한다. staged REQUEST+blocked FINAL+shutdown 테스트는 `test_helper_ownership.cpp:759`~`:830`에 있다. |
| W06 | **해소 — 확정된 지원 범위 안** | identity shared_ptr의 TLS owner는 정상 API 실행 중 파괴되지 않는다(`part_helper_api.cpp:34,49`). main README `:963`~`:966`은 Core identity의 TLS destructor 이후 part API 호출을 정의하지 않는다고 명시했다. 그 이후 application TLS destructor에서의 호출까지 안전하다고 확대하지 않는다. |
| W07 | **부분** | DONTWAIT MORE/FINAL close stress(`test_public_inproc_multipart_send.cpp:807,850`), 다른 RID(`:1126`), 두 socket(`test_helper_ownership.cpp:605`), 5-part spill(`:657`), 여러 slot close(`:693`), 후속 MORE OOM이 추가됐다. 그러나 1차에서 지적한 `unittest_complete_record_admission.cpp:72`의 초기화 실패→`more_done` 미발행이 **그대로**다. 고정된 DONTWAIT FINAL OOM/close 경계도 없다(B202, W204). |
| W08 | **해소** | main `02-message.ko.md:108`과 대응 영문 diff가 동시 multipart 범위를 PAIR·DEALER·ROUTER로 한정한다. |
| S01 | **부분** | 만료와 close의 node 분리 및 `reset_send_sequence` 재사용은 반영됐다. 다만 abort의 buffer/context 분리·close 루프는 `part_helper_api.cpp:1008`~`:1029`, `:1052`~`:1069`에 중복되고 pin 전제도 호출자에 흩어져 있다. 이 차이가 B203에 직접 연결된다. |
| S02 | **해소** | `socket_send_complete.cpp:358`~`:363`은 complete-record sync와 PUB/XPUB marker를 구분하고 `part_helper_internal.hpp:204`~`:206`도 suspend의 적용 범위를 한정한다. PUB/XPUB가 사용하는 marker/suspend/resume는 죽은 코드가 아니다(`socket_message_send_api.cpp:298,541`). |

## 차단 항목

### B201 — 반복 logical RID revoke의 counter 재차감

- 근거: `core/src/api/socket/socket_request_reply_runtime_io.cpp:848`~`:864`.
  `router_reply_target_matches_rid()`(`:249`~`:254`)는 RID만 비교한다. 이미 `revoked=true`인
  checked-out entry를 건너뛰는 조건이 없으며 tombstone의 `checked_out`은 true로 유지된다.
- 공개 재현 순서: A가 X/T로 REPLY MORE를 보관하고 FINAL 없이 기다린다. B가
  `zlink_disconnect_rid(router, X)`를 두 번 호출한다. 첫 호출은 counter를 1→0으로 만든다.
  두 번째 호출도 `router.hpp:82`~`:86`에서 route 제거 성공 여부를 확인하기 **전에** revoke를
  호출한다. `socket_base_endpoint.cpp:1156`~`:1177`에도 없는 RID를 미리 제외하는 검사는 없다.
  따라서 두 번째 호출은 `reply_target_checkouts > 0` assertion에 도달한다.
  다른 live checkout이 있으면 그 capacity를 잘못 차감한 뒤 늦은 commit/restore에서 정합성이 깨진다.
- 계약: main README `:1141`~`:1149`의 token 무효화·slot 반환. 이미 무효화된 token을 대상으로
  capacity를 다시 반환할 근거가 없다. 해당 tombstone에 대한 늦은 restore/commit의 특수 처리는
  맞지만, **revoke 자체의 반복성**이 닫히지 않았다.
- 수정 방향: registry가 live→revoked 전이를 한 번만 계산하게 한다. 새로운 counter나 별도
  tombstone 표를 추가할 필요는 없다. 반복 disconnect와 같은 RID 재연결 후 재제거, 그 뒤 늦은
  restore/commit을 검사해야 한다.
- 판정: **B, 이번 tombstone/counter 수정의 결함.** 1차 B05 부분 해소의 잔여다.

### B202 — DONTWAIT FINAL staging 실패와 close 사이의 sequence 수명 공백

- 근거: SEND `core/src/api/socket/socket_message_send_api.cpp:400`~`:418`은 NONE에만 staging
  admission을 잡는다. DONTWAIT의 complete admission은 `:469`까지 없다. 그런데 append 실패는
  `:448`~`:451`에서 helper mutex를 놓고 raw `sequence`로 `abort_send_step()`을 호출한다.
  REQUEST도 `socket_request_reply_submit_api.cpp:816`~`:836`, `:892`~`:896`, `:922`가 같은 구조다.
- 재현 순서: A가 DONTWAIT REQUEST MORE를 성공시킨 뒤 FINAL staging allocation을 실패시킨다.
  A가 `state_lock.unlock()`한 직후 B가 close를 완료한다. 이때 A에는 public-handle pin만 있고
  admitted API scope가 없으므로 close가 허용된다. Close는 `zlink.cpp:168` →
  `part_helper_state.cpp:67`~`:74`에서 A의 sequence를 분리·파괴한다. A가 이어서
  `part_helper_api.cpp:1019`에서 `sequence_->buffered_parts`를 읽으면 해제된 객체 접근이다.
  SEND도 5번째 part append의 allocation failure 등으로 같은 순서가 가능하다.
- 수명 구분: `socket_public_handle.cpp:88`~`:105`의 pin은 **socket 최종 파괴**를 늦출 뿐
  helper map cleanup을 막지 않는다. Close의 실행 중 API 판정은
  `socket_lifecycle_runtime.cpp:295`~`:315`의 별도 inflight 값이다.
- 계약·설계: main README `:57`~`:59`, MP-1 `:145`는 staging의 lifecycle admission을 없애는
  최적화를 금지한다. Physical sync 밖 해제를 위해 complete scope를 뒤로 옮긴 결정에
  lifecycle 보호까지 함께 빠졌다.
- 수정 방향: helper 조작·실패 소유권 분리 동안 lifecycle admission을 유지하고 payload 해제는
  physical/helper lock 밖에서 끝낸다. 기존 admission의 인계·재사용으로 두 조건을 함께 만족시켜야
  하며, raw sequence를 mutex 밖으로 넘긴 뒤 close와 경합하는 정리를 유지하면 안 된다.
- 판정: **B, MP-3의 B02 수정으로 생긴 수명 회귀.** 정상 MORE/FINAL close stress는 이 OOM 경계를
  고정하지 않는다.

### B203 — publish invalid-flags abort에 남은 socket pin 공백

- 근거: `core/src/api/socket/socket_message_send_api.cpp:778`~`:784`는 publish flags 검증 실패 시
  public pin보다 먼저 `abort_current_non_publish_send_sequence()`를 호출한다. 그 helper는
  `part_helper_api.cpp:1036`~`:1040`에서 자체 pin을 놓고 `:1066`~`:1069`에서 payload/context를 닫는다.
- 공개 재현 순서: A가 ROUTER의 REPLY MORE를 보관한 뒤 같은 handle에 invalid flags로
  `zlink_publish_part()`를 호출한다. 기존 코드가 해당 caller의 non-publish sequence를 abort한다.
  분리한 zero-copy prefix의 free callback을 대기시키고 B가 socket close·파괴를 끝내게 한 뒤
  callback을 반환한다. 뒤따르는 context destructor는
  `socket_request_reply_submit_api.cpp:378`~`:384`에서 restore를 수행한다.
  Closing registry의 slot 반환은 `socket_request_reply_runtime_io.cpp:731`~`:761` → `:20`~`:21`로
  파괴된 `state_->socket`을 사용한다.
- 계약·설계: 1차 B06과 같은 invalid-argument cleanup의 수명 의무다. 잘못된 family/flags의 오류
  결과가 socket 수명 위반을 허용하지 않는다. 실제 공개 entry가 공용 abort를 호출하므로
  내부 helper의 가상 사용 시나리오가 아니다.
- 수정 방향: detached payload와 context를 해제하는 소유자가 기존 pin을 끝까지 유지하도록
  통일한다. SEND/REQUEST/REPLY 네 entry에만 pin 전제를 복사하는 방식은 호출 경로를 빠뜨린다.
- 판정: **B, 1차 B06 미수정 경로.** 원래 지적한 네 entry의 수정 자체는 유효하다.

### B204 — MP-7의 readiness 확인과 mailbox 대기 사이 lost wake

- 근거: `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:598`~`:612`는 completion 상태를
  확인한 뒤 `process_commands(remaining_ms, false)`로 기다린다. 호출 이전의 command-wait epoch를
  관찰·전달하지 않는다. `process_commands()`는 `:539`에서 전달된 epoch만 사용하며,
  `mailbox.cpp:259`~`:275`는 epoch/pending hint/commandless signal만 검사한다. Completion queue나
  ready transport pipe는 이 대기 predicate에 없다.
- **completion consumer가 하나여도 가능한 순서**: B가 등록된 poller의 socket에서 NONE pull을
  하고 `:600`에서 queue가 비었음을 확인한다. Reply가 도착하고 A의 일반 send/control progress가
  activate-read와 request-completion command를 먼저 처리한다. `socket_base_api.cpp:1187`~`:1189`
  또는 `:1649`~`:1661`은 transport head를 ready 목록에 넣는다. `object.cpp:87`~`:88`의 completion
  command는 no-op이고 같은 command batch에서 소비될 수 있다. A는 completion queue를 소비하지
  않는다. B가 이제 `process_commands()`를 호출하면 mailbox는 비어 있고 기존 edge를 관찰한
  epoch도 없다. 새 command가 없으면 유한 RCVTIMEO 전체 동안 지연되고 `-1`이면 다음 drain을
  영구히 실행하지 못할 수 있다.
- Poller wait와의 동시 호출도 같은 문제다. Poller thread가 B의 마지막 `has_ready=false` 이후
  transport를 drain·publish하고 알림 command까지 소비하면 queue에 record가 있어도 B는 mailbox를
  기다릴 수 있다. Queue의 `changed.notify_all()`(`socket_completion_queue_internal.cpp:354`)은
  mailbox CV를 깨우지 않는다. 두 thread가 queue를 동시에 소비할 필요가 없는 재현이다.
- Timeout도 하나의 budget으로 완결되지 않는다. 고정된 poller 등록 상태에서는 `:580`의 budget을
  재사용하지만, 기다리던 중 registration이 제거되면 `:587`~`:590`에서 0을 반환한다.
  `socket_message_handler_api.cpp:149`~`:155`는 이미 소비한 시간을 빼지 않고 원래 RCVTIMEO로
  queue wait를 다시 시작한다. 반대로 refs=0 판정 후 registration이 추가되는 전환은 async owner가
  계속 publish한다는 `return 0`의 전제를 깨뜨릴 수 있다.
- 계약: main README `:1203`~`:1214`의 한 queue consumer, NONE의 entry RCVTIMEO·무한 대기와
  종료 계약. `05-polling.ko.md:125`~`:126`의 owner 이전 중 readiness 보존.
- 수정 방향: 기존 command observation/대기 도구를 사용해 **관찰 시작→ready 재확인/drain→동일
  epoch로 대기**를 연결하고 owner 전환에도 entry deadline을 이어야 한다. 별도 poller·재시도 횟수·
  timeout 증가는 해결책이 아니다. 정상 reply가 대기 직전에 다른 command owner에 의해 처리되는
  경계와 poller wait/pull 겹침, registration 전환을 고정한 테스트가 필요하다.
- 판정: **B, MP-7 신규 wait 경로의 결함.** 단일 drain mutex의 상호배제만으로 lost wake가 해결되지는 않는다.

### B205 — REPLY 실패·blocking REQUEST 성공의 physical sync 안 payload 해제

- REPLY 근거: `core/src/api/socket/socket_request_reply_submit_api.cpp:681`~`:703`의 complete scope
  안에서 `send_completion_staged_frames_on_pipe()`를 호출한다. 이 함수는
  `socket_request_reply_runtime_io.cpp:1410`에서 transport sync도 잡고, 후속 frame 실패 시
  `:1461`에서 rollback한 뒤 `:1464`~`:1468`에서 나머지 입력을 소비한다.
  `pipe.cpp:2628`~`:2631` → `:3878`~`:3881`은 prefix의 message도 pipe lock 아래 닫는다.
- 공개 재현 순서: 별도 복사본 없이 zero-copy part로 REPLY MORE를 제출하고, FINAL이 HWM·크기
  제한 등으로 prefix 이후에 거절되게 한다. Prefix 또는 실패 FINAL의 마지막 참조를 닫을 때
  free callback이 같은 socket의 `zlink_set_option()`을 호출하면
  `socket_base_api.cpp:674`의 physical sync 재획득에서 멈춘다
  (`socket_lifecycle_runtime.cpp:391`~`:409`). 기존 write-after-prefix failpoint로도 경계를 고정할 수 있다.
- REQUEST 근거: blocking multipart는 `socket_request_reply_submit_api.cpp:939` → `:229` →
  `socket_send_submit.cpp:818`의 fast admission을 사용한다. 기본
  `consume_multipart_on_success_=true`(`socket_base.hpp:1329`)여서
  `socket_send_submit.cpp:603`~`:604`, `:652`~`:653`이 local complete scope를 놓기 전에 caller
  originals를 닫는다. 상대 receiver가 먼저 shallow-copy payload를 닫으면 이 마지막 original
  close가 같은 종류의 재진입 callback을 실행한다. DONTWAIT만 false를 전달하는 `:554`~`:559`의
  수정으로는 이 경로가 해소되지 않는다.
- 계약·설계: MP-1 `:145`의 **모든 helper/physical lock 밖 payload 해제**. Rollback의 원자성을
  위해 lock이 필요한 것과 마지막 user payload ref를 그 lock 안에서 해제하는 것은 별개다.
- 수정 방향: physical attempt/rollback 동안 마지막 user ref가 되지 않도록 기존 whole-record
  소유권을 유지하고, 성공·실패 모두 physical scope를 놓은 뒤 소비 책임을 수행하게 한다.
  Callback을 금지하거나 예외를 삼키는 우회는 적용하지 않는다.
- 판정: **B, 이번 리뷰에서 추가 확인한 기존 physical 경로의 잔여 결함.** REPLY 저수준 helper는
  이번 diff가 새로 만든 함수가 아니다. 하지만 누적 multipart 구현의 실제 호출 경로이며 B02의
  전 경로 해소 주장을 막는다. 독립적인 신규 MP-7 회귀로 분류하지 않는다.

### B206 — checked-out REPLY token에서 RID 오류를 EBUSY로 오분류

- 근거: `core/src/api/socket/socket_request_reply_runtime_io.cpp:692`~`:701`은 `checked_out`을
  RID 일치보다 먼저 검사한다. `socket_request_reply_submit_api.cpp:313`에서 EBUSY로 변환한다.
- 공개 재현 순서: A가 X/T로 MORE를 열어 둔다. 자기 sequence가 없는 B가 **Y/T**, X≠Y로 FINAL을
  제출한다. Y는 형식상 유효한 RID다. 코드 결과는 INVALID_STATE/EBUSY이나 main README
  `:1131`~`:1132`의 RID 불일치 token은 NOT_FOUND/ENOENT다. 같은 X/T 중복과 구분해야 한다.
  A가 자기 후속 part의 RID를 바꾸는 경우는 helper spec mismatch의 EINVAL이며 이 항목과 다르다.
- 수정 방향: token이 주어진 RID의 capability인지 확인한 뒤 그 유효 token의 중복 checkout을
  EBUSY로 분류한다. 동일 RID 중복, 다른 RID+checked-out token, 자기 sequence의 RID 변경을
  서로 다른 테스트로 유지한다.
- 판정: **B, B01을 고치면서 추가된 오류 우선순위 결함.** 기존 같은 X/T 중복 테스트의 복구는 채택한다.

## MP-4/5의 counter·TLS·borrow 판정

| 확인 대상 | 판정과 근거 |
|---|---|
| C3 write의 잠금 | **적합.** 초기 `{0}`은 state 공개 전 construction이다. 그 이후 store는 생성 `part_helper_api.cpp:317`, erase `:84`, 만료 extract `:107`, close swap `part_helper_state.cpp:68`뿐이며 모두 helper mutex 아래다. `prepare_send_step()`도 `part_helper_api.cpp:757`에서 같은 mutex를 얻는다. |
| relaxed zero의 SEND/REQUEST/REPLY 적용 | **정상 API 및 기존 slot 수명 안에서 적합.** 자기 MORE의 store는 같은 thread의 후속 load보다 먼저 실행된다. Mutex가 다른 map writer를 직렬화하므로 자기 slot이 남아 있는 동안 그 이후의 크기 발행도 0이 될 수 없다. Atomic write-read coherence상 자기 store보다 과거의 0으로 되돌아가지 않는다. REQUEST의 MORE(`socket_request_reply_submit_api.cpp:1008`)와 REPLY의 MORE(`:525`)도 동일한 map 생성 경로다. Zero는 자기 slot 부재를 판단하는 negative filter일 뿐, nonzero가 자기 slot 존재를 증명하지는 않는다. |
| C3 계약과의 관계 | main synchronization model `:60`의 타 thread payload 발행용 acquire를 생략하는 일반 허가가 아니다. 여기서는 payload를 읽지 않고 자기 slot 부재만 판정하며 nonzero 뒤에는 mutex에서 재확인한다. D-B204의 승인된 제한적 근거와 일치한다. |
| borrowed `handle_state_t*` | **state 자체의 수명 주장은 적합.** `socket_base_request_reply_bridge.cpp:59`~`:67`은 acquire presence 뒤 immutable shared owner의 raw pointer를 반환한다. clear(`:87`~`:92`)는 owner를 reset하지 않는다. SEND/REQUEST entry의 pin이 socket 파괴를 늦추므로 raw state를 사용할 수 있다. **개별 `send_sequence_state_t*`의 수명까지 보장하지는 않는다**(B202). |
| TLS 참조와 만료 scan | **적합.** `part_helper_api.cpp:30`~`:50`은 TLS shared_ptr 객체의 주소를 반환하고 strong 복사를 하지 않는다. `owner_less<void>` 조회도 임시 weak key를 만들지 않는다. 정상 호출 중 TLS strong owner가 유지되므로 타 thread의 `weak_ptr::expired()`는 그 caller를 회수하지 않는다. Thread가 종료하면 weak key의 control block은 map이 보유하여 ID/주소 재사용과 구분된다. 지원 범위 밖 TLS destructor 호출은 W06 판정에 따른다. |
| 비용 주장의 범위 | SEND MORE/FINAL 성공은 helper mutex 각 1회지만 REQUEST FINAL은 `:1044`의 사전 조회와 `:834`의 prepare, REPLY FINAL은 `:510`의 active 조회와 `:525`의 prepare를 거친다. 만료가 있으면 unlock 후 relock도 있다. 모든 family에서 항상 2-part당 2회라는 주장은 성립하지 않는다. |

## MP-7의 completion owner와 계약

**기존 drain gate를 공유하는 방향은 채택할 수 있다. 현재 대기 구현은 B204 때문에 채택할 수 없다.**

`get_events()`는 `socket_base_api.cpp:788`에서 `_completion_owner_sync`를 잡는다. Poller의
`get_events_internal()`도 `:867`, async executor도 `socket_base_lifecycle.cpp:1462`에서 같은
mutex를 사용한다. 따라서 poller wait와 NONE pull이 겹쳐도 **physical drain 임계 구간을 동시에
실행하는 owner가 둘이 되는 순서**는 이 경로에서 찾지 못했다. Registration의 add/remove도
`socket_base_dispatch.cpp:232,278`에서 같은 gate로 이전을 직렬화한다. Public queue를 실제로
제거하는 `socket_completion::recv()`는 그 뒤에 별도로 실행된다.

계약의 구분은 다음과 같다.

- `05-polling.ko.md:122`의 owner는 completion bit를 가진 **poller registration** 하나다.
- README `:1203`~`:1204`는 public queue를 소비하는 thread 하나를 요구한다.
- `05-polling.ko.md:100`은 poller wait가 record를 제거하지 않는다고 명시한다. 그러므로 다른
  thread의 poller wait와 유일한 consumer의 NONE pull이 겹치는 것만으로 두 consumer 위반이라고
  볼 근거는 없다. 실제 물리 drain은 위 mutex가 직렬화하지만 wake는 B204처럼 별도로 연결해야 한다.
  두 thread가 `completion_recv()`로 같은 queue를 동시에 소비하는 경우는 여전히 지원 밖이다.
- NONE가 queue head를 확인한 뒤 DONTWAIT dequeue로 끝내는 것은 consumer가 하나이고 lifecycle이
  유지되는 동안 타 consumer가 head를 빼앗을 수 없다는 전제에서 맞다. Close가 head를 폐기하면
  queue recv의 lifecycle errno로 종료한다.

DONTWAIT는 `socket_message_handler_api.cpp:139`에서 새 physical drain을 호출하지 않는다.
앞선 `process_submit_commands()`는 기존 동작이며 이를 "어떤 socket progress도 없다"로 해석하면
안 된다. **새 transport completion drain을 시작하지 않고 public queue를 소비한다**는 의미에서
queue-only가 유지된다. `unittest_phase3_request_reply_owners.cpp:1277`~`:1304`의 NO_DATA 경계와
`:1352`~`:1378`의 finite-batch/stale-requeue 검증에 맞는 방향이다. NONE도 같은 physical batch
함수를 사용하므로 별도 fairness 규칙이나 무한 transport drain을 추가하지 않았다.

MP-7의 원인은 multipart membership 검사가 아니다. Poller가 등록되면 async completion publisher를
억제하는 `socket_base_dispatch.cpp:157`~`:161`과 종전 blocking pull의 queue-only wait 사이의
진행 공백이다. **열린 MORE 없이 single REQUEST만 있어도 같은 조건이 성립한다.** 따라서 Core
completion 계층의 기존 결함(B)으로 분리해야 하며, MP-6의 실패를 multipart의 또 다른 원인이라고
확정한 D-B209의 인과 해석은 MP-7의 진단으로 좁혀야 한다. 다른 binding 성능 격차에 대한 인과는
이 리뷰가 입증하지 않는다.

### 계약 문장 보강 제안

현재 계약을 구현에 맞춰 완화할 필요는 없다. 다만 registration owner와 queue consumer의 혼동을
없애기 위한 다음 **명료화 초안**을 감독자에게 제안한다. 보호 스펙은 수정하지 않았다.

> 한 socket의 public completion queue를 소비하는 thread는 하나다. Completion poller의 등록은
> 이 consumer가 `zlink_completion_recv(NONE)`를 직접 호출하는 것을 제한하지 않으며, blocking
> receive는 별도의 `zlink_poller_wait()` 호출에 의존하지 않고 RCVTIMEO 안에서 completion 진행과
> 대기를 수행한다. Poller wait는 public record를 소비하지 않으며, 같은 socket의 transport
> completion 진행은 하나의 직렬화된 drain 경로를 사용한다. DONTWAIT receive는 이미 게시된
> public completion queue를 소비하며 새 transport drain turn을 시작하지 않는다.

위 문장은 README Completion pull과 `05-polling` owner 절의 용어를 맞추는 제안이다. 두 queue
consumer를 새로 허용하거나 poller 등록을 제거하라는 사용 절차를 추가하는 제안이 아니다.

## MP-6와 잔여 검증 항목

### W201 — D-BP15의 열린 sequence 독립성은 관찰하지만 실패 record의 thread 이전은 미관찰

`test_writable_resubmit_from_other_thread_while_sequence_open.cpp:407`~`:433`에서 A는 MORE 뒤
barrier에 머물고 자기 FINAL로 닫는다. B는 `:525`~`:528`에서 다른 record의 첫 시도를 실패시키고
`:629`~`:670`에서 exact WRITABLE을 확인한 뒤 `:673`~`:676`에서 전부 재제출한다. Receiver는
`:571`~`:589`에서 B의 2-part·reply를, `:598`~`:616`에서 A의 2-part·reply를 확인한다.
REQUEST completion ID/context/payload는 `:269`~`:280`, `:701`~`:707`, `:728`~`:734`로 대조한다.
따라서 **A의 열린 sequence가 B를 막지 않고 A가 자기 sequence를 닫는다는 관찰은 유효**하다.

그러나 거절된 B record의 최초 MORE/FINAL과 재제출은 모두 test main thread가 호출한다.
실패를 A에서 받고 retained record를 completion thread B로 넘기는 단계가 없다. D-B207
`:1992`의 "실패 후 다른 thread에서 재제출"까지 완전히 덮었다고 보고하면 과장이다.
실패 record의 생성 thread와 재제출 thread를 분리하고, 그 사이 원 thread가 연 다른 record가
유지되는 변형을 추가해야 한다. 이 리뷰는 기존 테스트의 성공 의미를 부정하지 않고 coverage를 구분한다.

### W202 — TCP 포화 판정의 20ms quiet window

같은 파일 `:482`~`:492`는 WRITABLE event가 **20ms 동안 없으면** 실제 포화로 판정한다.
`sleep` 호출은 없지만 이 조건은 시간 기반의 부재 관찰이다. TCP writer/flow command가 20ms보다
늦게 진행되면 일시적 local 거절을 안정된 포화로 오인하고, `:533`의 B FINAL은 성공하여 테스트가
실패할 수 있다. 큰 filler의 거절이 훨씬 작은 B record의 거절을 논리적으로 증명하지도 않는다.
현재 assertion은 B FINAL이 실제로 BACKPRESSURED였는지 확인하므로 해당 경계를 거치지 않은
실행을 잘못 PASS시키지는 않는다. **성공 실행의 관찰은 유효하지만 TCP 반복 실행의 결정성은 미확보**다.
20ms를 늘리는 대신 공개 flow/credit 상태와 필요한 거절 경계를 직접 관찰하는 구성으로 정해야 한다.

실패 시 일반 worker 경로는 `abort_case()`(`:120`~`:127`)가 CV waiter를 깨우고, public receive/reply는
5초 timeout을 가지며 `:737`~`:744`에서 두 worker를 join한다. B completion 검증 실패는 abort 대신
failure를 기록하고 A를 해제하지만 최종 `:753`~`:761`이 실패를 반환하므로 false PASS는 아니다.
다만 std::thread 생성이나 테스트용 string/vector allocation 예외의 unwind는 별도 보장되지 않는다.

### W203 — B02 회귀 테스트의 잠금 검증 누락과 TLS 사전 상태 의존

`test_helper_ownership.cpp:58`~`:67`의 free callback은 `ZLINK_OPT_TYPE`을 조회한다.
`core/src/api/core/zlink_option.cpp:145`~`:156`은 이 옵션을 **socket physical sync 없이** 반환한다.
따라서 이 callback이 성공해도 physical sync 밖에서 해제됐음을 입증하지 않는다. Helper mutex도
획득하지 않으므로 그쪽 상호배제의 회귀 검증 역시 아니다. 실제 잠금을 취하는 기존 option 경로와
정리 경계를 검증해야 한다.

또한 테스트의 FINAL(`test_helper_ownership.cpp:581`)을 호출한 thread에 identity가 없으면
`part_helper_api.cpp:797`~`:802`가 만료 scan 전에 single로 반환한다. 현재 suite는 앞선 테스트의
MORE(`test_helper_ownership.cpp:981`부터)로 main thread의 TLS를 이미 생성한다. 독립적인 fresh
thread 변형은 abandoned prefix의 미계승만 확인하며 즉시 free count를 확인하지 않는다
(`test_helper_interleave.cpp:164`~`:188`). 이 테스트의 순서 의존과 회수 시점을 명시해야 한다.

### W204 — 1차 W07의 남은 실패 종료 문제

`unittest_complete_record_admission.cpp:72`~`:74`의 init 실패는 worker를 반환시키지만
`:100`~`:102`의 무기한 `more_done` wait를 깨우지 않는다. MP-3의 "worker init 실패도 barrier에
참여"는 이 파일에는 적용되지 않았다. 첫 MORE의 빈 message handle도 이후 close 없이 scope를
벗어난다(`:78`~`:98`); payload는 move되므로 이 사실만으로 payload leak을 주장하지는 않는다.

SEND/REQUEST 다중 caller fixture의 init 실패는 barrier 참여로 개선됐다. 다만 receiver의 Unity
assertion은 worker join 이전에 실행된다(`test_public_inproc_multipart_send.cpp:1040`~`:1072`,
`test_phase3_request_reply_contract.cpp:1897`~`:1926`). 실패 실행의 전체 정리가 검증됐다고 확대하면
안 된다. B202의 mutex-unlock→close→abort 경계와 B203의 callback 대기→socket 파괴 경계는 현재
신규 테스트에 고정돼 있지 않다.

### W205 — 남은 helper 탐색 비용과 회수 범위

만료 회수는 `part_helper_api.cpp:95`~`:104`의 한 번 순회로 개선됐지만 여전히 O(k)이며
REQUEST/REPLY의 사전 lookup도 남는다. identity가 없는 새 thread의 single FINAL은 scan을
생략한다(`:801`~`:802`). 이 조건에서 만료 slot은 이후 MORE/identity가 있는 caller의 helper 접근
또는 close까지 남을 수 있다. 정상 caller의 record를 막거나 새 caller가 prefix를 이어받는
문제는 아니지만 "모든 다음 helper 접근에서 즉시 회수"라는 설명은 정확하지 않다.

D-B208이 MP-3/4의 정적 runner provenance 불일치를 명시하므로 보고서 사이 Ir 수치를 독립적인
before/after 증거로 채택하지 않았다. 이번 리뷰는 성능 재측정을 하지 않았다. C3 negative filter의
타당성과 helper 획득 횟수만 코드로 판정했다.

### W206 — monitor가 async command owner를 유지할 때 MP-7의 busy loop

Monitor는 completion poller가 있어도 async command lease를 유지할 수 있다
(`socket_base_lifecycle.cpp:913`~`:917`, `socket_base_dispatch.cpp:254`~`:258`). 그 상태의
`process_commands(remaining_ms, false)`는 `socket_base_lifecycle.cpp:374`~`:379`에서 대기 없이
반환한다. `prepare_completion_pull()`은 queue가 비어 있는 동안 `get_events()`와 이 반환을
반복한다. 따라서 blocking pull이 잠드는 대신 RCVTIMEO 동안 CPU를 계속 사용할 수 있다.
MP-6는 monitor를 `:392`에서 닫으므로 이 변형을 관찰하지 않는다. Steady send 5셀 성능 결과도
이 cold completion wait의 비용을 대신하지 않는다.

## 코드 정리와 설계 판정

### S201 — 정리 소유자의 중복과 pin 전제

`abort_send_step()`과 `abort_current_non_publish_send_sequence()`가 각각 buffer/context를 move하고
잠금 밖에서 닫는다(`part_helper_api.cpp:1008`, `:1052`). 만료·close는 map node를 분리하고
`reset_send_sequence()`로 닫는다(`:111`, `part_helper_state.cpp:71`). Payload 해제 순서 자체는
유사하지만 socket pin과 node 수명 전제가 일치하지 않는다. 새 registry/helper 계층을 추가하기보다
**기존 sequence 소유자를 분리해 해제하는 책임**을 한 곳에서 확인할 수 있게 정리하는 편이 낫다.
명시적 reset 뒤 map clear가 destructor의 reset을 다시 부르는 경로는 두 번째 reset이 빈 상태라
double free로 판정하지 않았다.

### S202 — MP-7 주석과 도달하지 않는 분기

`socket_base.hpp:574`는 반환 1을 "this pull owner published"로 설명하지만
`socket_base_lifecycle.cpp:582`는 이미 있던 head에도 1을 반환한다. 반환 0의 "async owner will
publish"도 registration 전환까지 보장하지 않는다(B204). 현재 유일한 caller는 timeout 0을
건너뛰므로 `socket_base_lifecycle.cpp:602`의 timeout-zero 분기는 호출되지 않는다.
`unittest_phase3_request_reply_owners.cpp:1279`의 "completion pulls do not own physical draining"
주석도 MP-7 뒤에는 DONTWAIT로 범위를 한정해야 맞다.

설계 비교: caller별 TLS payload map이나 두 번째 token registry를 추가하는 안보다, 현재의
socket-owned caller map·token registry와 기존 physical admission을 유지하고 수명·해제 전제를
완결하는 안을 채택한다. 전자는 close/revoke마다 두 상태를 맞추는 규칙을 추가한다. 후자는 현재
실패 지점의 소유 모듈에서 수정할 수 있다. MP-7도 별도 async completion owner를 더 만드는 안보다
기존 직렬 drain을 재사용하되 wake와 entry budget을 연결하는 안이 맞다.

이번 리뷰의 코드 변경·규칙 추가는 **0→0**이다. 구현 보고서의 logical 규칙 묶음 6→3은 설계 방향의
설명으로만 받아들인다. Map과 registry의 단독 소유는 확인했지만, cleanup과 pin 전제가 아직
호출 경로별로 다르므로 이를 완결된 단순화의 증거로 채택하지 않는다. MP-7의 "2→1" 역시 실제
wait 경로 수가 하나라는 뜻은 아니다(poller/mailbox wait와 async/queue wait가 모두 남음).

## 채택 판정과 후속 검증 범위

관련 소유 계층은 Core helper의 caller slot·수명, Core req/rep registry의 token·counter,
Core physical submit의 rollback·payload 해제, Core completion의 drain·wake다. Binding이나
Framework에서 retry/poller를 추가해 보상할 사안이 아니다. 소스 변경이 없는 리뷰이므로 별도의
언어 runtime 구현·검증은 수행하지 않았다.

1차 결과 중 채택하는 것은 같은 RID/token EBUSY 복구, TLS OOM 경계, close의 무할당 map 분리,
counter의 제한적 negative-filter 논리, TLS 참조와 caller별 assembly다. 채택하지 않는 것은
"B01~B06 전부 해소", "모든 free가 두 lock 밖", "MP-6가 실패 record의 thread 이전까지 관찰",
"MP-7가 owner gate 재사용만으로 wake·timeout까지 완결"이라는 확대 판정이다.

수정 후에는 B201의 반복 revoke와 늦은 restore/commit, B202의 staging 실패/close, B203의
validation callback/파괴, B204의 관찰·대기 사이 command 소비와 owner 전환, B205의 마지막
zero-copy ref 해제, B206의 RID별 오류를 작은 고정 시나리오부터 검증해야 한다. 이 보고서는
그 실행을 수행하지 않았으며, 구현 보고서에 나온 기능·sanitizer PASS를 취소하거나 재현된 실패라고
바꿔 적지 않는다. 새로 확인한 것은 위 source 경로의 정적 결함과 coverage 한계다.

차단 항목 수 6 / 채택 가능 여부: 불가
