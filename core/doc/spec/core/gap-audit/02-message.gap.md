# Message 스펙–구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 실행 없음) · 2026-08-24
> 대조 범위: 공개 header·message storage·multipart send/receive helper·정적 test 표본

판정: **구현/문서 gap 6건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았고, 실행 테스트 없이 정적 대조만 수행했다.

## 대조 완료 계약군

- `zlink_msg_t` 64-byte opaque storage, 플랫폼별 alignment, `zlink_routing_id_t` field/255-byte 상한, `zlink_free_fn`과 metadata 매크로 값: 일치
- `init`/`init_size`/`init_data`/`close`/`move`/`copy`/`adopt`/`data`/`size`/`refcnt`/`multipart_close`의 공개 ABI signature: 일치
- 빈 message 초기화, size buffer의 `ENOMEM`, zero-copy callback의 마지막 owner 해제, large storage copy의 공유 및 close 시 refcount 감소: 대체로 일치
- multipart `MORE`/`FINAL` framing, handle/family/owner-thread sequence state, `zlink_multipart_close`의 TLS receive storage 정리: 대체로 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| A. 문서 누락 | `02-message.ko.md:171,191,220,240,259,279,307,377-379` — 실패 시 config result와 진단 errno만 일반적으로 기술 | `core/src/api/message/message_api.cpp:13-38,42-59,73-88,91-129,135-157`; `core/tests/integration/test_socket_null.cpp:94-164` | 모든 공개 message 함수군은 null handle 또는 invalid/closed message에 `EFAULT`를 설정하고, config-return 함수는 `ZLINK_CONFIG_INVALID_HANDLE`, `data`/`size`는 각각 `NULL`/`0`, `refcnt`는 `-1`을 반환한다. 이 입력 조건과 함수별 결과가 문서에 없다. |
| B. 구현 gap | `02-message.ko.md:323-324,445` — 초기화되지 않은 `zlink_msg_t`의 `zlink_msg_data`는 `NULL`을 반환 | `core/src/api/message/message_api.cpp:13-24,135-139`; `core/src/runtime/core/msg.cpp:89-92`; `core/src/runtime/core/msg.hpp:96-111` | 구현은 초기화되지 않은 storage를 먼저 `msg_t::check()`로 읽고 type byte가 우연히 `101..107`이면 유효 message로 취급한다. 그런 storage에는 `data()`가 uninitialized union field를 pointer로 반환할 수 있으므로, 모든 미초기화 message에서 `NULL`이라는 계약을 보장하지 못한다. |
| A. 문서 누락 | `02-message.ko.md:360-363,377-379,461` — `*error_out_`에 성공/실패 결과를 기록한다고만 기술 | `core/src/api/message/message_api.cpp:147-157`; `core/tests/integration/test_msg_flags.cpp:58-89` | `error_out_`는 선택적이다. `NULL`이어도 `zlink_msg_refcnt`는 count 또는 `-1`을 반환하며 error result 기록을 생략한다. null 허용과 그 동작이 공개 문서에 없다. |
| C. 문서-코드 모순 | `02-message.ko.md:370-372,381-384` — `zlink_msg_refcnt`가 atomic read를 수행하고, 별도 공유 handle의 copy/close 중에도 안전 | `core/src/runtime/core/msg.cpp:527-545`; `core/src/runtime/utils/atomic_counter.hpp:29-38,103-108,167-177`; `core/src/api/message/message_api.cpp:147-157` | `refcnt_value()`는 `atomic_counter_t::get()`을 호출하지만, mutex fallback의 `get()`은 lock 없이 `_value`를 읽는다. 그 외 non-C++11 fallback도 plain `volatile` read다. 모든 지원 build에서 atomic read라는 문서의 일반 보장은 성립하지 않는다. |
| C. 문서-코드 모순 | `02-message.ko.md:420-423,465` — multipart send 중간 실패 후 아직 보내지지 않은 part는 caller 소유로 남아 close/reuse할 수 있음 | `core/src/runtime/core/multipart_send_txn.cpp:47-58,75-88`; `core/src/api/socket/socket_message_send_api.cpp:369-397`; `core/src/api/socket/request_reply_protocol_internal.hpp:289-310` | bulk multipart 실패 시 실패한 index부터 모든 part를 close한 뒤 빈 message로 다시 init한다. part API도 prepare/send 실패에서 입력 part를 같은 방식으로 소비한다. rollback으로 다음 sequence를 분리하는 점은 맞지만, 미전송 part의 caller ownership 보장은 구현과 반대다. |
| D. 구현 서술 낡음 | `02-message.ko.md:427-429` — receive helper가 sequence 동안 같은 handle·socket family·source·owner thread를 확인 | `core/src/api/socket/part_helper_api.cpp:595-637` | active sequence에서는 family와 owner thread만 비교한다. `source_socket_`은 첫 part에서 저장한 뒤 후속 part의 source와 비교하지 않고 저장된 source를 그대로 반환한다. 이 절이 설명한 source 변경 검증은 현재 코드에 없다. |

## 요확인

- `02-message.ko.md:155-156`은 두 metadata 매크로가 raw ZMP metadata codec의 사용자 key/value 범위를 고정한다고 설명한다. 정적 검색에서 매크로 자체의 사용처는 공개 header와 public-surface 검사뿐이고 codec 구현의 직접 참조는 찾지 못했다. 다만 실제 raw ZMP wire metadata의 key/value 제한은 protocol fixture 또는 wire-level test가 있어야 상수 미연결인지, 다른 codec 경로의 등가 검증인지 확정할 수 있다.
