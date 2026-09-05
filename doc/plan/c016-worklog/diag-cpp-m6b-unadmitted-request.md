# C++ m6b 미승인 요청의 native reply 대기 진단

## 판정

감독이 판단할 문제는 `verify_unadmitted_request_is_rejected_without_framework_queue()`의
실패를 Core 결함으로 수정할지, C++ 테스트의 receive 진행 누락으로 수정할지다.

**B — 기존 C++ 테스트 결함이다.** Native 결과는 즉시 `REQUEST_NOT_CONNECTED`가 아니라,
원래 5초 deadline에서 발생한 **`ZLINK_REQUEST_TIMED_OUT`(101), reply 0 part**다.
Target이 먼저 보낸 RouteMesh `hello` DATA를 DEALER fixture가 읽지 않아 같은 physical
FIFO의 뒤에 있는 REPLY가 completion 처리 지점까지 진행하지 못한다.

관찰을 바꾼 수정은 **`7cbf12de41`의 monitor command lease 유지**다.
`8b82d51b75`의 handover terminal 변경은 이 실패의 원인이 아니다. `8b82d51b75`를
포함하는 `7cbf12de41^ = 1c1bfdade3`에서는 같은 테스트가 통과한다.
현재 패키지에서는 monitor ready event → hello DATA → failed reply submit → request timeout이
관찰된다. Core의 FIFO 계약은 구 Core에도 이미 존재했다.

- 소유 계층: Core가 physical FIFO·monitor command 진행·request completion을, binding이 typed 결과 전달을, Framework가 logical admission과 hello/rejection reply를 소유한다. 빠진 DATA receive는 이 테스트의 raw DEALER fixture 책임이다.
- Spec 조항: Core socket README §4 RID 중복 정책, `06-dealer` §2:39–51, `07-router` §9:287–322, `05-polling` §4:86–102, `06-monitoring` §3.1–3.2. Framework wire protocol §4:315–320.
- 교차언어 대조: .NET·Java·Node도 Core timeout을 typed request timeout으로 전달한다. 해당 Framework ingress는 미승인 peer의 application 요청을 reply 없이 거부한다. C++은 failed reply까지 검사하는 별도 raw DEALER fixture가 있다. 다른 언어는 소스 대조이며 실행 검증과 구분한다.
- 변경 분류: **B — 기존 테스트의 DATA receive 진행 누락.** 새 handover 계약에 대한 A 적응이 아니며, Framework runtime 변경은 제안하지 않는다.

STAGE 1 진단만 수행했다. 소스·테스트·spec·package 수정과 commit은 없다.

## 실제 완료 결과와 순서

대상 파일은
`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:5201`이다.
기존 translation unit을 include해 이 함수만 호출하는 진단 executable을 build 디렉터리에
만들었다. 원래 함수와 assertion은 수정하지 않았다.

