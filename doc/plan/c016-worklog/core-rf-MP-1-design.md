# MP-1 — 동시 multipart 제출 설계 메모

> 독자: MP-2 구현 범위와 계약 개정을 결정하는 감독자.
> 기준: main `33ae2be045`, detached worktree `/home/hep7hep7/project/zlink-work/mp1`.
> 분석·설계 전용. 소스·공개 헤더·ABI·export·enum·errno mapper·스펙·기존 테스트를 변경하지 않았다.
> 버그 보고서의 Core 0.17.1 실행 결과와 아래 main 정적 분석은 별도 근거다. 이번 job에서 재현·빌드·성능 측정은 실행하지 않았다.

## 1. 결론과 결정 경계

**A: socket이 소유하는 caller별 staging으로 통일하고, FINAL에서 기존 complete-record admission을 사용한다.** Caller별이라는 말은 payload의 분리 단위이며 메모리를 OS thread의 TLS가 소유한다는 뜻이 아니다. PAIR·DEALER·ROUTER의 SEND·REQUEST·REPLY가 같은 caller의 sequence 규칙을 공유해야 한다. SEND만 수정하면 D-BP12의 Go REQUEST 재현은 해결되지 않는다.

단, **현재 계약을 그대로 둔 구현 착수는 불가하다. 변경 분류는 D(spec gap)**다. 독립 제출 지원은 사용자·감독자가 결정했지만, public staging 동안의 control 지연과 HWM 문구까지 모두 승인된 것으로 해석하지 않는다. 아래 D 표와 문장 초안을 감독자가 검토·개정한 뒤 MP-2를 A(확정 계약 적응)로 진행한다. 이번 job은 설계까지만 수행한다.

| 결정 | 권고 | 이유 |
|---|---|---|
| staging 위치 | 기존 socket-owned helper에 caller별 slot | close가 모든 payload를 찾을 수 있고, thread가 멈춰도 다른 caller를 막지 않는다. |
| physical admission | 기존 complete scope 안에서 record 전체 write/rollback | wire·pipe HWM·route·completion을 새로 구현할 필요가 없다. |
| control 경계 | public MORE가 아니라 **실제 pipe multipart write 구간** | pipe의 기존 incomplete-frame 판단을 재사용한다. 모든 caller의 FINAL을 기다리는 전역 boundary를 만들지 않는다. **계약 개정 필요.** |
| HWM | public staging과 physical frame admission을 구분. 기존 pipe 정책 유지 | 현재 public SEND도 이미 MORE 때 pipe HWM을 검사하지 않는다. 완성된 버퍼를 보았다는 이유로 oversized prefix까지 허용하지 않는다. |
| marker | 대상 세 socket의 staged SEND·REQUEST·REPLY에서 제거 | PUB·XPUB에는 물리 incremental 경로가 남으므로 marker 구현 전체 삭제는 별도 범위다. |
| 공개 결과 | 기존 mapper와 FINAL의 token 계약 유지 | caller 간 조립 충돌 자체는 오류가 아니다. 실제 HWM·연결·수명·인자 오류는 기존 결과로 반환한다. |

B는 caller가 FINAL을 미루는 시간까지 다른 caller를 막는다. 네 caller가 각각 MORE 성공 후 barrier에서 FINAL을 맞춰 제출하는 유효한 사용은 A에서 가능하지만, B에서는 첫 caller만 barrier에 도달하고 나머지는 MORE에서 기다린다. DONTWAIT에서는 여전히 marker 경합으로 EAGAIN이 발생하므로, B는 대기·재제출을 포함한 직렬화 지원이며 동시 조립 지원과 관찰 동작이 다르다.

## 2. 현재 구조 재검증과 전제 정정

이하 source 위치는 기준 commit의 행 번호다. 구현은 아직 바꾸지 않았다.

| 항목 | 확인한 사실 | 근거 |
|---|---|---|
| public SEND MORE | PAIR·DEALER `zlink_send_part`, ROUTER `zlink_send_part_rid`는 completion output의 NULL 여부와 무관하게 `submit_completion_aware_part`로 간다. MORE는 `send.buffered_parts`로 move한다. **바로 pipe에 쓰는 public SEND 경로라는 전제는 현재 main에 맞지 않는다.** | `core/src/api/socket/socket_message_send_api.cpp:322`, `:388`, `:418`, `:609`, `:690` |
| staging 소유자 | `handle_state_t`에 `send_sequence_state_t send` 하나. 그 안에 buffer·owner_thread·send_scope가 있다. **completion-aware도 caller-local이 아니라 socket-local 단일 slot**이다. | `core/src/api/socket/part_helper_internal.hpp:65`, `:105` |
| 첫 번째 충돌 지점 | helper mutex 아래 `active && owner_thread != current_thread`이면 EINVAL. 다른 caller의 prefix를 abort하지는 않는다. | `core/src/api/socket/part_helper_api.cpp:169`, `:176`, `:781` |
| 두 번째 충돌 지점 | `enter_public_send`는 새 multipart와 기존 multipart/complete count의 공존을 금지한다. complete 진입도 multipart bit가 있으면 EINVAL. EBUSY는 count overflow 등이며 보통의 marker 충돌은 EINVAL이다. | `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:94`, `:116`, `:121` |
| MORE 사이의 수명 | suspend는 in-flight와 sync만 반환하고 multipart bit는 남긴다. FINAL은 resume하고 reset에서 marker를 해제한다. staged 상태 자체는 실행 중 API가 아니므로 close할 수 있다. | 같은 파일 `:180`, `:199`, `:242`, `:278` |
| physical MORE | `pair_t::xsend`의 MORE는 `pipe::write`, FINAL은 `write_and_flush`. 실패한 continuation은 `rollback_incomplete`로 prefix를 제거한다. **이 설명은 physical write 계층에서는 맞다.** | `core/src/runtime/sockets/pair/pair.cpp:76`, `:89`, `:96` |
| public SEND FINAL | DONTWAIT buffered FINAL은 기존 multipart scope 안에서 `try_send_parts_scoped_once`; blocking FINAL은 buffer를 꺼내 marker를 놓고 blocking complete submit. 두 경우 모두 실패한 record를 소비한다. | `core/src/api/socket/socket_message_send_api.cpp:437`, `:458`, `:476`; `core/src/runtime/sockets/common/socket_send_submit.cpp:208` |
| REQUEST MORE/FINAL | MORE도 같은 helper buffer에 보관한다. FINAL은 buffer를 꺼내 marker를 놓은 다음 `submit_pull_*_request`에서 complete admission. 이 인계 구간에 새 MORE가 marker를 잡으면 complete 진입이 다시 거절될 수 있다. | `core/src/api/socket/socket_request_reply_submit_api.cpp:748`, `:831`, `:936` |
| REPLY의 추가 단일 소유자 | helper 이외에도 `public_router_reply_active/owner/token/target`가 socket당 하나다. helper만 map으로 바꿔도 독립 reply는 EBUSY로 막힐 수 있다. 같은 token의 중복 checkout 거절은 유지해야 한다. | 같은 파일 `:291`, `:301`; `core/src/api/socket/socket_request_reply_internal.hpp:525` |
| `multipart_send_txn`의 역할 | caller별 저장소가 아니다. 완성된 배열을 complete scope 한 번 안에서 순차 송신하고 실패하면 물리 prefix를 되돌리는 도구다. | `core/src/runtime/core/multipart_send_txn.cpp:14`, `:63`, `:294`, `:370` |
| control boundary | helper MORE는 별도 atomic bool도 세운다. `public_multipart_send_active`는 multipart bit 또는 그 bool이다. public FINAL 인계 동안 control이 앞서 나가는 것을 막는다. | `core/src/api/socket/part_helper_api.cpp:709`; `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:324`; `core/src/runtime/sockets/common/socket_send_complete.cpp:422` |

