# Polling 스펙-구현 gap 감사

> 감사 도구: codex (정적 대조) · 2026-08-24
> 실행 테스트: 수행하지 않음 (감사 지침)

판정: **구현/문서 gap 11건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았으며, 이 보고서만 작성했다.

## 대조 완료 계약군

- 공개 ABI의 함수 signature, source-kind enum, event flag의 정수값: 일치
- raw socket·timer·FD 등록 경로와 timer `POLLIN` readiness: 대체로 일치
- socket close 뒤 한 번의 `POLLERR`와 등록 유지: socket source에서는 일치
- poller의 active wait 중 destroy가 `ZLINK_CLOSE_BUSY`/`EBUSY`인 경로: 일치
- `user_data`가 등록 pointer를 그대로 반환하는 경로와 event array의 caller-owned 복사: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `05-polling.ko.md:155-157,222-225` — `zlink_poll` timeout은 `0`, `-1`은 실패 | `core/src/api/monitoring/poller_poll_once.cpp:65-76`; `core/src/runtime/core/socket_poller.cpp:772-781` | source가 등록된 일반 timeout에서 native poller는 `-1`/`EAGAIN`을 반환하고, `zlink_poll`은 이를 `0`으로 정규화하지 않아 그대로 `-1`과 mapped `error_out`을 반환한다. 반대로 item이 0개면 `poller_poll_once.cpp:22-25`가 timeout 값과 무관하게 즉시 `0`을 반환한다. 문서의 timeout 반환 계약과 둘 다 맞지 않는다. |
| C. 문서-코드 모순 | `05-polling.ko.md:59-60,113` — `ZLINK_POLLCOMPLETION`은 raw DEALER·ROUTER 등록 전용 | `core/src/api/core/zlink.cpp:249-269`; `core/src/runtime/sockets/common/socket_send_complete.cpp:44-54` | 구현은 DEALER·ROUTER뿐 아니라 asynchronous send completion을 지원하는 PAIR·STREAM도 completion channel 보유 source로 받아들인다. 허용 socket 집합이 문서보다 넓다. |
| C. 문서-코드 모순 | `05-polling.ko.md:64-68,238-240` — completion만 처리하면 public event 없이 wait가 `0`을 반환할 수 있음 | `core/src/runtime/sockets/common/socket_base_api.cpp:528-554`; `core/src/api/monitoring/poller_api.cpp:357-413`; `core/tests/integration/test_zmp_request_reply.cpp:2435-2465` | completion drain 뒤 `get_events_for_poller`는 `ZLINK_POLLCOMPLETION` bit를 만든다. `zlink_poller_wait`는 이를 public event array에 복사해 count를 반환하며, integration test도 reply callback이 block된 뒤 wait 결과 `1`을 단언한다. |
| B. 구현 gap | `05-polling.ko.md:84-85,231` — completion bit를 `zlink_poll` item, 다른 source 또는 modify에 쓰면 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL` | `core/src/api/monitoring/poller_poll_once.cpp:38-62`; `core/src/api/monitoring/poller_api.cpp:167-201`; `core/src/api/socket/socket_message_handler_api.cpp:82-96` | one-shot socket item과 FD add/modify 경로에는 completion bit 거부가 없다. socket callback validator는 receive-dispatch 충돌만 검사하고, FD 경로는 전달된 mask를 그대로 native poller에 등록한다. 명시된 invalid-argument 실패를 보장하지 못한다. |
| B. 구현 gap | `05-polling.ko.md:200-203,230` — 잘못된 event bit는 `INVALID_ARGUMENT`/`EINVAL`, source 미지원 event는 `NOT_SUPPORTED`/`ENOTSUP` | `core/src/api/core/zlink.cpp:260-265,300-315`; `core/src/api/monitoring/poller_api.cpp:167-201`; `core/src/runtime/core/socket_poller.cpp:116-136,358-388` | 공개 add/modify 경로에는 허용 mask 또는 source별 지원 여부를 검사하는 코드가 없다. 예를 들어 `ZLINK_POLLITEMS_DFLT` 및 미정의 bit는 등록되고, FD backend는 IN/OUT/PRI만 pollset에 반영한다. `ENOTSUP`을 설정하는 source-event 검증 경로도 없다. |
| C. 문서-코드 모순 | `05-polling.ko.md:200,228` — 같은 source 재등록은 `ZLINK_CONFIG_CONFLICT`/`EEXIST` | `core/src/runtime/core/socket_poller.cpp:98-114`; `core/src/api/core/zlink.cpp:262-265`; `core/src/api/monitoring/poller_api.cpp:178-180` | socket·FD native poller의 duplicate 검출은 `errno = EINVAL`이다. 상위 API는 errno 기반 변환을 하므로 `ZLINK_CONFIG_INVALID_ARGUMENT`을 반환하며, `EEXIST`/`ZLINK_CONFIG_CONFLICT`가 아니다. timer도 FD 등록 helper를 사용한다. |
| C. 문서-코드 모순 | `05-polling.ko.md:200-201,229` — 없는 source modify·remove는 `ZLINK_CONFIG_NOT_FOUND`/`ENOENT` | `core/src/api/core/zlink.cpp:302-306,335-343`; `core/src/api/monitoring/poller_api.cpp:193-220,265-270`; `core/src/api/monitoring/poller_registration.cpp:219-233` | socket·FD의 없는 등록은 대체로 `EINVAL`과 `ZLINK_CONFIG_INVALID_ARGUMENT`으로 귀결된다. timer remove는 `errno = ENOENT`을 설정하지만 결과는 여전히 `ZLINK_CONFIG_INVALID_ARGUMENT`을 직접 반환한다. 문서가 요구한 result/errno 쌍이 지켜지지 않는다. |
| C. 문서-코드 모순 | `05-polling.ko.md:131-138,235` — `zlink_poller_event_t::fd`는 FD source에서만 유효 | `core/src/api/monitoring/poller_api.cpp:239-247`; `core/src/api/monitoring/poller_event_conversion.cpp:105-120` | timer는 내부 signaler FD로 등록되고, TIMER event 변환도 그 native FD를 `event_out_->fd`에 기록한다. TIMER source에서 `fd` field가 채워지므로 field-validity 계약과 다르다. |
| A. 문서 누락 | `05-polling.ko.md:159-197` — poller 함수는 signature만 열거 | `core/src/api/monitoring/poller_api.cpp:88-164,273-415` | `zlink_poller_new`의 `NULL`/`ENOMEM`, destroy 성공 시 caller pointer nulling 및 invalid-handle 결과, `zlink_poller_size`의 count·`-1` 규칙, `zlink_poller_wait`의 count/timeout/실패 반환과 `events == NULL` 또는 capacity `<= 0`의 `EINVAL` 규칙이 공개 구현에 있으나 문서에 없다. 세 함수의 `error_out`이 null 허용인 동작도 문서화되어 있지 않다. |
| A. 문서 누락 | `05-polling.ko.md:45-55,107-114` — FD에는 readable/writable만, `ZLINK_POLLPRI`에는 의미 설명 없음 | `core/src/runtime/core/socket_poller.cpp:381-385,582-615` | FD poller는 platform `POLLPRI`를 `ZLINK_POLLPRI`로, 그 밖의 poll error bit를 `ZLINK_POLLERR`로 public event에 변환한다. 이 FD event semantics와 반복 가능성은 공개 문서에 없다. |
| C. 문서-코드 모순 | `05-polling.ko.md:89-90,234` — poller는 source handle을 빌리고 source destroy 전에 remove해야 함 | `core/src/api/monitoring/poller_registration.cpp:40-83,236-249`; `core/tests/integration/test_multi_socket_contract_regressions.cpp:671-679` | socket 등록은 lifetime pin을 획득한다. 실제 contract test는 등록한 router를 먼저 `zlink_close`하여 성공시키고, 이후 `POLLERR`을 받은 뒤 remove한다. source가 단순 borrowed handle이고 remove가 public close의 선행조건이라는 문서 표현은 실제 lifecycle과 다르다. |

## 요확인

- `05-polling.ko.md:92-94,246`은 서로 다른 poller의 동시 사용을 허용하고 한 poller의 operation 직렬화를 caller 전제로 둔다. 구현은 `core/src/api/monitoring/poller_api.cpp:145-163,273-312`에서 같은 poller의 병행 API entry를 `ZLINK_CONFIG_BUSY`/`EBUSY`로 거부하고, completion callback 중에는 mutex를 일부러 풀어 재진입 deadlock을 피한다. callback에서 같은 poller의 어떤 호출 조합까지 허용·거부해야 하는지는 스펙이 정하지 않아, 정적 대조만으로 별도 gap으로 확정하지 않았다.
