# Socket — XSUB 스펙-구현 gap 감사

> 감사 도구: codex (GPT-5, 정적 코드 대조) · 2026-08-24
> 검증 범위: `core/doc/spec/core/socket/05-xsub.ko.md`, `core/include/`, `core/src/`, 관련 `core/tests/` 표본. 실행 테스트 없음.

판정: **구현/문서 gap 8건, 요확인 0건**. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- `zlink_sub_option_t` ABI와 `ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400`, `int` 조회 경로: 일치
- SUB/XSUB 전용 `zlink_set_sub_option`·`zlink_get_sub_option` signature와 공통 option 분리: 일치
- XSUB의 `zlink_set_subscription`/`zlink_unset_subscription`이 구독 control message를 만들어 upstream pipe로 보내고, 새 연결·hiccup에도 현재 구독을 재전송하는 경로: 일치
- `zlink_subscribe_part` signature, SUB/XSUB type 제한, 성공 시 source RID `NULL`, topic buffer 부족 재시도와 multipart 같은-thread 진행: 일치
- XSUB의 receive-flow 미지원, `ENOTSUP`/`ZLINK_CONFIG_NOT_SUPPORTED`, flow monitor detail/event 제외: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `socket/05-xsub.ko.md:17-19,36-42,119-122,252` — XSUB가 등록한 byte-prefix filter에 맞는 topic message를 구독·수신 | `core/src/runtime/sockets/pubsub/sub.cpp:7-15`; `core/src/runtime/core/options.cpp:123`; `core/src/runtime/sockets/pubsub/xsub.cpp:360-385,620-633` | `SUB`만 `options.filter = true`로 설정하고 XSUB의 기본값은 `false`다. XSUB 수신은 `!options.filter || match(...)`이므로 local subscription과 무관하게 들어온 모든 message를 전달한다. upstream XPUB가 filtering할 수는 있어도 raw XSUB 자체의 filter-match 수신 계약과 다르다. |
| C. 문서-코드 모순 | `socket/05-xsub.ko.md:213-215,223-224,259-260` — `zlink_subscription_at`의 작은 buffer는 `ZLINK_CONFIG_BUFFER_TOO_SMALL`/`ENOBUFS`, 비지원 type은 `ENOTSUP` | `core/src/api/core/zlink_option_subscription_api.cpp:73-76,160-167`; `core/src/api/core/config_result_internal.hpp:17-39` | 작은 buffer에서 필요한 길이는 기록하지만 구현은 `errno = EINVAL`으로 끝나므로 `ZLINK_CONFIG_INVALID_ARGUMENT`을 반환한다. SUB/XSUB 이외 type도 `EINVAL`/`ZLINK_CONFIG_INVALID_ARGUMENT`이며 `ENOTSUP` 경로가 없다. |
| A. 문서 누락 | `socket/05-xsub.ko.md:56-57,208-211,258` — 0-기반 index로 구독 filter를 조회한다고만 정의 | `core/src/api/core/zlink_option_subscription_api.cpp:49-57,97-109` | 조회 전에 filter byte열, 그 다음 pattern flag 기준으로 snapshot을 정렬한다. 따라서 `index_`는 등록 순서가 아니라 lexicographic snapshot의 index인데, 관찰 가능한 index 의미가 문서에 없다. |
| A. 문서 누락 | `socket/05-xsub.ko.md:200-217,258-259` — filter 문자열과 실제 길이를 반환한다고만 정의 | `core/src/api/core/zlink_option_subscription_api.cpp:73-82` | 구현은 filter byte 수만 `memcpy`하고 종료 NUL을 쓰지 않는다. `filter_out_`이 C 문자열이 아닌 byte buffer라는 출력 형식이 공개 문서에 없다. |
| A. 문서 누락 | `socket/05-xsub.ko.md:200-211,215` — `is_pattern_out_`을 결과 pointer로 기술 | `core/src/api/core/zlink_option_subscription_api.cpp:81-82,157-169` | `is_pattern_out_`은 `NULL`이어도 성공하며, 제공된 경우에만 값을 쓴다. 선택 output이라는 호출 규칙이 문서에 없다. |
| A. 문서 누락 | `socket/05-xsub.ko.md:21-22,158-192,243-245,262-266` — SUB/XSUB topic-part receive surface로 `zlink_subscribe_part`만 정의·검증 | `core/include/zlink/socket/api.h:572-585`; `core/src/api/socket/socket_retained_part_api.cpp:480-604`; `core/tests/integration/test_retained_hwm_credit.cpp:354-445` | 공개 header/export surface에는 `zlink_subscribe_part_with_hwm_budget_lease`가 있다. 기본 수신 인자에 필수 `lease_out_`을 추가하여 retained HWM credit lease를 반환하는 별도 SUB/XSUB 계약인데, signature·lease ownership/release·오류·buffer retry 규칙이 이 문서에 없다. |
| A. 문서 누락 | `socket/05-xsub.ko.md:36-42,135-145,251-255` — 구독 등록과 해제의 단일 filter 동작만 정의 | `core/src/runtime/sockets/pubsub/xsub.cpp:258-308`; `core/tests/integration/test_xpub_verbose.cpp:150-173` | 같은 filter를 중복 등록하면 XSUB는 reference count를 늘리고, upstream unsubscribe는 마지막 대응 해제 때만 보낸다. 존재하지 않는 filter 해제도 upstream으로 보내지 않는다. XSUB의 upstream 관찰·전달 결과를 바꾸는 중복/미등록 해제 규칙이 빠져 있다. |
| B. 구현 gap | `socket/05-xsub.ko.md:241-245,247-270` — 열거한 각 공개 surface 계약이 unit test 하나로 이어져야 함 | `core/tests/unittest/unittest_typed_option.cpp:299-313`; `core/tests/integration/test_asio_tcp.cpp:301-317`; `core/tests/integration/test_helper_recv_part_basic.cpp:321-380`; `core/tests/integration/test_flow_state_paired.cpp:216-234` | XSUB typed-option test는 count와 snapshot 성공만 확인한다. upstream 전달 test는 `zlink_set_subscription`이 아닌 raw `zlink_send`를 쓰고, `zlink_subscribe_part` buffer-retry test는 SUB만 생성한다. flow-state test도 internal method만 호출한다. XSUB 공개 helper의 upstream 전달, 오류, buffer retry, source RID, monitor 계약을 직접 검증하는 요구된 contract test가 없다. |

## 요확인

- 없음. Socket 공통에 위임한 공통 option·수명·thread 안전성·일반 send/receive ABI 자체와 Auto HWM 계산·admission은 이 감사의 gap으로 중복 계상하지 않았다.