| 순서 | 관찰과 근거 |
| --- | --- |
| Target 시작 | `raw_mesh_node_owner.cpp:614–648`: ROUTER handover 설정, monitor open, bind, raw route port 생성. `raw_route_port.cpp:55–59`는 POLLIN·POLLOUT·POLLCOMPLETION을 같은 poller에 등록한다. |
| Request 제출 | 테스트 `:5208–5224`: 별도 context의 DEALER가 연결하고 50ms 뒤 native multipart request를 5초 timeout으로 제출한다. Framework hello/admit는 수행하지 않는다. |
| Monitor 진행 | 테스트 `:5231`의 `target.drain_monitor_events()`가 `CONNECTION_READY` event를 읽는다. `raw_mesh_node_owner.cpp:3863–3903`은 ready edge에서 hello를 일반 DATA로 보낸다. |
| Hello 도착 | Debugger가 `zlink_send_part_rid()`에서 읽은 첫 bytes는 `5a 4d 01 01 00 01 00 00`이다. Service-wire magic·major 뒤 command `01`은 hello다. DEALER `xread_activated()`도 호출된다. |
| Logical 거부 | `raw_mesh_node_owner.cpp:2942–2985`: admitted peer 없음 → `reply_failure(notConnected, none)` → `application-drop reason=peer-not-admitted action=reply-not-connected`. Application mailbox 0 assertion은 통과한다. |
| Reply 제출 | `raw_mesh_node_owner.cpp:1611–1651`과 `raw_route_port.cpp:265–287`이 public binding reply를 사용한다. Native `zlink_reply_part()`는 nonzero reply token 1과 FINAL로 호출된다. `reply-not-connected` 로그는 submit 성공을 뜻하며 requester 수신 완료를 뜻하지 않는다. |
| FIFO 정체 | Fixture는 hello DATA를 받지 않고 `await_native_reply()`만 호출한다(`:5241`, helper `:81–85`). `socket_base_api.cpp:1153–1159`는 count-1 FIFO head가 DATA이면 public receive에 준비시키고 completion으로 넘기지 않는다. |
| Terminal | Debugger가 실제 `zlink_completion_recv()` 반환에서 `kind=ZLINK_COMPLETION_REQUEST`, `request_result=ZLINK_REQUEST_TIMED_OUT`, `reply_part_count=0`을 확인했다. `fail_pending_requests_for_transport_pair()`와 `take_pending_reply_from_transport_locked()` breakpoint는 호출되지 않았다. |

Debugger 실행의 상대 시각은 hello DATA submit 1642.955ms, native reply submit 1685.324ms,
timeout completion 6644.958ms다. Debugger 초기 준비 시간이 포함되므로 성능 수치로 사용하지
않는다. Debugger 없는 현재 패키지 focused 실행은 약 5.79초 뒤 SIGABRT였다.

### 오류 문구의 별도 결함

`Network is unreachable (errno=110)`의 앞부분은 typed 결과를 판정하는 근거가 아니다.

1. `core/include/zlink_errno.h:132`의 `REQUEST_TIMED_OUT` 값은 101이다. `NOT_CONNECTED`는 109다.
2. C++ binding `completion_owner.cpp:432–435`는 101을 `request_result_t::timed_out`으로
   보존하고, `:36`에서 내부 errno를 `ETIMEDOUT`(110)으로 정규화한다.
3. `bindings/cpp/include/zlink/Contracts/Errors/errors.hpp:46–52`는 `error_text(code_)`에
   typed enum 값 101을 전달한다. `Runtime/Core/capability.cpp:15–17`의 구현은
   `zlink_strerror(101)`이므로 Linux errno `ENETUNREACH`의 문구가 붙는다.
4. Framework `contracts/dispatch/task.hpp:431–433`가 native 예외 문구를
   `framework_exception_t`에 담아 마지막 `.value()`에서 던진다.

즉 typed result와 내부 errno는 timeout으로 일치하고, 표시 문구만 잘못됐다.
Binding 오류 문구 수정은 별도 B이며 이 테스트의 reply 정체를 해결하지 않는다.

## Core 비교

모든 실행에서 같은 focused executable과 설치된 C++ binding archive를 사용했다.
변경한 것은 실행 process의 Core shared library 선택뿐이다.

| Core | Focused 결과 | 거부 전에 ready/hello 관찰 |
| --- | --- | --- |
| `a0157dc270 = 7ffb8e55d9^` | 통과, 약 0.05초 | 보존한 trace에 없음 |
| `1c69086a4a` | 통과, 약 0.06초 | 보존한 trace에 없음 |
| `1c1bfdade3 = 7cbf12de41^` (`1c69086a4a`, `8b82d51b75`, `0c39ed2e52` 포함) | 통과, 약 0.05초 | 보존한 trace에 없음 |
| 지정된 현재 package library (`7cbf12de41` 포함) | 실패, native TIMED_OUT·0 part | CONNECTION_READY 및 hello DATA 확인 |