### 2.1 EINVAL과 EAGAIN은 같은 mapper 결과가 아니다

| 발생 경로 | 내부 errno와 public 결과 | 해석 |
|---|---|---|
| 다른 thread가 열린 helper sequence 진입 | EINVAL → `ZLINK_SUBMIT_INVALID_ARGUMENT` | `prepare_send_step_state_locked:176`. mapper는 errno를 EAGAIN으로 바꾸지 않는다. |
| 새 MORE가 complete admission과 충돌, DONTWAIT | `begin_public_send_scope`의 EINVAL을 `begin_send_sequence_locked:101`이 EAGAIN으로 바꾸고 recovery를 arm → `ZLINK_SUBMIT_BACKPRESSURED` | 이 변환은 mapper 밖이다. 그 경로는 MORE wait token을 만들지 않는다. |
| marker 존재 중 complete send 진입 | EINVAL → INVALID_ARGUMENT | `enter_public_send:121`. REQUEST의 buffer 인계 경쟁도 확인해야 한다. |
| 실제 pipe HWM 등 | EAGAIN → BACKPRESSURED | 기존 FINAL failure/writable 등록 경로. |
| EBUSY | 일반 submit mapper에서는 INVALID_STATE | REQUEST 전용 `from_request_submit_errno`는 별도 sequence-exhaustion 호환 규칙이 있다. 어떤 wrapper를 거쳤는지 구분해야 한다. |

Mapper 근거: `core/src/api/message/submit_result_internal.hpp:12`, `:21`, `:41`, `:87`. D-BP12의 `errno=11/result=1`과 간헐 EINVAL 관찰은 그대로 인용하되, **0.17.1 실행의 매 실패가 어느 분기였는지는 이번 정적 분석만으로 확정하지 않는다.**

## 3. A/B 비교

| 비교 항목 | A — caller별 staging + FINAL admission | B — marker 대기 직렬화 |
|---|---|---|
| 독립 MORE | 각 caller가 즉시 자신의 buffer에 보관한다. 상대 caller의 FINAL을 기다리지 않는다. | 첫 owner만 성공. 나머지는 marker 반환까지 대기하거나 DONTWAIT EAGAIN. |
| 다른 thread의 single FINAL | 자기 sequence가 없으면 독립 single record. 다른 caller의 MORE보다 먼저 admission될 수 있다. 물리 part 사이에는 끼어들지 않는다. | 열린 marker 뒤에서 기다린다. DONTWAIT라면 marker용 wait token으로 재제출한다. |
| 원자성 소유 | 한 번의 physical complete scope + 기존 pipe rollback | socket당 marker가 public MORE부터 FINAL/abort까지 배제. complete count와 상호 배제도 유지. |
| 순서 | FIRST MORE 순서는 record 순서를 정하지 않는다. 같은 pipe에 대한 실제 admission 순서만 보인다. thread 간 FIFO 보장 추가 없음. | marker 획득 순서에 따른 직렬화. 대기자 FIFO·공정성은 기존 코드에 없고 새로 약속하지 않는다. |
| flags·family·RID | 같은 **caller의 열린 sequence**에서 불변. 위반은 그 caller의 prefix와 현재 part만 소비·폐기한다. 다른 caller는 다른 RID/family를 쓸 수 있다. | marker owner의 규칙은 유지. 다른 caller는 별도 sequence로 기다려야 하며 owner mismatch를 인자 오류로 취급하면 안 된다. |
| REQUEST·REPLY | REQUEST의 ID/metadata 조회도 caller별로 바꾼다. REPLY checkout은 기존 token registry에서 분리된 token별로 유지한다. | helper뿐 아니라 request/reply의 socket 단일 owner 충돌도 같은 대기에 참여해야 한다. |
| SNDTIMEO | 기존대로 FINAL 진입 snapshot. MORE 보관 시간은 포함하지 않는다. 물리 blocking wait는 기존 경로를 사용한다. | 첫 MORE에도 entry deadline이 필요하다. MORE를 기다린 시간과 FINAL timeout은 별개 호출 예산. marker 대기 후 HWM 대기에서 deadline을 재시작하면 안 된다. |
| DONTWAIT | MORE는 staging만 한다. FINAL의 실제 자원 거절만 기존 payload-free token. 내부 짧은 sync 획득은 현행과 동일하며 wait-free를 약속하지 않는다. | MORE 실패에도 nonzero token을 돌려줘야 한다. 현재 MORE ID=0·completion 없음 계약과 충돌한다. user_context는 MORE에서 NULL이어야 한다. |
| caller 정지 | 해당 caller의 buffer만 남는다. 다른 record·control 진행 가능. | socket 전체 send가 marker owner의 FINAL/abort에 의존한다. owner thread가 종료하면 해제 주체도 필요하다. |
| 메모리 | 동시 caller 수 × 미완성 record 크기. HWM은 staging 메모리의 상한이 아니다. 별도 hidden HWM·pending cap·timer를 추가하지 않는다. | 기존 단일 slot buffer + 대기자/token. public 경로가 이미 buffer이므로 B도 staging의 byte 상한을 새로 보장하지 않는다. |
| 주된 위험 | caller identity 재사용, close와 slot 수명, control 계약 변경, warmed single path lookup 비용 | lost wake, helper/sync를 잡은 채 대기하는 deadlock, wake 원인 혼합, starvation, DONTWAIT MORE 계약 변경 |

### 3.1 HWM·total-known의 판정

| 구분 | 현재 main / A에서 유지할 동작 | B의 차이 |
|---|---|---|
| public MORE 시점 | public SEND buffer move뿐이다. `test_dontwait_hwm_is_immediate_atomic_and_pending_options_do_not_apply`는 pipe가 가득 차도 MORE OK/ID=0, FINAL EAGAIN/nonzero token을 명시한다(`test_phase3_completion_contract.cpp:996`, `:1042`). A는 이 실제 동작을 caller별로 확장한다. | 현재 buffer 경로를 유지하는 B도 HWM 판정 시점은 FINAL이다. marker를 기다린다고 첫 MORE의 pipe HWM 판정이 생기지 않는다. |
| README :440–446 | “첫 MORE frame부터 일반 byte HWM”을 **public 호출부터**라고 읽으면 현재 코드·테스트와도 충돌한다. A는 public staging과 physical frame write를 계약에서 구분해야 한다. “첫 MORE→FINAL 이동”은 요청의 physical incremental 전제를 기준으로 한 차이다. | 기존 physical incremental 방식으로 되돌려 MORE에 HWM을 적용하는 B는 현재 completion-aware public 테스트를 깨뜨리는 추가 변경이다. 권고하지 않는다. |
| physical frame별 charge | FINAL의 한 attempt 안에서도 각 frame은 payload + `sizeof(zlink_msg_t)`로 charge된다. 실패 시 미flush prefix를 되돌린다. A는 `try_admit_send_parts_scoped`를 그대로 재사용한다. | 동일하게 유지 가능. |
| total-known 승격 | **buffer가 완성됐다고 모든 prefix에 empty-pipe oversize 예외를 적용하지 않는다.** PAIR complete fast path도 `pipe.cpp:2453`에서 oversize를 제외하고 일반 경로로 돌린다. `multipart_send_txn`도 새 total metadata를 넣지 않는다. | 기존 자격 유지. |
| 기존 final 예외 | `pipe.cpp:3595`와 Auto HWM §4 “Multipart와 큰 message”(`:449`)는 빈 pipe에서 시작하고 그동안 MORE가 일반 HWM을 통과한 sequence의 마지막 frame oversize를 허용한다. single/total-known 설명만으로 이 기존 final 예외를 지우지 않는다. | 동일. |
| 추가 reservation | A는 vector/inline buffer를 보관할 뿐 pipe의 transaction 전체 credit을 선점하지 않는다. **메모리 staging은 HWM reservation과 다르다.** 새 total-size hint나 transaction reservation을 넣는 안은 별도 정책 변경이다. | marker는 배타적 소유권이며 byte reservation이 아니다. |
| 제한 초과 시점 | actual HWM·route·peer MAXMSGSIZE 오류는 FINAL에서 확인될 수 있다. 기존 validation 실패·할당 실패는 MORE에서도 가능하다. caller-local cap을 새로 만들면 errno·허용 크기가 바뀐다. | marker 때문에 MORE timeout/EAGAIN이 추가된다. |

