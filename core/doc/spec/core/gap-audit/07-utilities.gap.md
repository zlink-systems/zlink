# Utilities 스펙-구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 실행 테스트 미실행) · 2026-08-24
> 범위: `core/doc/spec/core/07-utilities.ko.md`와 `core/include/`, `core/src/`, 관련 `core/tests/` 표본

판정: **구현/문서 gap 7건, 요확인 1건**. 코드와 스펙 문서는 수정하지 않았으며, 이 보고서만 작성했다.

## 대조 완료 계약군

- 공개 ABI signature: atomic counter, timer, stopwatch, capability, proxy, sleep, thread helper 모두 일치
- atomic counter의 초기값, `inc`의 이전 값 반환, `dec`의 0 도달 결과, destroy 뒤 handle null 처리: 일치
- timer 생성 실패의 `NULL`/errno, 반복 횟수 `0`과 양수의 기본 동작, recv/handler 상호 배타와 `EAGAIN`/`EBUSY` 결과: 대체로 일치
- stopwatch의 microsecond 측정 및 stop에서 handle 해제, capability의 `tcp`·조건부 `ipc`/`tls`/`ws`/`wss` 판정: 일치
- proxy handle의 borrowed ownership, 필수·선택 handle 검증, capture 복사와 loop blocking: 대체로 일치
- sleep과 thread의 ABI 및 join이 대상 종료를 기다린 뒤 handle을 해제하는 경로: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `07-utilities.ko.md:51-55,510-516` — `zlink_atomic_counter_new`는 메모리 부족 시 `NULL`을 반환 | `core/src/api/core/zlink_utils.cpp:68-72`; `core/src/runtime/utils/err.hpp:137-144` | `new (std::nothrow)`가 `NULL`을 반환하면 `alloc_assert`가 즉시 `zlink_abort`를 호출한다. 호출자에게 `NULL`을 반환하는 실패 경로가 없다. |
| C. 문서-코드 모순 | `07-utilities.ko.md:321-327` — `zlink_stopwatch_start`는 실패 시 `NULL`을 반환 | `core/src/api/core/zlink_utils.cpp:28-34`; `core/src/runtime/utils/err.hpp:137-144` | `malloc` 실패도 `alloc_assert`로 process abort가 된다. `NULL` 반환 계약과 다르다. |
| C. 문서-코드 모순 | `07-utilities.ko.md:477-483,545-546` — `zlink_thread_start`는 실패 시 `NULL`을 반환 | `core/src/api/core/zlink_utils.cpp:50-55`; `core/src/runtime/core/thread.cpp:231-240`; `core/src/runtime/utils/err.hpp:115-123,137-144` | thread handle 할당 실패는 `alloc_assert`, POSIX `pthread_create` 실패는 `posix_assert`로 abort한다. 성공 handle 또는 `NULL`이라는 공개 반환 계약을 제공하지 않는다. |
| C. 문서-코드 모순 | `07-utilities.ko.md:161-164,176-177,269-270,524` — fire count는 누적 횟수 | `core/src/api/monitoring/timer_api.cpp:153-161`; `core/src/api/monitoring/timer_scheduler_backend.cpp:103-127` | `zlink_timer_start`는 매번 `next_fire_count`를 `1`로 재설정한다. 같은 timer를 stop한 뒤 다시 시작하면 callback/recv가 lifetime 누적값이 아니라 새 실행의 1부터 받는다. |
| A. 문서 누락 | `07-utilities.ko.md:229-236,520-521` — interval의 단위와 repeat 규칙만 정의 | `core/src/api/monitoring/timer_api.cpp:140-148` | `interval_ns_ == 0`은 `ZLINK_CONFIG_INVALID_ARGUMENT` 및 `EINVAL`이다. 0이 유효하지 않다는 입력 경계와 결과/errno가 공개 문서에 없다. |
| B. 구현 gap | `07-utilities.ko.md:426-429,540` — control socket의 `PAUSE`는 pause, `RESUME`는 resume 명령 | `core/src/runtime/sockets/proxy/proxy.cpp:183-188` | `PAUSE` 분기는 size 5 입력에서 `memcmp`의 비영(불일치) 결과를 조건으로 삼아 상태를 `active`로 두며, 정상 `PAUSE` 문자열은 pause하지 않는다. 반대로 `RESUME`은 상태를 `paused`로 둔다. 두 명령의 공개 동작 계약을 만족하지 못한다. |
| A. 문서 누락 | `07-utilities.ko.md:427,431-432,540` — `STATISTICS` command 지원과 reply ownership만 정의 | `core/src/runtime/sockets/proxy/proxy.cpp:156-180` | 구현은 frontend/backend 각각의 receive/send message 수·byte 수를 순서대로 담은 `uint64_t` 8-frame reply를 보낸다. frame 수, 값의 의미, 순서, byte encoding이 문서에 없어 binding과 caller가 reply를 해석할 공개 계약이 없다. |

## 요확인

- `07-utilities.ko.md:426-436`은 frontend와 backend에 raw socket handle만 요구하며 서로 다른 handle이라는 제약을 두지 않는다. 그러나 `ZLINK_HAVE_POLLER` build에서 control을 함께 주고 두 handle이 같으면 `proxy_steerable`은 `poller_send_blocked` 등을 `NULL`로 남긴 채 control을 등록하려 dereference한다 (`core/src/runtime/sockets/proxy/proxy.cpp:241-260,318-342`). 해당 조합을 허용할지 명시적으로 거절할지는 문서만으로 확정하기 어려우며, 해당 build 설정의 focused 재현으로 crash 여부와 의도된 precondition을 확인해야 한다.