`7cbf12de41`는 `socket_base_dispatch.cpp:222–240`에서 completion poller 취득 시
monitor의 command owner까지 중단하던 처리를 고쳤고,
`socket_base_lifecycle.cpp:921–925`는 monitor가 가진 command lease를 유지한다.
현재 fixture는 monitor를 먼저 연 뒤 completion poller를 등록한다. 이 수정으로 ready event가
첫 monitor drain 전에 처리되며, 기존 Framework 코드가 hello를 보내는 조건이 성립한다.
구 실행에서 hello가 기록되지 않았다는 관찰을 “Core는 hello를 보내지 않는다”는 계약으로
일반화하면 안 된다. Hello 송신은 Framework 코드의 결정이다.

SHA-256:

```text
현재 package 및 주 작업 트리 core/build-dev:
a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3
a0157dc270 비교 library:
2c65ea9ba17d3abd667b7092ef093e2ff5395608cd750f1a5d35619fdaa8bd36
1c69086a4a 비교 library:
19cf54668e767a5d7a7076993323323f546c09a45abea06b3bb500e463eb5fee
7cbf12de41^ 비교 library:
d6b000fcdd2589c59b987fd2591275db7c1d33aeb4f5ddd8c415eaf4a20cba5b
```

### 비교 증거의 제한

`/home/hep7/project/zlink-core-b`에서 11:23:24에 `7ffb8e55d9`를 checkout한 뒤,
11:23:36에 다른 작업이 HEAD를 `7cbf12de41^`로 변경했다. 그 사이 빌드된 library와
`7ff-focused*.log`의 session assertion/SIGSEGV는 **혼합 빌드이므로 판정에서 제외**한다.
이후 상대 작업의 build가 별도 snapshot 디렉터리를 사용하는 것을 확인했고,
`1c69086a4a`와 `7cbf12de41^`는 HEAD 유지 상태에서 각각 다시 빌드했다.

정확한 `7cbf12de41` checkout은 시작 때부터 있던
`core/tests/integration/test_disconnect_progress.cpp` 삭제 변경 때문에 Git이 거부했다.
강제 checkout·restore·stash는 하지 않았다. 따라서 표의 마지막 행은 **설치 package 결과**이며
정확한 `7cbf12de41` 단독 빌드라고 주장하지 않는다. 직전 commit의 통과, 현재 package의
native trace, 해당 commit의 두 runtime 변경으로 노출 원인을 판정했다.

## 계약 판정

- [Core socket README §4](../../../core/doc/spec/core/socket/README.ko.md)의 즉시
  `REQUEST_NOT_CONNECTED`는 handover로 **superseded된 transport pair에 이미 admit된
  request**에 적용한다. Framework가 descriptor hello/admit를 하지 않았다는 사실은 이 조건이
  아니다. 이 실행에는 중복 pipe나 supersession이 없고, Core는 request를 정상 접수해
  ROUTER에 nonzero reply token과 함께 전달했다.
- [07-router §9](../../../core/doc/spec/core/socket/07-router.ko.md)는 DEALER reply에 ready
  Application pipe를 사용하고 FINAL 성공은 local admission만 보장한다고 명시한다.
  Framework의 rejection reply가 성공적으로 제출됐다는 사실과 requester가 payload를 읽는
  시점은 다르다.
- [06-dealer §2](../../../core/doc/spec/core/socket/06-dealer.ko.md)와
  [05-polling §4](../../../core/doc/spec/core/05-polling.ko.md)는 앞선 DATA를 dequeue하지 않으면
  뒤의 REPLY가 진행하지 못하고 request timeout이 먼저 발생할 수 있다고 명시한다.
  이 문장은 `a0157dc270`에도 존재한다. Reply가 DATA를 추월하도록 Core를 바꾸는 것은 계약과
  반대다.
- Framework logical admission은
  [wire protocol §4](../../../framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md)가
  소유한다. Hello를 **수신만** 하고 admit를 보내지 않는 DEALER는 여전히 미승인 상태다.

따라서 요청에 제시된 “Core의 NOT_CONNECTED 과잉 적용”과 “control reply가 Core terminal과
중복되어 expectation을 바꿔야 한다”는 양쪽 가설이 모두 맞지 않는다. Rejection reply는
중복되지 않았고 FIFO 뒤에서 대기했다. Native timeout으로 기대치를 바꾸면 기존 거부 reply
검증을 잃는다.