예: HWM=1024, metadata를 포함한 첫 MORE charge=2048인 2-part record는 A에서도 physical 첫 frame에서 거절되어야 한다. 첫 MORE charge=128, FINAL charge=2048이고 시작 전 pipe가 비었다면 기존 final 예외가 적용될 수 있다. “배열 전체를 안다”는 이유로 첫 경우까지 허용하는 구현은 이 권고안에 포함하지 않는다. Auto HWM의 decoder allocation 전 reservation도 변경하지 않는다.

### 3.2 control ordering

요청의 “README control boundary 조항”은 현재 socket README에 그 이름으로 존재하지 않는다. 정확한 소유 문장은 `core/doc/spec/core/protocol/01-zmp.ko.md:241`–`:265`, 검증 요구 `:543`, ROUTER `core/doc/spec/core/socket/07-router.ko.md:419`이며, 공개 재현은 `test_sl_flow_control_boundary_coalesces_latest_state`다.

| 경계 | A 권고 | B |
|---|---|---|
| public MORE 뒤, physical write 전 | FLOW/WEIGHT가 먼저 전달될 수 있다. PAUSED→WEIGHT→RUNNING을 모두 public staging 기간에 제출해도 PAUSED가 반드시 사라진다고 보장하지 않는다. **기존 보장 변경(D).** | marker가 살아 있으므로 기존 coalescing·FINAL 뒤 전달을 유지한다. |
| 실제 pipe multipart prefix가 존재 | `pipe::write_*_control_and_flush`와 `flush_pending_peer_controls_unlocked`의 incomplete 검사·pending slot을 그대로 사용. 최종 write/rollback 뒤 최신 slot을 enqueue sequence 순서로 전달한다. | 동일. |
| 여러 caller가 동시에 staging | 전역 “마지막 caller의 FINAL까지 control 보류” 상태를 만들지 않는다. | 첫 owner의 marker가 경계를 소유. 다음 owner가 잡기 전 release/wake와 deferred flush 순서를 확정해야 한다. |
| 기존 public 경계를 유지하는 A 변형 | bool만 유지하면 A의 FINAL이 B의 boundary를 잘못 해제한다. refcount/active-set로 바꾸면 중단된 caller 하나가 모든 control을 지연하고 snapshot·handoff 규칙이 추가된다. 규칙 수를 줄이지 못하므로 기각한다. | 별도 shared boundary count는 불필요하지만 기존 marker→complete 인계 규칙은 남는다. |

ROUTER target RID는 sequence 동안 복사본으로 보존한다. 물리 pipe·generation을 MORE에서 새로 pin하지 않는다. 실제 선택·same-RID replacement·weight·PAUSE 판단은 기존 FINAL admission 소유 모듈에 남긴다. 물리 첫 frame에서 pipe를 선택한 뒤에는 기존 `_current_out/_more_out`·DEALER LB 연속 상태가 FINAL/rollback까지 같은 pipe를 지킨다(`socket_send_complete.cpp:348`).

REPLY는 SEND/REQUEST와 달리 첫 `MORE` 또는 `FINAL`에서 RID·token을 검증하고 checkout하는 기존 계약(Socket README :1102)을 유지한다. reply registry가 보관한 target의 lifetime pin과 same-RID 갱신을 caller slot이 새로 복제하지 않는다. 위 FINAL 선택 설명을 REPLY의 최초 checkout 지연으로 적용하면 안 된다.

## 4. A 구현 범위와 수명 설계

### 4.1 저장 위치 선택

| 후보 | close/ctx term 및 수명 | 비용·판정 |
|---|---|---|
| TLS map keyed by raw socket pointer | 다른 thread의 TLS payload를 close가 안전하게 비울 수 없다. socket 주소 재사용·thread 종료·TLS destructor와 socket 파괴 경쟁을 해결하려면 공유 registry/pin이 필요하다. | payload 소유권이 두 곳에 걸린다. 기각. |
| public scope 객체에 보관 | C part API에는 여러 호출을 잇는 caller-supplied scope 인자가 없다. stack scope는 MORE 반환 시 사라진다. persistent scope를 socket에 저장하면 결국 slot 설계다. | 공개 API 추가는 금지. 현재 persistent send_scope는 payload보다 오래 marker를 소유하는 문제를 남긴다. 기각. |
| socket-owned caller slot | 기존 helper가 payload와 slot을 소유. close가 한 곳을 비운다. FINAL은 buffer 소유권만 local record로 옮기고 slot을 제거한 뒤 제출한다. | **채택.** 기존 `send_part_buffer_t`의 4-part inline storage 재사용. receive state는 변경하지 않는다. |

Caller key는 함수 family나 RID가 아니라 **호출 thread의 수명 단위 identity**다. `(thread, family, RID)`별 별도 map은 같은 caller의 잘못된 family/target 변경을 새 record로 받아들이므로 금지한다.

공개 API에는 sequence ID가 없으므로 다른 thread의 FINAL을 기존 caller의 continuation인지 독립 single인지 식별할 수 없다. A는 이를 그 thread의 single record로 해석한다. “한 record를 thread 사이에 나누지 않는다”는 호출자 의무를 유지하며 이를 새 오류로 감지한다고 약속하지 않는다.

단순 `std::thread::id`만 장기간 보관하면 thread 종료 뒤 ID 재사용으로 새 caller가 옛 prefix를 이어받을 수 있다. 구현 후보는 TLS에 payload·socket 포인터 없이 thread-lifetime identity만 두고, slot이 그 identity의 weak ownership을 key로 사용하는 방식이다. `owner_less` 계열 비교를 사용하면 control block 주소가 재사용되어도 살아 있는 weak key와 충돌하지 않는다. thread 종료 후 남은 slot은 다음 helper 접근의 정리 또는 socket close에서 해제한다. **thread 종료 순간의 즉시 payload 회수까지 약속하지 않는다.** 그 보장이 필요하면 별도 요구사항이며 TLS destructor에서 raw socket을 호출하는 임시 우회로 풀지 않는다.

### 4.2 바꿀 곳과 재사용할 곳

