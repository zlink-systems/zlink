# Socket — STREAM 스펙–구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 테스트 미실행) · 2026-08-24
> 대조 범위: `core/include/`, `core/src/`, 필요한 `core/tests/` 정적 표본

판정: **구현/문서 gap 9건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았고, 지정된 이 보고서만 작성했다. 테스트는 실행하지 않았다.

## 대조 완료 계약군

- `ZLINK_SOCKET_STREAM` 생성, 4 byte routing ID 부여, bind 전용과 `zlink_connect()`의 `EOPNOTSUPP`: 일치
- 공개 ABI signature와 `ZLINK_STREAM_OPT_NOTIFY = 0x3501`, STREAM 전용 typed option의 socket-type 검증: 일치
- routed single-part send의 `ZLINK_PART_MORE` 거부, `EAGAIN`일 때만 payload 보존, 미연결 target의 결과 매핑: 일치
- raw part receive의 필수 output, source RID의 TLS borrowed view, part ownership, `ZLINK_DONTWAIT` 결과: 일치
- raw/packet callback의 STREAM 전용성, handler 재등록 및 callback 안 self-close의 `EBUSY`: 일치
- receive-flow-state의 `ENOTSUP`, flow-state monitor detail/event 비발생, 4 byte RID disconnect: 일치
- packet state의 per-pipe 누적·move 전달, per-source-RID 직렬화, oversize/불완전 packet의 callback 미전달: 대체로 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| A. 문서 누락 | `08-stream.ko.md:43-59` — `ZLINK_STREAM_OPT_NOTIFY`의 허용값만 설명하고 기본값은 명시하지 않음 | `core/src/runtime/core/options.cpp:122-127`; `core/src/runtime/core/options_core_socket.cpp:299-302` | 새 socket의 `stream_notify` 기본값은 `false`이며 get은 이를 `0`으로 노출한다. 공개 option의 기본값이 문서에 없다. |
| B. 구현 gap | `08-stream.ko.md:45,57-59,401-402` — bind 전 `NOTIFY=1`이면 연결·해제를 source RID를 가진 길이 0 data record로 수신 | `core/src/runtime/core/options_core_socket.cpp:143-144,299-302`; `core/src/runtime/sockets/stream/stream.cpp:274-287,1006-1024`; `core/src/api/core/zlink_option.cpp:38-55`; `core/src/runtime/sockets/common/socket_base_api.cpp:366-402` | `stream_notify`의 Core 참조는 set/get뿐이며 STREAM attach는 monitor용 connection-ready event만 낸다. 수신 경로는 이 값을 읽어 0-byte record를 만들지 않는다. 또한 option set 경로에는 bind 완료 여부 검사가 없어 "bind 전" 제약도 강제하지 않는다. |
| B. 구현 gap | `08-stream.ko.md:67-78,404-410` — 첫 `zlink_recv_part()`가 raw-part 모드를 고정하고 이후 handler 등록은 `EBUSY` | `core/src/api/socket/socket_message_api.cpp:23-160`; `core/src/api/socket/socket_message_handler_api.cpp:11-54`; `core/src/runtime/sockets/stream/stream_dispatch_lifecycle.cpp:33-80` | `zlink_recv_part()`는 STREAM dispatch mode/state를 설정하거나 검사하지 않는다. 반면 두 handler 등록 API는 `_dispatch_active`일 때만 거부하므로, raw part receive를 먼저 성공시킨 handle에는 뒤이어 raw 또는 packet handler를 등록할 수 있다. |
| A. 문서 누락 | `08-stream.ko.md:91-109` — routed raw data part 전송과 실패 소유권만 정의하며 길이 0 part의 송신 의미는 없음 | `core/src/runtime/sockets/stream/stream.cpp:370-407` | target RID가 있는 STREAM 송신에서 `msg_->size() == 0`은 byte record를 전송하지 않고 `pipe_t::terminate(false)`로 peer 종료를 요청한다. empty part의 공개적으로 관찰 가능한 종료 의미가 문서에 없다. |
| B. 구현 gap | `08-stream.ko.md:177-181,189-191` — `header_size == 0 && body_size == 0`인 정확한 빈 packet도 non-NULL 두 message로 callback을 유발 | `core/src/runtime/sockets/stream/stream.cpp:830-881,900-938`; `core/tests/integration/test_stream_socket.cpp:1999-2073` | parser의 outer loop 조건은 `offset < payload_size`다. 정확히 6 byte prefix가 입력의 끝이면 zero size를 판독한 뒤 `body_stage`로만 전환하고 loop를 끝내므로 callback을 호출하지 않는다. 정적 test도 이 6-byte frame 뒤 callback을 기대하지만, 다음 byte가 들어오기 전에는 dispatch되지 않는다. |
| C. 문서-코드 모순 | `08-stream.ko.md:182-183,201-205` — 양수 `maxmsgsize`에서 한계를 넘는 선언된 `header_size` 또는 `body_size`만 malformed | `core/src/runtime/sockets/stream/stream.cpp:847-860` | 구현은 각 길이뿐 아니라 `header_size + body_size > maxmsgsize`도 거부한다. 예를 들어 maxmsgsize가 4일 때 header 3, body 3은 문서의 각-size 기준을 통과하지만 코드에서는 disconnect된다. |
| D. 구현 서술 낡음 | `08-stream.ko.md:311-315` — WS/WSS는 gather write를 지원하지 않고 `supports_gather_write()`가 false | `core/src/runtime/transports/ws/ws_transport.hpp:70-81`; `core/src/runtime/transports/tls/wss_transport.hpp:78-89`; `core/src/runtime/transports/ws/ws_transport.cpp:269-294` | WS와 WSS transport 모두 `supports_gather_write()`에서 `true`를 반환하며, 두 buffer를 묶어 `async_write`하는 `async_writev()`를 구현한다. 내부 구조 설명이 현재 구현과 반대다. |
| D. 구현 서술 낡음 | `08-stream.ko.md:357-365,376-386` — STREAM/TCP speculative write는 상시 on의 env 비제어 고정값이며 현행 STREAM env 목록에 해당 제어가 없음 | `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:57-60,88-95,115-123` | `ZLINK_ASIO_STREAM_ASYNC_WRITE`가 설정되면 STREAM/TCP speculative write를 끈다. 따라서 "상시 on 고정" 및 env 비제어 설명이 사실이 아니고, 현행 STREAM env 목록도 불완전하다. |
| A. 문서 누락 | `08-stream.ko.md:376-386` — "현재 유지되는 STREAM 런타임 환경변수" 목록 | `core/src/runtime/core/session_base.cpp:29-35,364-370` | `ZLINK_STREAM_PIPE_LWM_HINT`(기본 4)는 STREAM application pipe의 low-water-mark hint를 `값 * 1024` byte로 설정한다. 현재 목록에 없는 STREAM runtime 환경변수·기본값·효과다. |

## 요확인

- `08-stream.ko.md:300-309`의 TCP/WS/WSS 처리량 수치는 특정 벤치마크 머신의 실행 결과다. 정적 코드 대조로는 1493/696/382 MB/s와 "64KB 이상에서 TCP line rate 근접"을 확인할 수 없다. 문서가 가리키는 동일한 머신·payload·TLS 설정으로 benchmark를 재현해야 한다.