## 공개 C API 재현

Framework 없이 공개 C API만 사용하는
[`repro.c`](../../../framework/languages/cpp/build/linux-ninja-c-e2e/diag-unadmitted/repro.c)를 보존했다.
별도 context의 TCP DEALER/ROUTER를 연결하고 multipart request를 받은 뒤, ROUTER가 일반 DATA
`hello`와 `zlink_reply_part()`의 `reply-not-connected`를 차례로 제출한다. DEADLINE은 원래
테스트와 같은 5000ms다. `drain` 인자는 DEALER public DATA receive를 수행한다.

| 실행 | 공개 API 결과 |
| --- | --- |
| 현재 Core, hello를 읽지 않음 | reply submit OK → 5001.832ms에 REQUEST_TIMED_OUT(101), 0 part |
| `a0157dc270`, hello를 읽지 않음 | reply submit OK → 5000.359ms에 REQUEST_TIMED_OUT(101), 0 part |
| 현재 Core, public `zlink_recv_part()`로 hello 수신 | DATA `hello` 확인 → 0.648ms에 REQUEST_OK(0), 1 part, payload 일치 |
| 현재 Core, 앞선 DATA가 없는 대조 실행 | REQUEST_OK, 1 part; TCP 별도 context·multipart·completion poller에서도 통과 |

Core가 reply를 조기에 terminal로 대체하는 공개 API repro가 아니라, **구·신 Core의 같은 FIFO
계약과 fixture의 누락을 분리하는 repro**다. `completion_recv` 성공 뒤의 global errno는
request terminal errno가 아니므로 C 결과의 `errno=0`을 성공 request로 해석하지 않는다.

```bash
diag=framework/languages/cpp/build/linux-ninja-c-e2e/diag-unadmitted
gcc -Wall -Wextra -Werror -g -O0 \
  -I .artifacts/wsl/install/zlink-core/0.17.0/include "$diag/repro.c" \
  -L .artifacts/wsl/install/zlink-core/0.17.0/lib \
  -Wl,-rpath,/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib \
  -lzlink -o "$diag/repro"
TMPDIR=/dev/shm/zlink-tmp-cpp "$diag/repro"       # exit 1: timeout 관찰
TMPDIR=/dev/shm/zlink-tmp-cpp "$diag/repro" drain # exit 0: reply payload 확인
```

## 언어별 대조

병행 중인 .NET·다른 언어 빌드와 테스트는 실행하거나 수정하지 않았다. 다음은 현재 소스의
같은 ingress 조건과 binding 완료 변환을 대조한 결과다.

| 언어 | 미승인 peer의 node request ingress | Native timeout 전달 |
| --- | --- | --- |
| C++ | `raw_mesh_node_owner.cpp:2942–2985`: mailbox에 넣지 않고 native reply로 Framework `notConnected` header를 제출한다. 이 테스트는 그 payload까지 검사한다. | Binding `completion_owner.cpp:432–435`: `request_error_t(timed_out, ETIMEDOUT)`. 실제 focused 실행으로 확인했다. |
| .NET | `ZLinkManagedMeshNode.cs:5039–5041,5469–5473`: peer가 없거나 Admitted=false이면 ProtocolError를 publish하고 return false. 이 node request 경로는 reply를 보내지 않는다. | Binding `CompletionOwner.cs:1291–1294`: `ZlinkRequestException(completion.RequestResult)`. 같은 Core timeout은 `RequestResult.TimedOut`이다. |
| Java | `ZLinkJavaRawMeshNode.java:4443–4445`: topology에 peer가 없으면 return. node-request 분기는 이후 `:4535`다. | Binding `CompletionOwner.java:674–678`: `ZlinkRequestException(RequestResult.TIMED_OUT)`. |
| Node | `raw-service-mesh-runtime.ts:797–798`: peer가 undefined이면 protocolError. mailbox 실패 뒤의 NotConnected reply(`:905–922`)는 이미 이 guard를 지난 다른 조건이다. | Binding `completion_owner.ts:154–155`: `RequestError(completion.requestResult)`, 이 경우 `RequestResult.TimedOut`. |