| 파일·함수 | 변경 설계 |
|---|---|
| `core/src/api/socket/part_helper_internal.hpp` — `handle_state_t`, `send_sequence_state_t` | 단일 `send`를 caller별 sequence 저장소로 대체. SEND·REQUEST·REPLY의 spec·buffer·reply checkout 참조를 같은 slot에서 찾는다. payload를 두 map에 중복 보관하지 않는다. PUB의 물리 incremental state는 대상 socket과 분리된 기존 역할로 남긴다. |
| `core/src/api/socket/part_helper_api.cpp` — begin/prepare/resume/reset/abort/take | caller 조회·validation·append/detach를 기존 helper mutex 한 번 안에서 처리. 타 caller를 거절하는 owner 검사 대신 key 조회. target/family/flags 비교는 기존 `send_spec_equals` 재사용. 대상 세 socket의 persistent send_scope·suspend/resume 제거. |
| `core/src/api/socket/part_helper_state.cpp` — `cleanup_socket` | 모든 slot을 컨테이너에서 분리한 뒤 part를 close. zero-copy free 함수가 재진입할 수 있으므로 helper mutex·physical sync 밖에서 해제. close는 owner thread의 FINAL을 기다리지 않는다. |
| `core/src/api/socket/socket_message_send_api.cpp` — public part entry, `submit_completion_aware_part` | MORE는 caller slot에 move. FINAL은 자기 slot만 찾아 complete record를 분리. DONTWAIT는 local complete scope로 `try_send_parts_scoped_once`; blocking은 `send_completion_submit_blocking`. marker를 놓고 다시 잡는 인계 삭제. completion 등록·소비 경로 재사용. |
| `core/src/api/socket/socket_request_reply_submit_api.cpp` — `request_part_common`, `submit_buffered_request_step`, reply sequence 함수 | 현재 `helper_state->send`의 request_seq/pending_cookie 조회부터 caller별로 전환. ID 예약·metadata·correlation commit은 기존 request 모듈에 유지. 독립 reply는 기존 token registry checkout을 사용하고 socket 전체의 `public_router_reply_*` 독점 상태를 caller slot의 sequence 참조로 대체. 같은 token 중복은 계속 거절. |
| `core/src/api/socket/socket_request_reply_internal.hpp`, `socket_request_reply_runtime_io.cpp:747/:799/:862`, `socket_request_reply_internal.cpp:421` | 위 socket-wide reply owner 필드를 제거·이동하고 commit/abandon/close가 token registry를 한 번만 정리하도록 수정. token validity의 별도 복제 상태 금지. |
| `core/src/runtime/sockets/common/socket_base_request_reply_bridge.cpp` — helper cache/active flag | 현재 active bool로 “내 FINAL이 continuation인가”를 결정하면 안 된다. 본 안은 state 존재 확인 후 caller slot 조회를 사용하고 P/D/R active flag 의존을 제거한다. 최초 helper 미생성 single fast path를 유지한다. |
| `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp`, `socket_runtime.hpp`, `socket_base_msg.cpp` | 대상 세 socket의 조립은 marker를 취득하지 않는다. MORE에도 기존 generic public API lifecycle scope를 사용한다. FINAL의 physical complete scope·sync는 유지한다. PUB가 사용하는 marker/suspend/resume 코드는 전역 삭제하지 않는다. |
| `core/src/runtime/sockets/common/socket_base_flow_state.cpp:73`, `socket_base_dispatch.cpp:529`, `socket_send_complete.cpp:422` | public staging을 control 지연 원인으로 사용하는 분기를 대상 세 socket에서 제거. pipe의 실제 incomplete boundary에 위임. 다른 이유의 deferred work까지 통째로 삭제하지 않는다. |
| `core/src/runtime/sockets/common/socket_send_submit.cpp`, `socket_send_complete.cpp:242` | existing FINAL entry budget, record attempt, rollback, route selection, wait-token 등록 재사용. local scope를 받아 물리 시도를 수행하도록 연결한다. 새 retry policy·send queue·poller 없음. |
| `core/src/runtime/core/multipart_send_txn.cpp` | 기존 complete-array 송신·prefix rollback 도구로 재사용 가능. **여기에 TLS 저장소를 추가하지 않는다.** public DONTWAIT buffered SEND를 무조건 `logical_multipart_send`로 바꾸면 PAIR complete fast path를 잃으므로 `try_admit_send_parts_scoped` 경로를 유지한다. |
| `core/src/runtime/core/pipe.cpp`, PAIR/DEALER/ROUTER xsend/LB | admission·prefix rollback·frame charge·물리 control 경계는 원칙적으로 변경하지 않는다. owner를 caller helper로 옮기지 않는다. |

### 4.3 close·ctx term·실패 경로

| 시점 | A의 처리 | B의 처리 |
|---|---|---|
| MORE API 실행 중 close | MORE도 `socket_public_api_scope_t`로 admitted call이어야 한다. 실행 중 close의 기존 EBUSY 계약 유지. **public-handle pin만으로 대체 불가:** pin은 파괴 지연이며 `socket_public_handle_t::begin_close:88`는 pin이 남아도 close를 seal한다. | marker 획득 대기도 admitted API로 셀지 명확히 하고 기존 fail-fast 의미에 맞춘다. 기다리면서 helper mutex/sync를 잡지 않는다. |
| MORE 반환 후 close | 실행 중 API가 없다면 close accepted. `zlink_close:168`→`cleanup_socket`가 모든 caller buffer를 소비. 아직 admission되지 않은 prefix는 peer에 보이지 않는다. | suspended marker owner의 기존 rollback/cleanup 후 marker 해제. 대기자가 실제 API 실행 중이면 close EBUSY일 수 있다. |
| FINAL admission/대기 중 close | 기존 complete/blocking 경로의 수명 처리를 재사용. detached record는 FINAL call이 단독 소유하며 cleanup이 다시 free하지 않는다. | marker를 얻기 전 waiter, marker owner, complete waiter가 모두 같은 lifecycle 결과로 종료되어야 한다. |
| ctx shutdown/term | `process_stop:1296`의 ETERM·completion close·blocking wait 종료 사용. MORE도 기존 `is_ctx_terminated()`를 확인한다. shutdown은 socket destructor가 아니므로 남은 slot은 socket close에서 정리할 수 있다. ctx term은 모든 socket close를 기다린다(Context §`zlink_ctx_term`, `:179`). 종료 후 FINAL이 없는 경우도 close가 모든 buffer를 회수한다. | 기존 stop 채널이 marker wait도 깨워야 한다. 무한 MORE 대기가 `ctx_term`을 영구히 막지 않아야 한다. |
| MORE validation/할당 실패 | 현재 caller slot과 실패 part를 분리·소비하고 errno 보존. 타 caller slot과 request reservation은 건드리지 않는다. allocation 실패는 기존 ENOMEM 결과. | owner의 오류면 rollback 뒤 release+wake. 대기자 인자 오류는 owner의 record를 abort하지 않는다. |
| FINAL physical 중간 실패 | 기존 `try_admit_send_parts_scoped:403` 등의 rollback을 scope 해제 전에 완료. 실패 record 전체 소비. DONTWAIT는 payload-free token만 유지. NONE은 기존 attempt 복사본/entry budget을 사용한다. | 동일한 물리 rollback 뒤 marker 반환·wake. 실패한 prefix가 남은 채 다음 caller를 깨우면 record가 섞인다. |
| thread 종료·ID 재사용 | 위 identity와 socket-owned cleanup으로 새 caller가 옛 sequence를 잇지 않게 한다. abandoned payload의 최대 유지 기간은 다음 정리 또는 socket close까지다. | marker owner thread 종료 시 영구 점유 위험. 동일한 thread-lifetime 인식 또는 명확한 abandon 처리 없이는 지원을 완결할 수 없다. |

Lock 순서는 **staging용 lifecycle admission → helper mutex → buffer 분리 → helper unlock → physical submit**를 기본으로 한다. DONTWAIT FINAL을 physical scope 한 번으로 감싸는 경우에는 **physical sync → helper mutex**로 고정하고, MORE·abort 쪽에서 helper를 잡은 채 physical sync를 취득하는 역방향을 제거한다. payload 해제는 모든 helper/physical lock 밖에서 수행한다. generic lifecycle admission을 없애서 원자 연산 수만 줄이는 안은 close 계약을 깨므로 기각한다.

