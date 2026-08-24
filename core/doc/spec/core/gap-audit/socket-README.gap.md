# Socket 공통 스펙-구현 gap 감사

> 감사 도구: codex (GPT-5, 정적 대조) · 2026-08-24
> 검증 범위: `core/include/`, `core/src/`, 필요한 `core/tests/` 표본. 실행 테스트 없음.

판정: **구현/문서 gap 14건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- socket type·send/recv/part·submit/request result enum 값: 일치
- 기본 HWM·send/receive timeout·submit retry 기본값, HWM의 `uint64_t` exact-size 처리: 일치
- `zlink_socket`, `zlink_recv_part`, bind/connect/disconnect와 receive-flow API의 signature 및 주요 결과: 대체로 일치
- retained-credit receive·lease release, HWM admission과 completion-lane의 기본 구조: 대체로 일치
- async send의 ownership 이동, pending 상한 적용, 완료 순서 및 callback 직렬화: 대체로 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `socket/README.ko.md:95-109` — `ZLINK_SOCKET_ANY`는 filter API의 전체 type wildcard | `core/include/zlink_enum.h:41-52`, `core/src/api/socket/socket_api_internal.hpp:71-92` | 공개 enum 외에는 `core/include/`·`core/src/`에 이 상수를 소비하는 filter API가 없고, 공개 socket 생성 변환은 기본 분기에서 type을 거절한다. 문서가 약속한 filter 용도가 구현되어 있지 않다. |
| A. 문서 누락 | `socket/README.ko.md:395-445` — 공통 option enum 전체를 열거 | `core/include/zlink_enum.h:106-110`, `core/src/runtime/core/options.cpp:116-120`, `core/src/runtime/core/options_core_socket.cpp:121-140` | 공개 `ZLINK_OPT_SEND_PENDING_MAX_MSGS=0x303A`와 `ZLINK_OPT_SEND_PENDING_MAX_BYTES=0x303B`가 문서 enum에서 빠졌다. 둘은 `uint64_t`, 0 거절, 기본값 각각 1024와 4,096,000 byte인 async-send pending 상한이다. |
| C. 문서-코드 모순 | `socket/README.ko.md:438` — `ZLINK_OPT_ZMP_METADATA`는 binary metadata | `core/src/runtime/core/options_core_socket.cpp:146-147,305-308` | 구현은 binary 값을 받거나 반환하지 않고 `int` 0/1 strict boolean으로 ZMP metadata attachment를 켜고 끈다. option의 타입·의미가 다르다. |
| C. 문서-코드 모순 | `socket/README.ko.md:440,448-449` — 제거 전 discovery route value size option은 read-only 공통 option이며 raw socket과 discovery에 적용 | `core/src/api/core/zlink_option_mapping.cpp:75`, `core/src/api/core/zlink_option.cpp:128-138` | descriptor가 socket에서 unsupported로 표시되어 `zlink_get_option`도 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`으로 거절한다. 현 option target 해석도 socket만 처리한다. 문서의 read-only 조회 표면이 구현에 없다. |
| C. 문서-코드 모순 | `socket/README.ko.md:451-453` — socket option API의 `ZLINK_OPT_BLOCKY`는 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP` | `core/src/api/core/zlink_option_mapping.cpp:53`, `core/src/runtime/core/options_owner.cpp:69-71`, `core/src/api/core/zlink_option.cpp:107-114` | public mapping에는 있지만 owner가 unknown이라 socket `setsockopt`가 invalid option으로 귀결된다. 문서가 정한 not-supported 결과/errno를 보장하지 않는다. |
| C. 문서-코드 모순 | `socket/README.ko.md:501-506` — retry 예산 소진은 `ZLINK_SUBMIT_BACKPRESSURED`/`EAGAIN` | `core/src/runtime/sockets/common/socket_base_msg.cpp:288-350`, `core/src/api/message/submit_result_internal.hpp:22-31` | retry loop는 마지막 연결 오류를 복원한다. 따라서 `ENOTCONN`/`EHOSTUNREACH`는 `ZLINK_SUBMIT_NOT_CONNECTED`, `ECONNREFUSED`는 `ZLINK_SUBMIT_NOT_ADMITTED`로 정규화되며, 예산 소진을 일률적으로 `EAGAIN`으로 바꾸지 않는다. |
| C. 문서-코드 모순 | `socket/README.ko.md:328,345-348` — `terminal_errno`는 `ZLINK_SEND_TERMINAL` 사유 | `core/src/runtime/sockets/common/socket_send_complete.cpp:161-177,215-222` | async timeout도 `ZLINK_SEND_TIMED_OUT`와 `ETIMEDOUT`을 completion의 `terminal_errno`에 기록한다. ADMITTED가 아닌 모든 result에 값을 싣는 구현과 문서의 TERMINAL 한정 설명이 다르다. |
| C. 문서-코드 모순 | `socket/README.ko.md:710-714` — raw socket의 routing ID 설정 가능 | `core/src/api/core/zlink_option.cpp:141-151`; `core/tests/unittest/unittest_typed_option.cpp:293` | raw `STREAM`도 raw socket이지만 API가 명시적으로 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`으로 거절한다. 문서는 STREAM 예외와 실제 결과를 적지 않는다. |
| C. 문서-코드 모순 | `socket/README.ko.md:753-754,776-777` — TLS setter는 지원 type 이외에 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP` | `core/src/api/core/zlink_option.cpp:183-229`, `core/src/runtime/core/options_protocol_metadata.cpp:7-51` | 두 setter는 socket type/server-client role을 검사하지 않고 TLS option들을 그대로 설정한다. TLS build가 아니면 protocol option도 구현되지 않아 invalid option 경로가 된다. 문서의 type별 not-supported 결과를 구현이 제공하지 않는다. |
| A. 문서 누락 | `socket/README.ko.md:335-340,918-1023` — async options와 submit 계약 | `core/include/zlink/socket/api.h:140-153`, `core/src/runtime/sockets/common/socket_send_complete.cpp:552-567,681-701` | `struct_size == sizeof(zlink_send_async_options_t)` 필수, `timeout_ms == 0`은 deadline 없음, `op_id_out_`은 선택적이며 제공되면 실패 시 0으로 초기화한다는 공개 동작이 문서에 없다. |
| A. 문서 누락 | `socket/README.ko.md:979-981,1045-1056` — routed target 선택은 ROUTER/DEALER만 설명 | `core/src/api/socket/socket_message_send_api.cpp:538-563`, `core/src/runtime/sockets/common/socket_send_complete.cpp:599-620` | `zlink_select_routed_submit_target`은 STREAM도 지원하며, STREAM async send는 non-NULL exact target을 요구한다. 문서에는 STREAM target 선택·필수 조건이 없다. |
| A. 문서 누락 | `socket/README.ko.md:608-620` — callback 중 close의 일반 lifecycle 규칙 | `core/src/api/core/zlink.cpp:143-151` | raw STREAM message/packet callback 안의 self-close는 deferred close가 아니라 `ZLINK_CLOSE_BUSY`/`EBUSY`다. STREAM 수신 callback의 명시적 예외가 `zlink_close` 계약에 없다. |
| A. 문서 누락 | `socket/README.ko.md:1154-1169` — monitor open은 `events`만 설명 | `core/include/zlink/eventing/api.h:71-76`, `core/src/api/monitoring/monitor_socket_api.cpp:63-68,136-145` | `options_->monitor_hwm_bytes`가 0이면 Core default, 양수면 exact byte HWM을 설정하는 공개 입력인데 문서에 없다. |
| D. 구현 서술 낡음 | `socket/README.ko.md:1212-1217` — monitor status ABI version 3 | `core/include/zlink/eventing/api.h:69,182-200`, `core/src/runtime/sockets/common/socket_base_monitor.cpp:44-51,135-140` | 현재 ABI는 4이며 receive-flow 상태 detail flag와 다섯 flow metric field를 추가한다. 내부 구조 절의 ABI version 및 제공 field 설명이 낡았다. |

## 요확인

- `socket/README.ko.md:44-57,608-620`은 같은 handle의 public socket API 동시 호출 안전성을 폭넓게 보장한다. lifecycle coordinator와 control-path lock이 존재하지만, opaque handle 해제 직전의 동시 API entry가 모두 안전하게 pin되는지는 정적 읽기만으로 확정하지 못했다. close/send/option/monitor 교차 stress 및 ASan/TSan 검증이 필요하다.
