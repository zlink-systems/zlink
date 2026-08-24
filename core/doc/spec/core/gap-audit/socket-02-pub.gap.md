# Socket — PUB 스펙–구현 gap 감사

> 감사 도구: codex (gpt-5.6, reasoning high, 정적 대조) · 2026-08-24
> 감독 검증: 공개 ABI·option mapping·PUB/XPUB 송신 경로·pipe rollback·관련 contract test 표본을 정적 대조

판정: **구현/문서 gap 5건, 요확인 1건**. 코드·스펙 문서는 수정하지 않았고, 실행 테스트 없이 정적 대조만 수행했다.

## 대조 완료 계약군

- `zlink_pub_option_t`의 9개 enum 이름·값과 `zlink_set_pub_option`/`zlink_get_pub_option` ABI signature: 일치
- `zlink_publish_part`의 PUB/XPUB 타입 제한, 비지원 타입의 `ENOTSUP`, `part_` 소비, topic frame의 첫 part 삽입: 일치
- topic byte를 포함한 message-size/HWM 검증과 `EMSGSIZE` 결과, 기본 `ZLINK_PUB_OPT_NODROP == 0`의 lossy fan-out: 일치
- 정상 송신 실패에서의 pipe rollback, PUB의 receive-flow 미지원(`ENOTSUP`)과 flow monitor detail/event 비생성: 일치
- Auto HWM budget 계산·분배는 위임 문서 소유이므로 본 감사의 gap에 계상하지 않음

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `02-pub.ko.md:43-46,116,229-231` — `NODROP=1`에서 HWM이 차면 `zlink_publish_part()`가 `ZLINK_SUBMIT_BACKPRESSURED`/`EAGAIN`을 반환 | `core/src/runtime/sockets/pubsub/xpub.cpp:384-405`, `core/src/runtime/sockets/common/socket_base_msg.cpp:370-429`, `core/tests/integration/test_xpub_nodrop.cpp:204-228,235-338` | XPUB는 HWM에서 먼저 `EAGAIN`을 내지만, `ZLINK_DONTWAIT`가 없고 `SNDTIMEO != 0`이면 공통 send 경로가 writable 될 때까지 재시도한다. 따라서 기본/양수 send timeout에서는 호출이 block한 뒤 성공할 수 있으며, 즉시 `BACKPRESSURED`를 반환하는 것은 non-blocking 또는 timeout 0/만료 경로다. 문서와 contract-test 요구가 이를 무조건 반환값으로 단정한다. |
| B. 구현 gap | `02-pub.ko.md:60-64,191-195,224-226` — 열린 sequence의 중간 또는 마지막 submit 실패는 staging part와 실패 part를 폐기하고 sequence를 닫으며, 다음 publish는 새 record여야 함 | `core/src/api/socket/socket_message_send_api.cpp:379-401,775-806`, `core/src/api/socket/part_helper_api.cpp:431-491,668-677` | topic/flag 변경, 다른 send helper, 다른 thread처럼 `prepare_send_step()` 단계에서 실패하면 호출 part는 소비되지만 활성 sequence를 `abort_send_step()`으로 정리하지 않는다. abort는 `send_fn_` 실행 뒤 실패한 경우에만 호출된다. 따라서 동일한 원래 spec으로 후속 part를 보내면 기존 record를 계속할 수 있어, 모든 중간/마지막 submit 실패 뒤 sequence를 닫는 계약을 지키지 못한다. |
| C. 문서-코드 모순 | `02-pub.ko.md:112-116` — `VERBOSE`, `VERBOSER`, `MANUAL`, `MANUAL_LAST_VALUE`, `NODROP`의 int 값은 각각 `0` 또는 `1` | `core/src/runtime/sockets/pubsub/xpub.cpp:197-224,259-272` | setter는 `sizeof(int)`와 음수가 아님만 검사한다. `2` 이상도 성공하며 boolean으로 정규화하고 getter는 `0` 또는 `1`을 반환한다. 문서의 허용값 제한과 다르다. |
| C. 문서-코드 모순 | `02-pub.ko.md:115` — `ZLINK_PUB_OPT_MANUAL_LAST_VALUE`는 “수동 모드 최신 값 caching” | `core/src/runtime/sockets/pubsub/xpub.cpp:212-218,365-377`, `core/src/runtime/sockets/pubsub/xpub.hpp:97-107` | 설정은 `_manual`과 `_send_last_pipe`를 켜며, 다음 발행에서 마지막으로 subscription event를 낸 pipe만 matching 한다. 저장된 발행 값이나 topic별 latest-value cache는 없고, 해당 상태를 보유하는 필드도 없다. 현재 공개 동작은 cache가 아니라 마지막 subscription source에 대한 수동 승인 경로다. |
| A. 문서 누락 | `02-pub.ko.md:107,140-144,161-164` — 모든 Pub option을 set/get API와 함께 사용한다고만 설명하고 `TOPICS_COUNT`만 읽기 전용으로 표기 | `core/src/api/core/zlink_option_mapping.cpp:94-104`, `core/src/runtime/sockets/pubsub/xpub.cpp:225-246,251-281` | `ZLINK_PUB_OPT_APPROVE_SUBSCRIBE`와 `ZLINK_PUB_OPT_REJECT_SUBSCRIBE`는 manual mode에서만 set이 의미 있는 action option이다. getter에는 두 option의 case가 없어 `EINVAL`으로 실패한다. 두 option이 write-only이고 manual mode 밖에서의 결과가 무엇인지 공개 문서에 없다. |

## 요확인

- `02-pub.ko.md:184-189,215-219`은 topic-frame storage 확보 실패를 반드시 `ZLINK_SUBMIT_OUT_OF_MEMORY`/`ENOMEM`으로 약속한다. 그러나 `zlink_publish_part()`는 `core/src/api/socket/socket_message_send_api.cpp:800-805`에서 `std::string`으로 topic을 먼저 복사하며, 이 할당의 `std::bad_alloc` 처리 경로가 없다. `topic_msg.init_size()` 실패는 errno를 반환하지만, 문자열 복사 실패까지 C API 결과로 변환되는지는 현재 빌드의 예외 정책·실제 OOM 주입으로 확인이 필요하다.