## 5. B 구현 범위와 lost-wake 검토

| 파일·함수 | 필요한 변경 |
|---|---|
| `part_helper_api.cpp` — `prepare_send_step_state_locked`, `begin_send_sequence_locked` | owner mismatch 즉시 EINVAL 대신 marker 대기 진입. mutex를 풀고 기다린 뒤 반드시 owner/spec를 다시 확인. 요청의 인자 오류와 다른 caller 경합을 구분. |
| `socket_lifecycle_runtime.cpp` — `enter_public_send`, `leave_public_send`, `release_public_multipart_marker` | marker 충돌뿐 아니라 새 MORE 대 complete-count 충돌, complete 대 marker 충돌 모두 같은 대기 대상으로 처리. FINAL/abort marker 해제와 마지막 complete-count 해제를 wake 전이로 사용. suspend는 marker 해제가 아니므로 wake하지 않는다. |
| `socket_base_lifecycle.cpp` — `observe_submit_progress`, `wait_submit_progress`, `notify_submit_progress` | blocking marker wait에 기존 epoch/CV/mailbox 경로 재사용. lifecycle coordinator는 completion queue를 직접 소유하지 않으므로 scope 해제 결과를 socket submit owner에 전달해 알림을 호출해야 한다. 새 poller·sleep polling 금지. |
| `socket_send_complete.cpp` — `register_send_writable_wait_after_failure`, publish/notify | marker 때문에 거절된 token을 route/credit token과 구별하는 내부 wait 원인이 필요하다. marker release가 token을 깨우고, HWM recovery가 아직 점유된 marker token을 깨우지 않아야 한다. 단순 EINVAL→EAGAIN 치환으로는 해결되지 않는다. |
| `core/src/api/socket/socket_completion_queue_internal.hpp:81`, `socket_completion_queue_internal.cpp` — reservation/waiter | 현재 target/correlation 중심 waiter에 marker 원인을 통합. public enum·completion layout은 그대로. SEND MORE도 65,536 unified slot 한도·ENOMEM·ID output 생략의 기존 소유 규칙과 일관되게 정의해야 한다. |
| request/reply submit 및 close/stop | MORE에 나온 token과 REQUEST FINAL ID를 혼동하지 않게 하고, reply owner도 대기/abort 해제 대상에 포함. close/ETERM으로 대기자 종료·reservation 정리를 연결. |

| 경쟁 순서 | 잘못된 구현 | 필요한 보장 |
|---|---|---|
| waiter가 marker 확인 → owner release → waiter 등록 | release 알림을 놓치고 무한 대기 | attempt **전** `observe_submit_progress`, 등록 후 epoch/predicate 재검사. release가 waiter 수 0이어도 epoch를 증가시킨다. 기존 `notify_submit_progress:745`가 이 규칙을 가진다. |
| wait-token 등록 직전 marker release | token은 영원히 미완료 | queue에 완전히 연결한 뒤 marker 자격 재검사. 기존 `register_send_writable_wait_after_failure:215`의 register→fence→recheck 구조를 실제 거절 자원에 맞게 적용한다. |
| marker 점유 중 pipe credit/attach | readiness가 true라서 조기 WRITABLE | marker token의 readiness는 marker/complete-count 배제 조건을 검사해야 한다. `xsend_writable_target_ready`만 호출하면 부족하다. |
| marker release 뒤 다른 caller가 선점 | wake를 admission 보증으로 오해하거나 token을 두 번 발행 | WRITABLE는 재시도 가능 신호이며 reservation이 아니다. token당 한 건만 발행. 재제출의 실제 자원 경합은 기존 모델로 처리. |
| marker release, route 소멸, ctx stop이 경쟁 | 다른 종료 원인으로 중복 completion | 기존 completion reservation owner가 한 terminal 결과를 확정. marker와 route의 별도 completion queue 금지. |
| waiter가 helper/sync를 잡고 대기 | owner가 FINAL에 들어올 수 없음 | 대기 전 lock 해제, wake 뒤 상태 재검사. 처음 snapshot한 deadline만 사용. |

현재 `notify_incremental_send_released`는 control flush만 하며 marker 대기자를 위한 `notify_submit_progress`나 token publish를 하지 않는다. `POLLIN`, `POLLCOMPLETION`, unread WRITABLE의 `POLLOUT` level을 바꾸지 않고 release를 새 wake 자원으로 넣어야 하므로 B도 계약 작업이 필요하다.

## 6. 기존 테스트 영향과 신규 검증

### 6.1 기존 테스트 판정(정적 예측, 실행 결과 아님)

| 기존 test 이름 / 파일 | A | B |
|---|---|---|
| `test_complete_record_admission_rejects_new_multipart_sequence` — `core/tests/unittest/unittest_complete_record_admission.cpp:48` | **기존 기대와 충돌.** 다른 caller의 MORE가 EINVAL 대신 staging 성공. | **기존 기대와 충돌.** NONE MORE가 기다리므로, holder 해제 전 contender.join을 하는 fixture 자체도 대기한다. |
| `test_open_send_part_sequence_rejects_concurrent_single_records` — 같은 파일 `:533` | **기존 기대와 충돌.** single record 성공을 허용해야 한다. | **기존 기대와 충돌.** caller가 contender 완료 후 FINAL을 내는 fixture라 timeout/교착이 발생한다. 단순 assertion 변경으로 해결할 수 없다. |
| `test_sl_flow_control_boundary_coalesces_latest_state` — `core/tests/integration/test_dealer_router_single_lane_contract.cpp:2333` | **control 계약 개정 대상.** public MORE 뒤 PAUSED가 보이지 않는다는 기대를 더는 보장하지 않는다. physical boundary 검증과 public staging 관찰을 별도 test로 확정해야 한다. | 유지해야 한다. marker handoff wake 순서 변경으로 깨질 위험은 있다. |
| `test_dontwait_hwm_is_immediate_atomic_and_pending_options_do_not_apply` — `test_phase3_completion_contract.cpp:996` | 유지. 이미 MORE 성공/FINAL HWM 실패·payload-free token을 검증한다. | buffer 유지 B는 유지. MORE부터 pipe HWM으로 되돌리는 B 변형은 실패한다. |
| `test_pipe_rejects_multipart_before_partial_bytes_exceed_hwm`, `test_empty_pipe_incomplete_multipart_stops_at_max_message_size`, `test_drained_pipe_oversize_multipart_uses_fresh_peer_credit` — `unittest_router_pipe_contract.cpp:739/:813/:909` | 유지. caller staging을 total-known 예외 확대로 오해하면 회귀한다. | 유지. |
| `test_peer_control_does_not_complete_open_application_multipart` — 같은 파일 `:86` | 유지. **physical prefix 보호는 제거하지 않는다.** | 유지. |
| `test_wrong_send_helper_aborts_open_sequence`, `test_target_change_aborts_open_routed_sequence` — `test_helper_interleave.cpp` | 같은 caller의 실패·소비 기대 유지. map key에 family/RID를 넣으면 깨진다. | 유지. |
| `test_pair_close_aborts_suspended_multipart_without_exposing_prefix`, `test_pair_peer_termination_races_local_multipart_cleanup` — 같은 파일 | 유지. 여러 caller slot으로 확장 검증 필요. | 유지. 새 waiter가 없는 기존 케이스의 close는 더 느려지거나 대기하면 안 된다. |
| `test_dealer_multipart_size_failure_rolls_back_and_preserves_errno` — `test_public_inproc_multipart_send.cpp:300` | FINAL EMSGSIZE·전량 소비 유지. | 유지. |
| `test_pair_one_call_multipart_backpressure_aborts_before_concurrent_final`, `test_pair_whole_multipart_does_not_interleave_concurrent_final_records` — `unittest_complete_record_admission.cpp:346/:355` | 유지. 완성 배열 송신의 physical 원자성 회귀 감시. | 유지. |
| `test_public_inproc_pair_send_is_safe_from_multiple_threads`, `test_public_inproc_dealer_send_is_safe_from_multiple_threads` | 기존 single record concurrency coverage이며 4 caller의 독립 MORE/FINAL 검증을 대신하지 않는다. | 동일. |