다른 언어가 C++과 같은 rejection payload를 현재 생성한다고 주장할 수 없다. 다른 언어에서
이 특정 raw request를 실제로 실행하면 위 ingress return 때문에 reply가 없고 원래 Core
deadline에서 timeout을 관찰할 것으로 예상한다. 이는 소스 기반 추론이며 실행 결과가 아니다.
이 차이를 C++ 실패 해결을 위한 runtime 변경으로 확대하지 않는다.

## 최소 수정안과 회귀

**승인 후 C++ 테스트 fixture만 수정하는 B를 제안한다.** 같은 DEALER의 기존 public DATA
receive 경로로 target의 hello를 수신·검증하면서 native completion을 기다린다. Hello를
보냈다는 이유로 source를 admit시키지 않는다. 두 번째 poller, transport generation 상태,
timeout 증가, retry 추가와 assertion 완화는 필요 없다.

대안인 “ROUTER fixture로 바꿔 별도 Completion lane을 사용”은 DEALER의 기존 검증 범위를
바꾸므로 선택하지 않는다. Core의 DATA/REPLY 순서를 변경하거나 target의 정상 hello를
억제하는 runtime 변경도 책임 경계와 계약을 위반한다.

승인된 수정의 focused regression은 다음을 모두 유지해야 한다.

1. 정상 hello DATA를 확인하되 source는 Framework hello/admit를 보내지 않는다.
2. Target pump 결과는 protocol_error이고 application mailbox는 0이다.
3. Native reply는 1 part이며 correlation=1, terminal=notConnected, failure_code=none이다.
4. 기존 5초 request deadline을 유지한다. Native TIMED_OUT/NOT_CONNECTED를 성공 reply로
   인정하지 않는다.

현재 실패를 노출하는 hello-before-reply 조건을 회귀에 명시하고, 구 package의 지연된 monitor
event에 의존한 통과를 완료 근거로 사용하지 않는다. Framework production runtime에는 이
검증을 위한 DATA drain이나 raw frame 처리를 추가하지 않는다.

## 보존 증거와 변경 범위

증거 root는
[`framework/languages/cpp/build/linux-ninja-c-e2e/diag-unadmitted/evidence/`](../../../framework/languages/cpp/build/linux-ninja-c-e2e/diag-unadmitted/evidence/)다.

- `new-focused.log`, `new-trace-loaded.log`, `new-hello-memory.log`: 현재 native timeout,
  handover terminal 호출 없음, 실제 hello DATA bytes와 reply submit.
- `old-focused-trace.log`, `1c-focused.log`, `pre-7cb-focused.log`: 같은 focused executable의
  구 Core 비교.
- `new-c-data-before-reply.log`, `old-c-data-before-reply.log`, `new-c-data-drain.log`:
  공개 C API FIFO 대조.
- `old-core-build.log`, `1c-core-build.log`, `pre-7cb-core-build.log`, `core-b-reflog.txt`:
  비교 빌드와 commit 경계. 혼합 `7ff` 빌드는 유효 결과에 포함하지 않았다.

원본 로그와 비교 library snapshot은 `/dev/shm/zlink-tmp-cpp/diag-unadmitted/`에도 남겼다.
비교 실행에는 process별 `ZLINK_LIBRARY_PATH`·`LD_LIBRARY_PATH`만 설정했다. 주 작업 트리의
`core/build-dev`, 설치 package 및 다른 작업의 소스·빌드는 변경하지 않았다.
임시 debugger trace sink는 제거했고 기존 tracing에 runtime 코드를 추가하지 않았다.
변경 파일은 이 진단 문서이며, 진단용 wrapper·공개 C repro·실행 로그는 ignored build
디렉터리의 증거다. 전체 gate는 실행하지 않았다. 원래 m6b 실패는 STAGE 2 승인 전이므로
수정하지 않은 상태다.
