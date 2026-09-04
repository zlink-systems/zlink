# Bucket B — C++ stream connector bind 실패 조사

## 결론

`test_cpp_stream_connector`와 `connector_perf_smoke`의 loopback server fixture가
Core STREAM socket의 receive mode를 지정하지 않고 bind했다. Core 0.17.0의 STREAM 계약은
첫 bind 또는 connect 전에 `RAW`나 `PACKET`을 명시하도록 요구하므로, Core가
`ZLINK_BIND_INVALID_ARGUMENT`(501)과 `EINVAL`(22)을 반환한 동작은 계약에 맞다.

두 fixture는 `recv()`로 framing 전의 raw byte record를 읽는다. 따라서 C++ framework의
connector test와 perf fixture가 이 실패를 소유한다. Core, transport, 실제 framework STREAM
host에는 변경이 필요하지 않다.

## bind sequence와 관찰 결과

수정 전 첫 실패 sequence는 다음과 같았다.

```text
zlink::stream_socket_t(context)
  -> receive mode = UNSPECIFIED (default)
  -> notify(false)
  -> bind("tcp://127.0.0.1:0")
  -> ZLINK_BIND_INVALID_ARGUMENT (501), errno=EINVAL (22)
```

`ZLINK_CPP_MESH_TRACE=1 ZLINK_CPP_STREAM_TRACE=1`로 단독 CTest를 실행하면 connector의
앞선 test 구간은 완료되고 첫 Core STREAM loopback server를 만들 때 process가 종료됐다.
gdb breakpoint와 throw backtrace는
[`test_cpp_stream_connector.cpp`](../../../framework/languages/cpp/tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector.cpp#L1391)의
socket type이 `zlink::stream_socket_t`, bind endpoint가 `tcp://127.0.0.1:0`, bind 직전
`server.options().recv_mode()`가 `unspecified`임을 확인했다. Throw stack은
`main`의 해당 bind 호출에서 `zlink::socket_t::bind()`로 직접 이어졌다.

Endpoint가 `tcp://`이고 receive mode 검증에서 side effect 없이 실패했으므로 ws, wss, TLS
option 순서와 transport 구현은 이 실패에 관여하지 않았다.

## 계약 판정

[`08-stream.ko.md` §2](../../../core/doc/spec/core/socket/08-stream.ko.md#2-생성-bind와-option)는
receive mode의 기본값을 `UNSPECIFIED`로 정하고, mode를 선택하지 않은 bind가 endpoint side
effect 없이 `ZLINK_BIND_INVALID_ARGUMENT`과 `EINVAL`로 실패한다고 규정한다. 같은 문서의
[§3](../../../core/doc/spec/core/socket/08-stream.ko.md#3-수신-모드)는 raw byte record를
`zlink_recv_part()` 계열로 읽는 socket이 bind 전에 `RAW`를 선택하도록 규정한다.

Core 결과 값도 [`zlink_errno.h`](../../../core/include/zlink_errno.h#L183)에서
`ZLINK_BIND_INVALID_ARGUMENT = 501`로 정의돼 있다. 따라서 spec-valid bind를 Core가 거부한
사례가 아니다.

## 수정과 regression

- [`test_cpp_stream_connector.cpp`](../../../framework/languages/cpp/tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector.cpp#L1391)의
  raw loopback server fixture 21개가 `recv_mode(raw) -> notify(false) -> bind()` 순서를
  명시한다. 첫 fixture를 포함해 모든 connector regression 구간이 같은 계약을 검증한다.
- [`connector_perf_client.cpp`](../../../framework/languages/cpp/connector/perf/connector_perf_client.cpp#L246)의
  loopback server도 같은 순서로 설정한다.
- Assertion, timeout, workload와 Core/package는 변경하지 않았다.

실제 framework STREAM host는 이미
[`stream_host_service.cpp`](../../../framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp#L1553)에서
`max_message_size -> recv_mode(packet) -> monitor_open -> bind()` 순서를 사용하므로 수정하지
않았다.

## 검증 결과

| 검증 | 결과 |
|---|---|
| 수정 전 `test_cpp_stream_connector` 단독 재현 | 0/1, 501 + errno 22로 종료 |
| 수정 후 대상 증분 빌드 | `test_cpp_stream_connector`, `connector_perf_client` 성공 |
| 수정 후 두 CTest case 동시 focused 실행 | 2/2 통과 |
| 수정 후 두 CTest case 3회 반복 | 6/6 통과 |
| 수정 후 전체 C++ CTest `-j2` | 67/69 통과 |

이 조사에서 수정 후 실행한 전체 CTest에서도 `test_cpp_stream_connector`, `connector_perf_smoke`,
`test_cpp_framework_m6b_runtime`은 통과했다. 남은 실패는 다음과 같다.

- `test_cpp_framework_common_e2e_inventory`: 278개 inventory 조건이 열려 있는 기존 실패
- `test_cpp_stream_connector_install_consumer`: relocated consumer configure에서
  `zlink::stream_connector`의 `IMPORTED_LOCATION`이 설정되지 않은 package 작업 소관 실패

## C++ binding 진단 메시지 gap

501을 `Unknown error 501`로 표시하는 것은 원인은 아니지만 별도 C++ binding 진단 gap이다.
Binding은 [`results.hpp`](../../../bindings/cpp/include/zlink/Contracts/Errors/results.hpp#L31)에
`bind_result_t::invalid_argument = 501`을 정의하고 bind 실패를 typed
[`bind_error_t`](../../../bindings/cpp/src/Runtime/Sockets/socket.cpp#L99)로 보존한다. 그러나
`binding_error_t::build_message()`는 결과 code 501을 errno 문자열 함수인
`zlink_strerror()`에 그대로 전달하므로 알려진 typed result의 이름을 만들지 못한다.

이 작업은 `bindings/**` 변경 금지 범위이므로 수정하지 않았다. C++ binding이 typed result를
자체 메시지로 변환하고 `internal_errno()`의 설명을 별도로 붙이는 후속 작업이 필요하다.

## BLOCKERS

이번 bind 실패 수정과 focused regression에는 blocker가 없다. 전체 C++ CTest의 green 판정은
위 inventory와 package 실패가 해결될 때까지 보류된다. C++ binding의 501 진단 메시지 개선도
허용 범위 밖 후속 작업으로 남는다.