기존 public contract test는 이번 job에서 변경하지 않았다. 위 충돌 test는 **계약 개정 후** 감독자가 새 의미와 fixture를 승인하는 대상이지, 구현을 통과시키기 위해 기대값을 완화할 대상이 아니다.

### 6.2 신규 테스트 목록

아래 이름은 제안이며 아직 생성하지 않았다. sleep으로 경쟁을 유도하지 말고 barrier와 기존 test hook으로 구간을 고정한다.

| 제안 test | 입력과 통과 조건 |
|---|---|
| `test_dealer_four_callers_stage_two_parts_independently` | DEALER 1 socket, 4 OS thread, thread마다 `(caller_id, part_index)` 2-part. 시작 barrier, **4개의 MORE 성공 뒤 두 번째 barrier**, FINAL 동시 제출. NONE/DONTWAIT, inproc/TCP. 충분한 HWM·ready 상태에서 8 part 모두 소비되고 4 record가 정확히 한 번, 각 2-part·동일 caller ID로 수신. 실패 0, mixing 0, token 0. |
| `test_pair_four_callers_stage_two_parts_independently` | PAIR 동일. zero-byte FINAL도 포함. 4개의 MORE 완료를 상대 FINAL에 의존하지 않는다. |
| `test_router_four_callers_stage_two_parts_independently` | ROUTER 동일. 같은 RID 집중 및 서로 다른 RID 두 케이스. RID마다 올바른 record만 수신. |
| `test_request_four_callers_complete_independently` | D-BP12 직접 회귀. 같은 DEALER에서 4개의 2-part REQUEST, 모두 admission 성공·고유 ID, 각 reply/context 정확히 한 completion. ROUTER requester도 검사. |
| `test_reply_independent_tokens_submit_concurrently` | ROUTER에서 독립 token 4개에 2-part reply 동시 제출 성공. 동일 token 중복 checkout은 기존 결과로 거절. |
| `test_other_caller_single_final_during_staging` | A가 MORE 후 정지. B의 single FINAL이 A의 FINAL 없이 성공·수신. 나중에 A의 두 part는 별도 record로 도착. P/D/R, public SEND 및 REQUEST family 혼합. |
| `test_caller_local_validation_abort_is_isolated` | A/B MORE 뒤 A가 flags/family/RID/part_flag를 잘못 제출. A만 폐기·실패 part 소비, B는 정상 FINAL. 오류 뒤 A의 single FINAL이 옛 prefix를 이어받지 않음. |
| `test_close_discards_all_staged_callers` | 모든 MORE 반환 후 close. 모든 zero-copy free counter 정확히 1, prefix 미노출, owner thread의 FINAL 필요 없음. thread는 close 뒤 raw handle로 FINAL하지 않음. |
| `test_staging_entry_close_race` | MORE append 진행 hook과 close 경쟁: admitted API면 기존 busy 규칙, close accepted 뒤 진입은 shutdown. helper state UAF·중복 free 없음. |
| `test_ctx_shutdown_with_staged_and_blocked_records` | 여러 staged caller + HWM에 대기하는 FINAL. shutdown이 blocking 호출을 ETERM으로 끝냄. 모든 socket close 후 ctx term 종료, staged/free/token reservation 잔여 0. |
| `test_thread_identity_reuse_does_not_continue_record` | MORE 후 thread 종료, 새 thread 생성·identity 재사용 유도. 새 caller의 FINAL은 single이고 옛 prefix를 잇지 않음. close 때 abandoned payload 전량 회수. |
| `test_staged_hwm_timing_and_oversize_eligibility` | 가득 찬 pipe에서 MORE OK/FINAL EAGAIN+token. 첫 physical MORE 자체가 HWM 초과인 record 거절. 작은 prefix+oversized FINAL의 기존 빈 pipe 예외 유지. frame charge·rollback snapshot 보존. |
| `test_physical_record_controls_and_staging_order` | physical write 중 FLOW/WEIGHT는 끼어들지 않음. public staging만 있을 때에는 control이 진행하고, first-MORE FIFO는 보장하지 않음. |
| `test_multipart_writable_tokens_preserve_resource_cause` | 실제 HWM·flow·ready·route-loss 복구/종료, token당 WRITABLE 1개, reservation 65,536 공유, unread POLLOUT/POLLCOMPLETION level 유지. |
| B 전용 `test_marker_wait_release_before_registration` 등 | FINAL/abort/마지막 complete release를 waiter 등록 전·중·후에 고정. NONE SNDTIMEO 0/양수/-1, DONTWAIT MORE token, target-loss/ETERM, helper lock 미보유를 검증. 4-MORE barrier test는 B에서 통과할 수 없으므로 A와 같은 지원으로 보고하지 않음. |
| TSan 동시성 gate | 위 concurrency/close/ctx/identity/control/wake를 **memory/atomic access 계측이 켜진** TSan으로 실행해 새 race 0. suppression으로 MP race를 지우지 않음. |

### 6.3 MP-2 검증 순서

| 단계 | 검증 |
|---|---|
| 이번 MP-1 | source/spec/test 정적 대조만 수행. build/ctest/TSan/벤치 실행 없음. 미실행을 PASS로 보고하지 않는다. |
| 최소 회귀 | 신규 multipart test + `test_helper_interleave`, `test_helper_ownership`, `unittest_complete_record_admission`, `test_public_inproc_multipart_send`. 첫 실패를 분리한다. |
| 관련 suite, 각 5회 | `test_phase3_completion_contract`, `test_phase3_request_reply_contract`, `test_request_writable_contract`, `test_dealer_router_single_lane_contract`, `test_ctx_destroy`, `unittest_router_pipe_contract`, 관련 wake/flow test. STREAM public 계약도 5회. `ctest --repeat until-fail:5 -R '<확정한 target 패턴>'` 사용. 전체 ctest는 감독자 gate 소유. |
| TSan | worktree의 기존 TSan 구성을 검토. `core/CMakeLists.txt:77`–`:78`은 ENABLE_TSAN에서 memory/atomic instrumentation을 끄므로 **ENABLE_TSAN=ON만으로 race 검증 PASS라고 하면 안 된다.** 실제 compile flags를 확인하고 해당 비활성 옵션이 없는 별도 worktree TSan 구성을 사용한다. 변경이 필요하면 감독자에게 구성 차이를 제시한다. |
| 성능 | 아래 5셀 1회 + 별도 4-thread 2-part/혼합 single 측정. foreground 실행, PERF_LOCK과 시작 load average 기록. reference 갱신 금지. |

실험이 필요할 때만 mp1 worktree의 dev 트리를 사용한다. 공통 규칙의 마지막 메모리 규칙이 앞선 JOBS=6을 대체하므로 JOBS=4, release/LTO 실험은 하지 않는다. 이번 job에서는 실험하지 않아 worktree source diff가 없다.

## 7. 성능 추정

**원자 연산은 uncontended 성공 경로의 RMW 수를 센다.** public-handle pin, msg refcount, queue 회계는 전후 공통으로 제외하며 CAS 재시도·실패·waiter 등록은 별도다. “sync 획득”은 lifecycle word의 논리 lock이며 pthread mutex 횟수와 합치지 않는다.

