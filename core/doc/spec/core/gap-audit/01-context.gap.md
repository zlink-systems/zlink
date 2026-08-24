# Context 스펙–구현 gap 감사

> 감사 도구: codex (gpt-5.6-terra, reasoning high, read-only) · 2026-08-24
> 감독 검증: BLOCKY·ctx_new errno·get_data EINVAL 3건 코드 표본 대조 일치 확인

판정: **구현/문서 gap 8건, 요확인 1건**. 코드·문서는 수정하지 않았고, 실행 테스트 없이 정적 대조만 수행했다.

## 대조 완료 계약군

- 공개 ABI signature, option/profile enum 값, 기본값: 일치
- `new`/`term`/`shutdown` 반환 타입과 invalid-handle 결과, `term`의 `EINTR` 재시도 경로: 대체로 일치
- Auto HWM `uint64_t` option의 전용 set/get 경로와 exact-size 조회 규칙: 대체로 일치
- `ZLINK_SOCKET_LIMIT`/`ZLINK_THREAD_PRIORITY` 값 `3` 충돌 시 전자를 우선 해석하는 동작: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `01-context.ko.md:153-159`, `343` — `zlink_ctx_new` 실패 시 `NULL`과 설정된 errno | `core/src/api/core/context_api.cpp:101-115` | `new (std::nothrow)`가 `NULL`을 반환하는 경로와 `ctx->valid()` 실패 경로에서 errno를 설정하지 않는다. 앞선 네트워크 초기화가 errno를 설정한다는 보장도 없다. 실패 시 errno 설정 계약을 보장하지 못한다. |
| C. 문서-코드 모순 | `01-context.ko.md:57-59`, `94` — `ZLINK_CTX_OPT_BLOCKY`가 종료 blocking 동작 제어 | `core/src/runtime/core/ctx_termination.cpp:39-98`, `core/src/runtime/sockets/common/socket_base.cpp:129-131` | context 종료는 `_blocky`와 무관하게 reaper 완료를 기다린다. 실제 `_blocky=0` 효과는 이후 생성 socket의 기본 `LINGER`를 `0`으로 설정하는 것이다. |
| C. 문서-코드 모순 | `01-context.ko.md:226-230` — Auto HWM enable 변경이 기존 socket에 "즉시" 반영 | `core/src/runtime/core/ctx_options.cpp:37-42,128-130`, `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:68-95`, `core/include/zlink/core/api.h:48-50` | 기본 debounce는 3000ms이며, 변경은 재계산을 예약한다. debounce가 `0`일 때만 호출 중 즉시 재계산된다. |
| B. 구현 gap | `01-context.ko.md:270-273` — explicit memory limit/Core budget이 기존 reservation과 자동 하한을 수용 못 하면 `ENOBUFS` | `core/src/runtime/core/ctx_auto_hwm_state.cpp:160-184`, `core/src/runtime/core/ctx_options.cpp:64-95,133-134` | 구현은 configured 값과 detected hard limit만 비교하며, 실패 시 `EINVAL`으로 귀결된다. 현 경로에는 manual reservation·자동 하한 검증이나 `ENOBUFS` 반환이 없다. |
| C. 문서-코드 모순 | `01-context.ko.md:264-266` — thread-name prefix는 null-terminated 문자열, `strlen(prefix)+1`, 최대 16 byte | `core/src/api/core/context_api.cpp:156-166`, `core/src/runtime/core/ctx_thread.cpp:75-86` | 구현은 길이 `1..16`인 임의 byte buffer를 받아 그대로 저장하며, 마지막 byte가 `'\0'`인지 검증하지 않는다. |
| C. 문서-코드 모순 | `01-context.ko.md:302-304` — `zlink_ctx_get_data`의 invalid output pointer는 `EFAULT` | `core/src/api/core/context_api.cpp:170-180` | 유효한 context라도 `optval_` 또는 `optvallen_`이 null이면 구현은 `ZLINK_CONFIG_INVALID_ARGUMENT`과 `EINVAL`을 반환한다. |
| C. 문서-코드 모순 | `01-context.ko.md:264-266` — prefix는 string data 경로로만 기술 | `core/src/api/core/context_api.cpp:34,145-153,210-232`, `core/src/runtime/core/ctx_thread.cpp:75-85,108-116` | 공개 API는 `zlink_ctx_set(..., ZLINK_THREAD_NAME_PREFIX, int)`도 허용하고, `zlink_ctx_get`은 저장 문자열을 `atoi`로 반환한다. 이 legacy int round-trip 동작은 문서의 string-only 설명과 맞지 않는다. |
| A. 문서 누락 | `01-context.ko.md:320-327` — `*error_out_` 기록만 기술 | `core/src/api/core/context_api.cpp:212-231`; `core/tests/integration/test_ctx_options.cpp:85-88,130-132` | `error_out_`는 선택적이다. null이어도 `zlink_ctx_get`은 값을 반환하거나 errno만 설정한다. null 허용 및 그 결과 동작이 공개 문서에 없다. |
| C. 문서-코드 모순 | `01-context.ko.md:226-229` — socket 생성 전·후 context 구성 가능, I/O thread 수·socket 상한은 context 옵션 | `core/src/runtime/core/ctx_options.cpp:21-34`, `core/src/runtime/core/ctx_bootstrap.cpp:17-28`, `core/src/runtime/core/ctx_runtime_resources.cpp:27-40,131-151`, `core/src/runtime/core/ctx_socket_registry.cpp:10-33,53-62` | 첫 socket 생성으로 runtime이 시작된 뒤에도 `ZLINK_IO_THREADS`와 `ZLINK_MAX_SOCKETS` set은 성공하고 조회값도 바뀐다. 그러나 실제 I/O thread pool과 socket-slot pool은 시작 시 한 번만 생성되므로 런타임 용량은 바뀌지 않는다. |

## 요확인

- `01-context.ko.md:44-45,185-187,356-357`은 모든 Context API의 동시 호출 안전성을 보장한다. 그러나 `zlink_ctx_term`은 `core/src/runtime/core/ctx.cpp:153-176`에서 객체를 해제하고, 다른 API는 `core/src/api/core/context_api.cpp:117-119,218-222`에서 raw handle을 역참조한다. `term` 진행 중 다른 API 호출의 lifetime race는 정적 검토만으로 안전성을 확정할 수 없다. 동시 `term`/`get`/`shutdown` stress와 ASan 검증이 필요하다.
