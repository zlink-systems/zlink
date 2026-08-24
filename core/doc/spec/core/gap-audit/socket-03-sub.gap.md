# Socket — SUB 스펙-구현 gap 감사

> 감사 도구: codex (GPT-5, 정적 코드 대조) · 2026-08-24
> 검증 범위: `core/doc/spec/core/socket/03-sub.ko.md`, `core/include/`, `core/src/`, 관련 `core/tests/` 표본. 실행 테스트 없음.

판정: **구현/문서 gap 6건, 요확인 0건**. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- `zlink_sub_option_t` ABI와 `ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400`, `int` 조회 경로: 일치
- SUB/XSUB 전용 `zlink_set_sub_option`·`zlink_get_sub_option` signature와 공통 option 분리: 일치
- 구독 등록·해제의 NUL 종료 filter 처리, byte-prefix 매칭, 빈 filter 및 literal `*`: 일치
- `zlink_subscribe_part` signature, SUB/XSUB type 제한, source RID의 `NULL` 결과, topic buffer 부족 재시도와 multipart 같은-thread 진행: 일치
- SUB의 Auto HWM `recv_ingress` 분류와 receive-flow 미지원·monitor flow-detail 제외: 일치. Auto HWM 계산·admission 자체는 대상 스펙이 위임한 `systems/06-auto-hwm.ko.md`의 감사 범위다.

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `socket/03-sub.ko.md:238-240,248` — `zlink_subscription_at`의 작은 buffer는 필요한 길이와 `ZLINK_CONFIG_BUFFER_TOO_SMALL`/`ENOBUFS`를 반환 | `core/src/api/core/zlink_option_subscription_api.cpp:73-76`; `core/src/api/core/config_result_internal.hpp:19-33` | 구현은 필요한 길이는 기록하지만 `errno = EINVAL`으로 종료한다. 따라서 반환값도 `ZLINK_CONFIG_INVALID_ARGUMENT`이며, 스펙의 `ENOBUFS`/`ZLINK_CONFIG_BUFFER_TOO_SMALL` 계약과 다르다. 부분 write 및 `is_pattern_out_` 불변은 구현과 일치한다. |
| C. 문서-코드 모순 | `socket/03-sub.ko.md:244,248-249` — 구독 조회를 지원하지 않는 handle type은 `ENOTSUP` | `core/src/api/core/zlink_option_subscription_api.cpp:160-165`; `core/src/api/core/config_result_internal.hpp:12-21` | SUB/XSUB 이외 socket은 `errno = EINVAL`, `ZLINK_CONFIG_INVALID_ARGUMENT`으로 반환한다. 구현에 `ENOTSUP`을 설정하거나 `ZLINK_CONFIG_NOT_SUPPORTED`로 정규화하는 경로가 없다. |
| A. 문서 누락 | `socket/03-sub.ko.md:47-49,233-236` — index로 개별 filter를 읽는다는 설명만 있음 | `core/src/api/core/zlink_option_subscription_api.cpp:49-57,97-109` | 조회 전 snapshot을 filter byte열, 그 다음 pattern flag 기준으로 정렬한다. 따라서 `index_`는 등록 순서가 아니라 lexicographic 순서의 snapshot index다. 관찰 가능한 index 의미가 문서에 없다. |
| A. 문서 누락 | `socket/03-sub.ko.md:233-240` — filter 문자열과 실제 길이를 반환한다고만 설명 | `core/src/api/core/zlink_option_subscription_api.cpp:73-82` | 구현은 `entry_.filter.size()` byte만 `memcpy`하고 종료 NUL을 쓰지 않는다. `filter_out_`을 C 문자열로 사용할 수 없는 출력 형식이 문서에 명시돼 있지 않다. |
| A. 문서 누락 | `socket/03-sub.ko.md:225-240` — `is_pattern_out_`을 결과 pointer로 서술 | `core/src/api/core/zlink_option_subscription_api.cpp:81-82,157-169` | `is_pattern_out_`은 `NULL`이어도 성공하며, 제공된 경우에만 값을 쓴다. 선택 output이라는 공개 호출 규칙이 문서에 없다. |
| A. 문서 누락 | `socket/03-sub.ko.md:24-26,185-217` — SUB/XSUB topic part 수신 공개 surface로 `zlink_subscribe_part`만 정의 | `core/include/zlink/socket/api.h:572-585`; `core/src/api/socket/socket_retained_part_api.cpp:480-604`; `core/tests/integration/test_retained_hwm_credit.cpp:354-445` | 공개 header와 export surface에는 `zlink_subscribe_part_with_hwm_budget_lease`가 있다. 이는 기본 수신 인자에 필수 `lease_out_`을 추가하고 retained HWM credit lease를 반환하는 별도 SUB/XSUB 수신 계약인데, 함수 signature·lease ownership/release·오류 및 buffer-retry 규칙이 이 문서에 없다. |

## 요확인

- 없음. 문서가 Socket 공통에 위임한 수명·스레드 안전성·공통 수신 모델, 그리고 Auto HWM 계산·admission은 이 감사의 gap으로 중복 계상하지 않았다.