| 2-part SEND의 해당 부분 | 현재 | A 보수적 설계 | B |
|---|---|---|---|
| MORE lifecycle RMW | enter + suspend = 2 | generic public enter + leave = 2. **Δ0** | 2 + 경합 시 재시도/대기 비용 |
| MORE public sync 획득 | 1 | 0. **−1/record** | 1 |
| DONTWAIT FINAL lifecycle RMW | resume + scope 해제 = 2 | local complete scope enter + leave = 2. **Δ0**. 그 scope 안에서 slot 분리·물리 attempt를 수행한다. | 2 + release 알림 비용 |
| NONE FINAL과 첫 physical attempt | multipart resume/reset 2 + complete attempt 최소 2 | generic staging scope 2 + 기존 complete attempt 최소 2. **Δ0** | 현재 비용 + marker wait 진행 |
| staging helper mutex | part마다 1회, 2/record | part마다 1회, 2/record. **Δ0**. payload 해제를 lock 밖으로 이동 | 대기 전 해제·후 재취득만큼 증가 |
| helper active/boundary atomic store | 2-part에서 active true/false + boundary hold/release, 기본 4회 | 대상 경로에서 제거 가능, **−4 stores/record**. generic lifecycle는 그대로 | 유지 |
| buffer move/pipe lock | 이미 buffer move와 FINAL 물리 write 사용 | 기본 Δ0. PAIR complete fast path 보존. slot allocation/lookup 비용은 추가 | 기본 Δ0, 경합 시 wake 비용 추가 |
| marker release wake | 없음 | 새 채널 없음 | `notify_submit_progress` 재사용 시 적어도 epoch fetch_add 1 + waiter load 1/관련 release. sleeper가 있으면 progress mutex/CV·mailbox 알림, token이면 completion mutex·publish 추가 |

따라서 **A를 “2-part당 RMW 4개 삭제”라고 평가하면 잘못이다.** MORE의 lifecycle protection이 사라지지 않는다. 일반 lifecycle scope와 local complete scope를 무심코 중첩하면 DONTWAIT FINAL에 **+2 RMW**가 붙으므로 구현 리뷰에서 확인한다. 단순화의 확실한 후보는 per-MORE sync와 조립 marker/control 상태이며 throughput·Ir 이득은 미측정이다.

현재 `hotpath_gate.py`의 5셀은 **모두 single-part**다(`hotpath_bench.cpp:156`, `:608`, `:449`). gate만 통과해도 이번 동시 multipart 개선을 측정한 것은 아니다.

| hotpath_gate 셀 | 보관된 reference Ir/iteration¹ | A의 성공 경로 ΔRMW / Δmutex(추정) | B의 성공 경로 영향(추정) |
|---|---:|---|---|
| `dealer_dealer_inproc` | 3230.922 | send helper 미생성 시 0/0. 이후 helper가 존재하면 caller 조회 mutex가 +1/send일 수 있음 | complete-release를 항상 통지하면 +1 epoch RMW/send, waiter 없을 때 mutex +0 |
| `dealer_router_reqrep_inproc` | 18663.506 | 기존 single REQUEST/REPLY fast path 유지 조건에서 0/0. reply recv가 helper를 이미 생성하므로 caller 조회 통합 방식에 따라 reply lookup +1 가능 | request/reply의 실제 scope release 횟수에 비례. 최소 2회라고 단정하지 말고 경로별 계수 필요 |
| `pair_inproc` | 2348.457 | sender helper 미생성 single path 0/0. warmed multipart socket은 lookup +1/send 가능 | 같은 release 알림 방식이면 +1 epoch RMW/send |
| `router_router_tcp` | 2972.5318 | sender helper 미생성 single path 0/0. 물리 route-shard/pipe lock Δ0 | release 알림을 적용한 scope 횟수만큼 증가 |
| `stream_tcp` | 14623.471 | STREAM은 FINAL-only 경로를 유지하므로 0/0 목표 | P/D/R marker wait를 STREAM까지 적용하면 불필요한 회귀. 대상별 적용에서 0/0 목표 |

¹ `core/tests/perf/hotpath_reference.json`에 저장된 값이며 이번 job의 before 측정값이 아니다. gate 허용치는 현재 5%지만 개선을 주장할 측정값은 없다. 별도 single-after-multipart 셀에서 +1 lookup mutex 가능성을 확인해야 한다. 이를 없애려고 TLS raw slot cache나 두 번째 active map을 추가하지 않는다. 필요하면 기존 helper 소유 구조 안에서 최적화하고 같은 사실의 소유자는 하나로 유지한다.

일반 ordered map 구현은 active caller k명에서 조회 O(log k), 첫 MORE에 node 할당 1회, FINAL에 node 해제 1회가 추가될 수 있다. TLS identity의 최초 생성에도 thread당 1회 할당이 필요하다. 이것은 기존 2-part inline payload buffer의 무할당과 별개이며, 정상 multipart의 성능 회귀 위험이다. 저장 방식의 최종 선택은 이 비용을 실제로 측정한 뒤 확정한다. 현재 표는 이 추가 비용을 0으로 가정하지 않는다.

메모리 추정: record가 n-part, 동시 caller가 k명이면 descriptor만 대략 `64 × n × k` byte이고 여기에 slot/컨테이너·payload 참조가 붙는다. 4-part inline buffer는 짧은 record의 spill 할당을 줄일 뿐 caller slot 생성까지 무료로 만들지는 않는다. 기본 MAXMSGSIZE=0에서 미완성 caller의 staging은 byte HWM으로 제한되지 않으며, A는 이 비용을 숨기지 않는다.

## 8. 감독자 결정이 필요한 D 항목

| ID | 깨지거나 불명확한 문장 | A에서 확정할 변화·이득 | B의 대안/한계 |
|---|---|---|---|
| D1 | Socket README :49와 :944, Message :108 | thread별 독립 sequence 동시 보관·FINAL 원자 admission. 서로 다른 caller 충돌 오류 제거. | NONE 직렬화·DONTWAIT 재제출 지원으로 범위를 좁혀야 함. |
| D2 | Socket README :440–446, :1312 | public staging과 physical MORE HWM 구분. 현재 public HWM test와 문장을 일치시킴. staging은 pipe HWM 밖이며 total-known 예외 확대 없음. | 현재 buffer 경로를 유지하는 B도 동일한 명료화 필요. |
| D3 | ZMP :241–265, :543; ROUTER :419; single-lane control test | control 보호를 실제 pipe multipart로 한정. caller가 FINAL을 미뤄도 control 진행. | 기존 public boundary 유지 가능하지만 owner 정지로 send/control 지연. |
| D4 | Socket README :397–399, REQUEST :1071, ZMP :472의 “admission 전에 payload를 보관하지 않는다” | API 호출 사이 조립 buffer와 거절된 완성 record의 pending 보관을 구분. rejected FINAL payload 미보관·PENDING 옵션 no-op 유지. | 현재 구현에도 필요한 구분. MORE token 도입 시 pending 범위 추가 확인. |
| D5 | ROUTER :54 “같은 handle에서 다른 family/ID를 섞을 수 없다” | 같은 caller의 sequence에만 적용. 다른 caller의 target/family는 독립. 같은 caller의 오류 폐기 계약 유지. | 대기자의 새 record와 owner의 잘못된 continuation을 구분해야 함. |
| D6(B 전용) | Socket README :959–989: MORE ID=0, NONE FINAL timeout, DONTWAIT FINAL token | A는 유지. 새 errno/enum/API 불필요. | MORE timeout·MORE nonzero token·marker release WRITABLE 원인 추가가 필요. |

