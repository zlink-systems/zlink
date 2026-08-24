# Errors 스펙-구현 gap 감사

> 감사 도구: codex (gpt-5.6-terra, reasoning high, 정적 대조) · 2026-08-24
> 실행 테스트: 수행하지 않음 (감사 지침)

판정: **구현/문서 gap 9건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았으며, 이 보고서만 작성했다.

## 대조 완료 계약군

- 8개 공개 result enum의 이름·정수값: 일치
- 공개 version macro와 `zlink_version()`의 선언: 불일치 1건 외 일치
- errno 확장 상수의 기존 4개 fallback과 `EFSM`/`ENOCOMPATPROTO`/`ETERM`/`EMTHREAD`: 일치
- receive·handler·bind·connect result 정규화 helper의 기본 분류: 표의 핵심 결과 enum과 대체로 일치
- `zlink_errno()`의 현재 thread errno 반환과 reply callback의 typed-result 선언: 코드상 확인

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `03-errors.ko.md:36-38`, `413-414`, `519-524` — 실패한 공개 함수는 같은 thread의 `zlink_errno()`에 대응 errno를 기록하고 config invalid argument는 `EINVAL`/`EMSGSIZE` | `core/src/api/core/zlink_option.cpp:95-104,120-125` | `zlink_set_option`과 `zlink_get_option`은 option descriptor를 찾지 못하면 `ZLINK_CONFIG_INVALID_ARGUMENT`만 반환하고 `errno`를 설정하지 않는다. 이전 errno가 그대로 남으므로 이 공개 failure가 대응표의 `EINVAL` 또는 `EMSGSIZE`를 반환한다는 계약을 보장하지 못한다. |
| A. 문서 누락 | `03-errors.ko.md:40-67` — 확장 errno 공개 상수를 정의하는 절 | `core/include/zlink_errno.h:15-79` | 문서는 fallback 네 개(`ESTALE`, `EALREADY`, `EDEADLK`, `ESHUTDOWN`)만 열거하지만, 공개 header는 platform 부재 시 `ENOTSUP`부터 `ENETRESET`까지 18개 POSIX errno에도 `ZLINK_HAUSNUMERO + 1..18` fallback을 제공한다. 공개 값·platform 동작이 문서에 없다. |
| C. 문서-코드 모순 | `03-errors.ko.md:239-249` — version `0.12.0` | `core/include/zlink.h:8-14` | 실제 공개 macro와 `zlink_version()`이 기록하는 값은 `0.13.0`이다. |
| C. 문서-코드 모순 | `03-errors.ko.md:322-330` — submit `NOT_CONNECTED`는 `ENOTCONN`, `NOT_ADMITTED`는 `EACCES`, `THREAD_VIOLATION`은 `EDEADLK`/`EPERM`, `ESHUTDOWN`은 `INVALID_STATE` | `core/src/api/message/submit_result_internal.hpp:22-54`; `core/src/runtime/core/internal_errno.hpp:199-220` | 실제 정규화는 `EHOSTUNREACH`도 `NOT_CONNECTED`, `ECONNREFUSED`도 `NOT_ADMITTED`, `EMTHREAD`도 `THREAD_VIOLATION`으로 보낸다. 반대로 `ESHUTDOWN`은 runtime failure로 분류되어 `TERMINATED`가 된다. 대응표가 실제 분류를 완전하고 단일하게 나타내지 못한다. |
| C. 문서-코드 모순 | `03-errors.ko.md:344-354` — request `NOT_CONNECTED`는 `ENOTCONN`, `INVALID_ARGUMENT`은 `EINVAL` | `core/src/api/message/request_result_internal.hpp:52-75` | 구현은 `EHOSTUNREACH`도 `NOT_CONNECTED`, `EFAULT`도 `INVALID_ARGUMENT`으로 정규화한다. 표의 errno 집합이 실제 callback result 변환보다 좁다. |
| C. 문서-코드 모순 | `03-errors.ko.md:413-416` — `EINVAL`은 `CONFIG_INVALID_ARGUMENT` 및 `CONFIG_INVALID_STATE` 양쪽에 대응 | `core/src/api/core/config_result_internal.hpp:20-32` | 실제 helper는 `EINVAL`을 오직 `ZLINK_CONFIG_INVALID_ARGUMENT`으로 정규화한다. 한 errno가 두 공개 result 행에 중복되어 있고 `INVALID_STATE` 행은 구현과 다르다. |
| C. 문서-코드 모순 | `03-errors.ko.md:280-285`, `542-545` — `zlink_strerror()`가 library-owned static storage를 반환하며 계속 사용 가능 | `core/src/api/core/context_api.cpp:86-89`; `core/src/runtime/utils/err.cpp:7-49` | 알려진 zlink 확장 errno 일부 외에는 libc `strerror(errnum_)`의 반환 pointer를 그대로 노출한다. 이는 zlink 소유 storage가 아니며, 문서가 보장한 pointer 수명도 구현만으로 보장되지 않는다. |
| D. 구현 서술 낡음 | `03-errors.ko.md:443-444` — result enum 값이 `0-706` 범위에서 전역적으로 고유 | `core/include/zlink_errno.h:97,125,144,159,171,181,192,208-217` | 모든 enum의 `*_OK`가 `0`이라 이미 중복이고, configuration enum에는 `707-709`도 있다. 내부 구조 절의 값 범위·전역 고유성 서술이 현재 header와 다르다. |
| D. 구현 서술 낡음 | `03-errors.ko.md:446-447` — reply callback이 raw `errno_`를 전달 | `core/include/zlink/socket/api.h:155-158`; `core/src/api/socket/request_reply_protocol_internal.hpp:338-346` | 공개 callback signature의 첫 인자는 이미 `zlink_request_result_t`이고, callback 직전 `from_errno`로 정규화한다. raw errno를 전달한다는 구현 서술은 현재 코드와 반대다. |
| D. 구현 서술 낡음 | `03-errors.ko.md:491-497` — request result가 13개 값이며 `BACKPRESSURED`를 열거하지 않음 | `core/include/zlink_errno.h:123-139`; `core/src/api/message/request_result_internal.hpp:39-40,71-73` | 공개 enum은 `OK`를 포함해 14개이고 `ZLINK_REQUEST_BACKPRESSURED=113`을 포함한다. helper도 `EAGAIN`/`ENOBUFS`를 그 값으로 변환한다. |

## 요확인

- `03-errors.ko.md:298-302`, `542-545`의 `zlink_version()` null output pointer 처리 및 진단 함수들의 "thread-safe" 보장은 구현이 포인터를 무조건 역참조하고(`core/src/api/core/context_api.cpp:79-84`), `zlink_strerror()`는 platform libc `strerror`에 위임한다. 현재 스펙은 null pointer와 지원 platform별 libc thread-safety의 전제조건을 정의하지 않으므로, 정적 코드만으로 별도 구현 gap으로 확정하지 않았다.