위 경로는 모두 보호 문서다. **이번 보고서에 제안만 적었으며 실제 문장은 수정하지 않았다.** README 세 위치만 개정하고 D3/D4/D5와 해당 검증 요구를 남기면 계약이 다시 충돌한다. 감독자는 승인 범위를 그 소유 문서까지 명시해야 한다.

규칙 수(대상 P/D/R의 조립·admission·control 소유 규칙만 셈): **수정 전 6 → A 3, B 최소 8**. 전 6개는 단일 slot 독점, marker 상호 배제, suspend/resume, complete 진입 배제, FINAL marker 인계, public control lease다. A 3개는 caller별 sequence/정리, FINAL physical admission/rollback, pipe physical control 경계다. B는 전 6개에 marker deadline 대기와 marker 원인별 wake를 더한다. 공통 message 소비·routing·lifecycle 규칙은 전후 동일하게 유지하며 이 수에 넣지 않았다.

## 9. 스펙 문장 초안 — A 채택 시

이 절의 인용문은 감독자 검토용이며 현행 스펙이 아니다. 규칙 문장만 제시한다.

### Socket README :49 — thread 안전성

> `send`는 여러 thread에서 동시에 호출할 수 있다. PAIR·DEALER·ROUTER에서 각 thread가 같은 socket에 독립된 multipart record를 제출할 수 있다. 한 record의 첫 `MORE`부터 `FINAL`까지는 같은 thread에서 호출해야 한다. 다른 thread의 미완성 record는 새 record의 조립을 막지 않는다. Core는 각 record를 다른 record의 part가 끼어들지 않도록 원자적으로 admission한다. 서로 다른 thread 사이의 record 순서는 첫 `MORE` 호출 순서로 보장하지 않는다.

### Socket README :944 — Part send와 pending admission

> PAIR·DEALER·ROUTER의 `MORE`는 같은 socket을 호출한 thread별 sequence에 part를 보관한다. `MORE` 성공은 pipe admission을 뜻하지 않으며 completion ID는 `0`이다. `FINAL`은 그 thread가 보관한 prefix와 현재 part를 하나의 record로 제출한다. 열린 sequence가 없는 thread의 `FINAL`은 single-part record를 제출한다.
>
> 같은 sequence의 함수 family, target과 flags는 같아야 한다. 서로 다른 thread의 sequence는 서로 다른 family와 target을 사용할 수 있다. 중간 호출이 실패하면 해당 thread의 prefix와 실패한 part를 모두 소비·폐기한다. 다른 thread의 sequence는 변경하지 않는다. 재시도할 caller는 첫 part 제출 전에 전체 record를 따로 보관해야 한다.
>
> Core는 socket close 때 모든 thread의 미완성 sequence를 폐기한다. API 호출 사이에 part를 보관한 상태는 실행 중인 API로 세지 않는다. 실행 중 API와 close의 관계는 thread 안전성 절을 따른다. 같은 thread가 하나의 socket에서 여러 record의 part를 번갈아 제출할 수는 없다.

### Socket README :440–446 — HWM

> HWM은 pipe가 보관하는 frame의 accounted byte에 적용한다. PAIR·DEALER·ROUTER의 public `MORE` 조립 buffer에는 pipe HWM을 적용하지 않는다. 해당 record의 pipe admission과 HWM 판정은 `FINAL`에서 수행한다. 조립 buffer는 transaction 전체의 pipe credit을 예약하지 않으며 `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`의 제한 대상이 아니다.
>
> Pipe에 incremental multipart를 쓰는 경우에는 첫 `MORE` frame부터 일반 byte HWM을 적용한다. Caller별 buffer에서 완성한 record도 이 frame별 admission을 따르며, buffer의 전체 크기를 안다는 이유로 `MORE` frame에 빈 pipe oversize 예외를 적용하지 않는다. 비어 있는 pipe에서 시작해 앞선 `MORE` frame들이 일반 HWM 검사를 통과한 record는 마지막 frame이 HWM을 넘더라도 한 record로 완성할 수 있다. Admission 시점에 전체 charge가 확정된 single-part 또는 total-known complete message는 빈 pipe에서 HWM보다 크더라도 한 건을 받아들일 수 있다. Public `MORE`로 조립한 record의 prefix는 이 total-known 예외 대상이 아니다. 모든 경우에 `ZLINK_OPT_MAXMSGSIZE`를 적용한다. 이 예외를 위해 known-total metadata나 transaction 전체 reservation을 추가하지 않는다.

### Control 경계 — README의 관련 계약 연결 및 ZMP 소유 문장

> FLOWSTATE와 WEIGHT는 실제 pipe에 쓰기 시작한 multipart record의 part 사이에 삽입하지 않는다. 열린 physical record 동안에는 kind별 최신 상태만 보관하고, FINAL commit 또는 rollback 뒤 다음 record 경계에서 살아남은 상태를 enqueue sequence 순서로 전달한다. Public `MORE`의 조립 buffer만 존재하는 동안에는 control 전달을 보류하지 않는다. Control은 아직 pipe admission을 시작하지 않은 record보다 먼저 전달될 수 있다. Control은 이미 commit한 record를 앞지르거나 inactive·initial transport hold를 우회하지 않는다.

### Payload 보관과 family/RID 조항의 함께 고칠 문장

> Core는 API 호출 사이의 미완성 sequence를 caller별 조립 buffer에 보관한다. FINAL admission이 실패한 뒤에는 그 record의 payload를 보관하지 않는다. WRITABLE 대기를 위해서는 token, target과 user context만 유지한다. 같은 caller가 같은 socket에서 연 sequence의 함수 family와 target은 FINAL 또는 abort까지 바뀌지 않는다.

## 10. 결과와 남은 검증

| 항목 | 결과 |
|---|---|
| 결과 | A/B 설계 비교, 현재 구조 정정, 코드·스펙·기존 test 대조, A 권고, 감독자용 D 표와 한국어 규칙 초안 완료. |
| 변경 파일 | `doc/plan/c016-worklog/core-rf-MP-1-design.md`, `doc/plan/c016-worklog/progress-MP-1.md`만 작성. |
| 실행 테스트 | 없음. 분석·설계 전용으로 source/spec/test를 정적으로 확인했다. |
| 남은 실패 | D-BP12의 기존 동시 multipart 실패는 구현 미수정으로 해결되지 않았다. TSan·성능·신규 concurrency 통과는 MP-2 이후 확인 대상이다. |
| 실험 diff | 없음. detached mp1 worktree에 소스 수정·빌드·실험 산출물이 없다. 적용할 patch 없음. |
| 사양 확인 | Socket §2/Part send/HWM, Message §4, ZMP control boundary, Auto HWM §4, Context 종료를 대조했다. **이번 job에서 어떤 스펙 문장도 수정하지 않았고 runtime 동작도 변경하지 않았다.** 제안 A의 관찰 동작 변화는 §8에 명시했다. |
| 소유 계층 | Core API helper가 caller별 조립·소비, socket runtime이 FINAL admission·lifecycle·wait, pipe가 frame 회계·물리 원자성·control 경계를 소유한다. |
| 교차언어 | Framework runtime 변경 없음. Go `bindings/go/internal/native/socket_multipart.go:91/:192`의 `LockOSThread`를 확인하여 caller=OS thread 조건을 충족함을 대조했다. 언어별 자체 send lock 추가는 불필요하다. binding 전체 parity 실행은 이번 범위 밖이다. |
| 변경 분류·멈춘 지점 | **D — 계약 공백/관찰 동작 개정 제안.** 소스 구현에 들어가지 않았다. 감독자의 계약 선택·개정 뒤 MP-2 착수. |
