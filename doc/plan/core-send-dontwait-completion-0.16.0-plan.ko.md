# Core socket pull 전환·성능 검증과 0.16.0 release 계획

> 작성일: 2026-09-01
> 작업 저장소: `/home/hep7/project/zlink`
> 기준 branch: `main`
> 상태: 0.16.0 확정 draft의 정식 스펙 이관과 구현 인수인계 계획
> 권장 주 작업자: `gpt-5.6-sol`, reasoning `ultra`

이 문서는 새 세션이 이전 대화를 읽지 않아도 Core 확정 계약의 정식 스펙 이관부터 binding package의
로컬 배포, Framework 반영, cross-language E2E와 sample 검증까지 이어서 수행할 수 있게 만든
실행 계획이다. 공개 계약은
[`core-socket-send-recv-completion-0.16.0-spec-draft.ko.md`](core-socket-send-recv-completion-0.16.0-spec-draft.ko.md)에
확정했다. 이 draft에는 구현자가 다시 고를 API·ownership·limit·binding 표면이
없다. 구현 중 다른 계약이 필요해 보이면 임시 API로 우회하지 않고 draft와 정식
스펙을 먼저 변경한다.

기존 Core에는 pending record, target별 FIFO, retry, request correlation, STREAM packet decoder와
fast path가 이미 있다. 이 작업은 재전송 protocol을 새로 만드는 작업이 아니다. 기존 엔진을
일반 part send와 pull receive 표면에 다시 연결하고, callback·physical generation에 결합된
부분을 걷어내는 작업이다. 다만 `zlink_completion_recv()`와 STREAM packet pull queue는 기존
처리를 공개 pull 모델로 전달하는 새 adapter이므로 contract test와 ownership 검증이 필요하다.

## 1. 완료 결과

작업은 다음 결과를 모두 충족해야 끝난다.

1. 확정 draft의 계약을 정식 Core·binding·Framework 스펙의 한국어·영어
   문서에 먼저 반영한다.
2. Core의 사용자 통지 callback을 제거하고 일반 DATA, STREAM packet, SEND·REQUEST completion,
   monitor와 timer를 poller readiness 뒤 pull 함수로 소비한다.
3. 일반 send는 `zlink_send_part()`·`zlink_send_part_rid()`의 direct `flags_`로 `NONE`과
   `DONTWAIT`을 고르고 `user_context_`·completion ID output을 뒤에 둔다. Send/request
   options 구조체, `zlink_send_async` 또는 operation cancel API를 남기지 않는다.
4. `ZLINK_POLLCOMPLETION`은 readiness만 반환하고, SEND·REQUEST payload는 하나의
   `zlink_completion_recv()`로 queue가 빌 때까지 꺼낸다. `zlink_poller_event_t` ABI에는
   completion payload를 추가하지 않는다. Socket당 65,536개의 공유 reservation 상한으로
   completion-bearing SEND·REQUEST를 접수 전에 제한하고 포화 시 `BACKPRESSURED`+`EAGAIN`으로
   operation·slot을 접수하지 않는다. 기존 part 계약대로 호출에 넘긴 `part_`와 staging
   prefix는 소비·폐기한다.
5. STREAM은 첫 bind/connect 전에 RAW 또는 PACKET을 명시적으로 고른다. RAW는 `zlink_recv_part()`, PACKET은
   `zlink_stream_recv_packet()`을 사용하며 두 mode를 섞지 않는다.
6. 일반 recv family는 `zlink_recv_part`, `zlink_router_recv_part`, `zlink_subscribe_part`,
   `zlink_xpub_recv_part`로 유지한다. Request reply는 일반 recv가 아니라 completion queue로 받는다.
7. Request는 `zlink_request_part`, reply는 `zlink_reply_part`로 통합한다. DEALER→ROUTER와
   ROUTER→ROUTER request만 허용하고 responder는 ROUTER다.
8. Public API·monitor·wire에서 `transport_pair_id`, `transport_pair_generation`과 exact-pair
   선택·send·request·disconnect 계약을 제거한다. Local send queue admission 전에 Core가
   보관한 pending send·request만 처음 선택한 logical RID 또는 configured endpoint의 reconnect 뒤
   재시도한다. ID `0` 또는 `ADMITTED` 이후 payload는 replay하지 않는다.
9. C, C++, .NET, Go, Java/Kotlin, Node, Python, Rust binding의 FFI와 언어 관용 API, test, perf,
   사용자 가이드를 같은 계약에 맞춘다.
10. Core test와 C STREAM 검증을 통과하고, C perf에서 직전 Core release와 1024 byte의 모든
    지원 pattern·transport cell을 비교해 어느 cell도 5%를 넘게 나빠지지 않는다.
11. 모든 binding perf가 각 언어가 지원하는 모든 pattern·transport를 1024 byte로 한 번씩
    실행하는 smoke를 통과한다.
12. Core와 binding version을 `0.16.0`으로 동기화하고 Core `core/v0.16.0` release asset,
    checksum과 provenance를 확인한다.
13. Release Core를 사용해 first-party binding package 여덟 개를 만들고 로컬 package 저장소에
    배치한 뒤 clean consumer와 version provenance를 확인한다.
14. Framework C++·.NET·Java·Kotlin·Node가 source가 아닌 로컬 binding package `0.16.0`을
    소비하고, Core callback bridge와 언어별 STREAM 6-byte assembler를 pull 경로로 바꾼다.
15. Framework unit test, 언어별 E2E, cross-language E2E와 공통 sample 일곱 개가 모두 통과한다.
16. 기능별 green gate 뒤 POSDDD·성능·불필요 코드 정리를 수행하고 관련 회귀 test를 다시
    통과시킨 뒤 검토 가능한 단위로 commit·push한다.

Binding의 외부 package registry publish와 언어별 GitHub release는 자동 실행 범위가 아니다.
Core GitHub release와 binding local package 생성·배포까지만 이 계획에 포함한다.

## 2. 계약 기준과 작성 원칙

Draft는 0.16.0 계약을 확정한 이행 입력이고, 보호된 정식 스펙을 수정하기 전까지는
현재 배포 계약을 대체하지 않는다. 정식 스펙 이관 후에는 아래 문서가 계약을 소유한다.

| 주제 | 계약·절차를 소유하는 위치 |
|---|---|
| 0.16.0 확정 API와 행위 계약 | `doc/plan/core-socket-send-recv-completion-0.16.0-spec-draft.ko.md` |
| Core 공통 send·recv·completion | `core/doc/spec/core/socket/README.{ko,en}.md` |
| Poller readiness | `core/doc/spec/core/05-polling.{ko,en}.md` |
| PAIR·DEALER·ROUTER·STREAM | `core/doc/spec/core/socket/{01-pair,06-dealer,07-router,08-stream}.{ko,en}.md` |
| ZMP request correlation·lane | `core/doc/spec/core/protocol/01-zmp.{ko,en}.md` |
| Monitor와 errno | `core/doc/spec/core/{04-events,06-monitoring,03-errors}.{ko,en}.md` |
| Binding 비동기 정책 | `bindings/doc/spec/async-coroutine-policy.{ko,en}.md`, `async-execution-model.{ko,en}.md` |
| Binding exact API | `bindings/doc/spec/{c,cpp,dotnet,go,java,node,python,rust}/README.{ko,en}.md` |
| Framework submit·cancellation·STREAM | `framework/doc/framework/common/spec/server/01-execution/{01-submit-and-completion,03-cancellation-and-shutdown}.{ko,en}.md`, `04-session/01-stream-session.{ko,en}.md` |
| Framework STREAM connector | `framework/doc/framework/common/spec/stream-connector/`와 `languages/<lang>/` |
| Core release와 local package | `scripts/local-package/README.ko.md` |
| C perf runtime·결과 규칙 | `bindings/c/perf/AGENTS.md`, `bindings/c/perf/README.md` |
| 스펙 작성 | `doc/principal/documentation/spec-writing-guide.ko.md` |
| 사용자 가이드 작성 | `doc/principal/documentation/guide-writing-guide.ko.md` |
| 공통 문서 문체·리뷰 | `doc/principal/documentation/documentation-principles.ko.md` |
| 리팩토링·성능 설계 | `doc/principal/dev/posddd.ko.md`, `doc/principal/dev/zlink-system-design-principles.ko.md` |

정식 스펙은 `spec-writing-guide.ko.md`에 따라 개념과 책임을 먼저 설명하고 exact interface를 뒤에
둔다. 마지막 top-level 절에는 public call과 관찰 결과가 하나의 test로 이어지는 검증 요구를
둔다. 한국어를 먼저 의미 검토한 뒤 영어 문서가 같은 계약을 갖게 한다. 정식 문서에는 draft·
plan 링크, 구현 진행 상황과 변경 이력을 남기지 않는다.

사용자 가이드는 `guide-writing-guide.ko.md`에 따라 사용자가 언제 무엇을 선택하는지, 실행 가능한
public 예제와 자주 발생하는 오류를 설명한다. Signature와 인자 나열은 실제 sample에서 확인한
코드와 줄 주석으로 보여주고, 내부 queue·lock·wire 배선은 넣지 않는다. 각 guide는 소유 스펙과
언어별 exact interface·검증 class를 가리킨다.

스펙과 가이드를 크게 고친 뒤에는 `documentation-principles.ko.md` 원칙 9에 따라 독립 reviewer가
다음 두 축을 각각 검토한다.

- 원칙 준수: 문체, 문서 역할, 현재 상태 서술, 링크와 예제
- 코드 부합: public header, export, binding interface와 실행 sample

Review finding은 파일·라인 근거를 직접 확인한 것만 반영한다. Markdown·anchor·상대 링크 검사와
`git diff --check`를 통과해야 문서 commit을 만든다.

## 3. 확정 공개 모델

정확한 signature, ownership과 언어별 추가·제거 API는 확정 draft가 소유한다. 구현자가 혼동하지 않도록
핵심 경계만 요약한다.

### 3.1 Pull-only 통지

| 내용 | 준비 신호 | drain 함수 |
|---|---|---|
| DATA·ROUTER REQUEST·subscription | `ZLINK_POLLIN` | socket별 `*_recv_part()` |
| STREAM framed packet | `ZLINK_POLLIN` | `zlink_stream_recv_packet()` |
| SEND·REQUEST 결과 | `ZLINK_POLLCOMPLETION` | `zlink_completion_recv()` |
| Monitor | `ZLINK_POLLIN` | `zlink_socket_monitor_recv()` |
| Timer | timer readiness | `zlink_timer_recv()` |

Poller event는 source와 readiness bit만 알려준다. Completion payload는 poller event에 넣지 않고
socket-local queue에서 SEND 또는 REQUEST tagged record로 한 건씩 꺼낸다. Request reply multipart는
completion record가 소유하며 `zlink_completion_close()`로 정확히 한 번 해제한다.

Core·C에서 `ZLINK_POLLCOMPLETION`은 queue가 비지 않았음을 알리는 non-consuming
level readiness다. Raw completion을 노출하지 않는 고수준 binding의
`PollCompletion`은 public poller `wait()`가 native queue를 비우고 Task·Future·Promise를 하나
이상 settle하거나 detached state를 정리한 뒤 반환하는 progress event다.
이 event는 public operation의 새 상태 변화를 보장하지 않는다. 고수준 event 반환 시 native queue가
이미 비어 있을 수 있으며 `POLLIN` DATA는 소비하지 않는다.

### 3.2 Send 결과

| 조건 | 반환 | completion ID | 후속 결과 |
|---|---|---:|---|
| `MORE` staging 성공 | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `NONE` FINAL admission | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `DONTWAIT` FINAL 즉시 admission | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `DONTWAIT` FINAL을 Core가 pending으로 보관 | `ZLINK_SUBMIT_OK` | nonzero | ADMITTED 또는 TERMINAL 한 건 |
| Core가 pending을 접수하지 못함 | 해당 submit result | 0 | 없음 |

ADMITTED는 local send queue admission이지 peer delivery가 아니다. Completion ID와
`user_context`는 여러 결과를 연결하기 위한 값이며 Core operation을 취소하지 못한다.
`NONE FINAL`은 호출 진입 시 socket `SNDTIMEO`를 snapshot해 admission까지 기다리고
만료하면 `BACKPRESSURED`+`EAGAIN`, ID `0`, completion 없음으로 실패한다. 기본값은
1,000 ms, `0`은 즉시, `-1`은 무한 대기다.
API 반환 전 explicit target removal은 `NOT_FOUND`+`ENOENT`, permanent peer-type reject는
`NOT_ADMITTED`+`EPROTOTYPE`, context/socket 종료는 `TERMINATED`+`ETERM/ESHUTDOWN`, internal
allocation failure는 `OUT_OF_MEMORY`+`ENOMEM`, 그 밖의 runtime failure는
`INTERNAL_ERROR`+`EIO`로 동기 실패한다. 모두 ID 0·completion 없음이고
request의 provisional reservation을 반납하며 전체 sequence를 소비한다.

0.16.0에서 `ZLINK_OPT_SEND_PENDING_MAX_MSGS/BYTES`는
`ZLINK_OPT_PENDING_MAX_MSGS/BYTES`로 rename한다. Numeric value `0x303A/0x303B`는
유지하고 0.15 이름 alias는 두지 않는다. 두 `uint64_t` option의 기본 `0`은
unlimited이며, 즉시 admission되지 않아 Core가 보관하는 DONTWAIT SEND·REQUEST가
socket-wide pool을 공유한다. `MAX_MSGS`는 완성 multipart record 수,
`MAX_BYTES`는 part별 `max(payload size, sizeof(zlink_msg_t))` 포화 합산이다.

### 3.3 Request와 reply

| Requester | target | Responder | 허용 여부 |
|---|---|---|---|
| DEALER | `NULL` | Core가 선택한 ROUTER | 허용 |
| ROUTER | non-NULL logical RID | 해당 ROUTER | 허용 |
| ROUTER | DEALER RID | DEALER | typed request 금지, DATA만 허용 |

Successful request FINAL은 언제나 nonzero completion ID를 반환하고 reply·timeout·terminal 중
한 건을 REQUEST completion으로 만든다. Responder ROUTER는 `zlink_router_recv_part()`에서 source
RID와 opaque reply token을 받고 `zlink_reply_part()`에 그대로 전달한다. Wire request sequence와
reply token은 서로 다른 계약 개념이며 숫자가 같다는 보장이 없다.

DEALER request의 후보는 handshake로 ROUTER임이 확인된 양수 weight logical route다.
예전에 ROUTER로 확인됐고 configured endpoint가 남은 detached route는 마지막 양수
weight로 후보가 된다. 한 번도 handshake하지 않은 endpoint를 ROUTER로 가정하지
않는다. `NONE`은 unknown endpoint handshake를 `SNDTIMEO` 동안 기다린다. 판정 시
known ROUTER가 없으면 `NOT_CONNECTED`+`ENOTCONN`, 하나 이상 있지만 모두 weight 0이면
`NOT_ADMITTED`+`ECONNREFUSED`다. `DONTWAIT`은 같은 판정식을 즉시 적용한다. Target은
`FINAL`에서 한 번 선택하고 재연결 중
다른 endpoint로 retarget하지 않는다. Reply timeout은 outbound local admission에서
시작하고 admission 이후 request payload는 replay하지 않는다.

### 3.4 Cancellation과 close

Core가 successful submit으로 payload를 소유한 뒤에는 operation cancel API가 없다. Language
cancellation은 submit 전 차단, Framework-owned queue 제거 또는 caller wait 중단으로만 처리한다.
Caller wait가 끝나도 Core operation은 admission·reply까지 진행할 수 있으므로 socket owner는
뒤늦게 온 completion을 반드시 drain하고 payload를 정리한다.

Socket close는 unread completion을 전달하는 별도 단계가 아니다. Caller가 결과를 필요로 하면
close 전에 drain한다. Close 뒤 Core가 unread completion과 packet을 정리한다.

## 4. 현재 상태와 보존할 최적화

### 4.1 Working tree 보호

새 세션은 첫 명령으로 다음을 실행한다.

```bash
git branch --show-current
git status --short
git diff -- core/src/runtime/sockets/common/socket_runtime.hpp \
  core/src/runtime/sockets/common/socket_send_async_submit.cpp \
  core/src/runtime/sockets/common/socket_send_complete.cpp \
  core/tests/integration/test_stream_send_blocking_wakeup.cpp
```

다음 네 파일의 사용자 STREAM 성능 변경은 2026-09-01 `main`에 commit됐다
(`59d6de03d3` fast-path). 이 변경을 승인 없이 revert, restore, reset, checkout,
삭제 또는 덮어쓰기하지 않으며 §4.2의 최적화 보존 대상으로 취급한다.

- `core/src/runtime/sockets/common/socket_runtime.hpp`
- `core/src/runtime/sockets/common/socket_send_async_submit.cpp`
- `core/src/runtime/sockets/common/socket_send_complete.cpp`
- `core/tests/integration/test_stream_send_blocking_wakeup.cpp`

다른 dirty·untracked 파일은 사용자 작업으로 보고 요청 범위 밖에서는 수정하거나 commit하지
않는다.

### 4.2 STREAM 최적화의 쉬운 설명

보존할 성능 개선은 다음과 같다.

1. 보낼 공간이 있으면 caller가 준 원본 message를 바로 send queue에 넣는다. 정상 경로에서
   pending용 복사본을 미리 만들지 않는다.
2. 실제로 `EAGAIN`이 나와 기다려야 할 때만 Core가 보관할 message copy를 만든다. 복사 비용은
   backpressure가 발생한 record만 부담한다.
3. STREAM RID별 예약 node를 logical route가 유지되는 동안 재사용한다. Packet마다 tree node를
   할당하고 해제하지 않는다.
4. 같은 target에 먼저 기다리는 record가 있을 때만 redrive를 예약한다. 이미 처리할 일이 없는
   queue를 반복해서 깨우지 않는다.
5. Detach와 admission이 경합하면 admit 여부를 한 지점에서 확정한다. Admit되지 않은 record는
   실패로 버리지 않고 logical RID queue로 돌려 reconnect 뒤 다시 보낸다.

적용 범위는 다음과 같이 고정한다.

| 경로 | 적용할 최적화 | 적용하지 않는 내용 |
|---|---|---|
| PAIR send | direct admission, EAGAIN 때만 copy, 단일 logical-route reservation 재사용 | RID map |
| DEALER send/request | direct admission, EAGAIN 때만 copy, 처음 선택한 configured-endpoint reservation 재사용 | 물리 pair·generation key |
| ROUTER send/request/reply | direct admission, EAGAIN 때만 copy, logical RID별 reservation 재사용 | STREAM packet decoder |
| STREAM send | direct admission, EAGAIN 때만 copy, logical RID별 reservation 재사용 | 다른 socket에 packet framing 이식 |
| PUB·XPUB publish·receive-only socket | 기존 전용 계약 유지 | Completion pending fast path 적용 |

즉 direct-admission·EAGAIN-only-copy와 logical target reservation은 공통 complete-record engine으로
올린다. STREAM decoder·packet framing은 STREAM에만 남긴다. 자료구조 변경은 각
경로의 profiler에서 copy·allocation·contention 비용을 확인한 뒤 적용한다. §5의 cell 성능
gate를 넘지 못한 자료구조 변경은 되돌리고 기존 구조를 유지한다. Public 계약과
reconnect/resend 소유권은 자료구조 선택과 관계없이 고정된다.

현재 작업 전 기준 결과는 다음과 같으며 최종 gate를 대신하지 않는다.

- `cmake --build core/build -j2`: 성공
- `test_stream_send_blocking_wakeup` 20회: 20/20 성공
- Core STREAM 관련 test: 20/20 성공
- Core 전체 test: 105/105 성공
- C multi STREAM tcp/tls/ws/wss × 64/256/1024/65536: 16/16 성공
- 기존 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260901_155600_stream_fastpath_finish.txt`

## 5. 실행 단계

각 phase는 가장 작은 관련 test부터 시작하고 green 상태에서만 다음 phase로 넘어간다. 실패한
전체 gate를 원인 변화 없이 반복하지 않는다.

### Phase 0 — 시작 상태와 확정 draft 대조

1. Root와 수정 대상 하위 경로의 `AGENTS.md`를 읽고 `main` branch·dirty 범위를 확인한다.
2. 작업자는 `spec-writing-guide`, `guide-writing-guide`, `documentation-principles`, `POSDDD`,
   `zlink-system-design-principles`를 처음부터 끝까지 읽는다.
3. 확정 draft §3~§12를 public header·export·binding interface 현재 상태와 대조해
   구현 gap만 적는다. Cleanup ABI, borrowed RID lifetime, reply-token invalidation,
   request timeout, STREAM error, 65,536 상한과 언어별 API를 다시 선택하지 않는다.
4. 정식 스펙 수정 전 root `AGENTS.md` §5에 따라 이 plan §2의 exact 보호 경로가
   현재 사용자 승인 범위인지 확인한다. 승인 범위가 아닌 보호 문서는 수정하지
   않고 exact `file:line`과 제안만 보고한다. 이는 파일 보호 절차이며 API 미정 항목이 아니다.
5. 제거·rename 대상의 초기 검색 결과를 보관한다.

```bash
rg -n "zlink_send_async|zlink_send_async_cancel|zlink_send_(async_)?options_t|zlink_request_options_t|zlink_send_complete_handler(_fn)?|zlink_reply_handler_fn|zlink_recv_handler|zlink_socket_msg_handler_fn|zlink_stream_packet_handler(_fn)?|zlink_socket_monitor_handler(_fn)?|zlink_monitor_ignore_handler|zlink_timer_handler(_fn)?|transport_pair_(id|generation)|router_recv_part_v2|dealer_(request|recv|reply)_part|router_request_part|router_reply_part|routed_submit_target|ZLINK_OPT_SEND_PENDING_MAX_(MSGS|BYTES)|RoutedSend|RequestCallback|RequestReplyCompletion|request_seq|RequestSeq|TrySend|send_async|sendAsync|onPacket|on_packet|onEvent|on_event|onFire|on_fire" \
  core bindings framework -g '!**/build/**' -g '!**/target/**' \
  -g '!**/dist/**' -g '!**/node_modules/**'
```

`request_seq`는 Core wire correlation 내부에는 남을 수 있지만 public field·parameter에서는
제거한다. Framework application handler와 zero-copy `zlink_free_fn`, user thread entry도
제거 대상이 아니다. 검색 hit를 symbol 이름만으로 지우지 않고 계약 소유 레이어를
확인한다.

완료 조건은 draft·plan·현재 public surface의 gap이 파일·symbol 단위로 연결되고,
수정 가능한 보호 문서 범위가 확인된 상태다.

### Phase 1 — 정식 스펙 반영

Core 정식 스펙은 다음 순서로 바꾼다.

1. `socket/README.{ko,en}.md`에 direct send/request 인자, completion tagged record, 일반 recv family,
   request/reply와 ownership을 정의한다. `NONE`의 `SNDTIMEO`, DONTWAIT pending·
   completion 상한, `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`의 공유 SEND·REQUEST 계산과
   구 option 이름 alias 부재를 포함한다.
2. `05-polling.{ko,en}.md`에서 `ZLINK_POLLCOMPLETION`을 level-triggered readiness로 정의하고
   poller wait가 payload를 소비하거나 callback을 호출하지 않는다고 명시한다.
   PAIR·DEALER·ROUTER·STREAM의 add·modify는 completion bit 추가·제거를 허용하고,
   다른 source와 `zlink_poll()` item은 `ZLINK_CONFIG_INVALID_ARGUMENT`+`EINVAL`로 거부한다.
   Socket당 completion registration owner는 한 poller만 허용하고 두 번째 owner는
   `ZLINK_CONFIG_INVALID_STATE`+`EBUSY`로 거부한다.
3. `01-pair`, `06-dealer`, `07-router`, `08-stream`의 target·reconnect·RAW/PACKET·reply token
   계약을 맞춘다. Admission 전 pending만 logical target에 재시도하고 admission 이후
   application payload는 replay하지 않는 경계, DEALER request route 후보·weight·고정 선택,
   STREAM `MAXMSGSIZE`·`NOTIFY`·output 오류를 exact result·errno까지 적는다.
4. `protocol/01-zmp`에서 wire request sequence는 internal correlation으로, public reply token과
   분리한다. Pair/generation property를 제거하되 필요한 `Zlink-Lane`은 유지한다.
5. `03-errors`, `04-events`, `06-monitoring`의 result·field·monitor pull 계약을 맞춘다.
   `03-errors.{ko,en}.md`에서 permanent peer-type rejection을
   `ZLINK_SUBMIT_NOT_ADMITTED`+`EPROTOTYPE`로 mapping하고, platform header에
   errno가 없을 때의 public portable fallback을
   `EPROTOTYPE = ZLINK_HAUSNUMERO + 23`,
   `EOVERFLOW = ZLINK_HAUSNUMERO + 24`로 고정한다. Completion sequence 소진은
   후자를 사용한다.
   XPUB topic buffer 부족은 event를 dequeue하지 않는
   `ZLINK_RECV_BUFFER_TOO_SMALL`+`ENOBUFS`로 일원화하고 0.15의 dequeue+
   `INTERNAL_ERROR`+`EMSGSIZE`를 제거한다. 같은 단계에서
   `core/doc/spec/core/07-utilities.{ko,en}.md`의 timer handler 표면과
   `core/doc/spec/core/protocol/02-raw.{ko,en}.md`의 stream packet handler 서술도
   pull 계약으로 정리한다.
6. Binding 비동기 스펙에서 Core operation cancel과 callback bridge를 제거하고 언어별 awaitable이
   poller+drain loop 위에 있다는 계약을 정의한다.
   `bindings/doc/spec/{c,cpp,dotnet,go,java,node,python,rust}/README.{ko,en}.md`에는
   draft §11의 exact 공개 signature, operation 시작 이름, 추가·제거 API,
   `ReplyToken`의 생성·동등성·socket-owner 검증, reusable `StreamPacket` lifecycle,
   monitor·timer pull과 `PollCompletion` owner 이전을 언어별로 그대로 옮긴다. Core/C의
   level readiness와 고수준 binding의 drain 후 progress event를 같은 의미로 쓰지 않는다.
7. Framework `01-submit-and-completion`, `03-cancellation-and-shutdown`, `01-stream-session`에서
   Core ownership 뒤 caller cancellation이 late admission을 막지 않는 경계와 packet pull 기반
   managed queue admission을 정의한다. Public Framework application handler는 유지한다.
8. 변경한 모든 한국어·영어 문서를 독립 2축 review하고 문서·public-surface 검사를 실행한다.

정식 스펙 commit은 코드 구현과 섞지 않는다. Review와 검사가 green이면 다음 형식의 독립 commit을
만들고 `main`에 push한다.

```text
docs(spec): define pull socket completion and receive contracts
```

### Phase 2 — Core public API 정리

주요 경로:

- `core/include/zlink/socket/api.h`
- `core/include/zlink/eventing/api.h`
- `core/include/zlink_enum.h`, `core/include/zlink_errno.h`
- `core/src/api/socket/`
- `core/src/api/core/zlink.cpp`
- `core/src/libzlink.vers`
- public header mirror·surface 검사

작업:

1. Options 구조체를 추가하지 않고 기존 `zlink_send_part*()`에 direct
   `flags_`, `user_context_`, `completion_id_out_`을 두며 `zlink_request_part()`에 direct
   `flags_`, `timeout_ms_`, `user_context_`, `completion_id_out_`을 둔다. `zlink_completion_t`,
   `zlink_completion_recv()`, `zlink_completion_close()`를 선언한다. Send·request의
   `completion_id_out_`은 optional이며 non-NULL이면 다른 validation 전 `0`으로
   초기화하는 동일 wrapper 계약을 사용한다.
2. Request/reply 이름은 `zlink_request_part()`·`zlink_reply_part()`, 일반 recv는 네 함수,
   STREAM packet은 `zlink_stream_recv_packet()` 하나로 draft §3~§7의 exact declaration을
   public header와 version script에 반영한다. 이 phase에서는 queue·token·decoder 동작을
   중복 구현하지 않고 Phase 3의 단일 runtime owner에 연결할 API adapter 경계만 만든다.
3. Completion kind/result/aggregate, completion ID, opaque reply token, STREAM receive mode와
   pending option의 enum value·struct layout을 선언하고 C ABI mirror와 compile-time layout
   assertion을 맞춘다.
   `zlink_errno.h`에는 `#ifndef EPROTOTYPE` fallback `ZLINK_HAUSNUMERO + 23`과
   `#ifndef EOVERFLOW` fallback `ZLINK_HAUSNUMERO + 24`를 추가하고 모든 native header
   mirror에 동기화한다. C++·Rust adapter는 별도 system/libc 숫자를 hard-code하지
   않고 이 public/native errno 값을 그대로 전달한다.
4. Public header 주석에 part 입력 소비, output empty predicate, reply array close, borrowed RID,
   timeout과 first bind/connect mode 경계를 적되 내부 queue 구현을 넣지 않는다.
5. Socket·send completion·request reply·STREAM·monitor·timer handler typedef와 등록 export를
   제거한다. `zlink_free_fn`과 `zlink_thread_fn`은 유지한다.
6. Separate async/cancel/deadline send API, DEALER typed recv/reply, router recv v2와 exact-pair
   API·field·export를 제거한다.
7. `ZLINK_OPT_SEND_PENDING_MAX_MSGS/BYTES`를 numeric value를 유지한
   `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`로 rename하고 구 이름 alias를 두지 않는다. PAIR·DEALER·
   ROUTER·STREAM 지원 matrix와 다른 socket의 `NOT_SUPPORTED`+`ENOTSUP`를 public option
   declaration·validation table에 반영한다. Core enum과 C/C++/Go/Rust native header
   mirror·수동 FFI/internal generated constant를 같은 commit에서 맞춘다. 0.15에 없던 새 고수준
   option façade는 C++·.NET·Java/Kotlin·Node·Python·Go·Rust에 만들지 않는다.
8. `zlink_poller_event_t`는 기존 layout을 유지하고 completion payload field를 추가하지 않는다.

Phase 2 gate는 runtime 행위를 선행 요구하지 않는다. Core build와 public C/C++ header compile,
export/version-script 검사, struct size·alignment·offset과 enum numeric value, 새/제거 symbol 및
C mirror parity만 green으로 만든다. Invalid handle·enum·flag처럼 wrapper boundary에서 완결되는
validation test, send·request ID output의 NULL/non-NULL·validation 전 zeroing test와
platform이 `EPROTOTYPE`·`EOVERFLOW`를 각각 제공하는/하지 않는 compile 경로의
fallback numeric·mirror parity·error mapping test도 여기서 통과시킨다. Runtime queue·reconnect·decoder를 요구하는 draft §12
contract test는 red/disabled 상태로 먼저 land하지 않고 Phase 3 구현과 같은 변경 단위에서 추가해
Phase 3 gate로 실행한다.

### Phase 3 — Core runtime 재배선

주요 경로:

- `core/src/runtime/sockets/common/socket_runtime.hpp`
- `core/src/runtime/sockets/common/socket_send_async_submit.cpp`
- `core/src/runtime/sockets/common/socket_send_complete.cpp`
- `core/src/api/socket/socket_request_reply_internal.cpp`
- `core/src/api/socket/request_completion_queue_internal.*`
- `core/src/runtime/sockets/common/socket_base*`
- STREAM decoder·receive queue와 socket별 write-activation/detach hook

작업:

1. SEND와 REQUEST terminal record가 같은 public completion queue와 readiness latch를 사용하게
   한다. Socket당 공유 reservation 상한은 65,536으로 고정하고 operation 접수부터
   completion recv까지 계산한다. Slot 부족은 `BACKPRESSURED`+`EAGAIN`으로 operation·slot을
   접수하지 않되 기존 part 계약대로 입력 part와 staging prefix를 소비·폐기한다. Socket이 열려
   있고 caller가 drain하는 동안에는 queue capacity 때문에 completion을 버리거나 합치지 않으며
   queue를 무제한으로 키우지 않는다. Socket close와 context termination의 lifecycle cleanup은
   이 전달 보장의 범위 밖이다. SEND·REQUEST resolver는 하나의 mutex/strand append
   linearization 지점에서 완료 순서를 정하며 submit 순서를 보장한다고 가정하지 않는다.
   Completion `peer_rid`는 PAIR·DEALER SEND와 DEALER REQUEST에서 empty, ROUTER·STREAM
   SEND와 ROUTER REQUEST에서 submit RID snapshot으로 고정하고 reconnect의 physical identity로
   바꾸지 않는다. Explicit endpoint/RID 제거, permanent peer-type/protocol 거절,
   allocation/runtime failure, transient disconnect와 close/context는 draft §4의 exact
   terminal result·errno 또는 close/context no-delivery lifecycle 행렬을 따른다.
   `zlink_send_part*()`와 `zlink_request_part()`의 MORE/FINAL staging, NONE wait, DONTWAIT
   immediate/pending, ID 0/nonzero와 input consumption을 이 owner에 연결하고 별도 async path를
   만들지 않는다.
2. Callback dispatch, callback TLS scope, callback-before-return lifetime pin과 handler registry를
   제거한다. `user_context`는 submit 전에 생성한 language state를 되찾는 opaque 값으로만 유지한다.
3. DONTWAIT pending key를 logical target으로 바꾼다. PAIR은 단일 route, DEALER는 처음 선택한
   configured endpoint, ROUTER·STREAM은 RID를 사용한다. DEALER request는 handshake로
   ROUTER임이 확인된 양수-weight route만 후보로 삼고 FINAL에서 한 번 선택한다.
4. Transient detach는 local admission 전 DONTWAIT pending record와 진행 중인 `NONE FINAL`
   admission wait를 terminal 처리하지 않는다. 같은 logical target이 reconnect되어
   writable해지면 DONTWAIT pending은 FIFO의 첫 record부터 다시 시도하고 `NONE`은 snapshot한
   `SNDTIMEO` budget 안에서만 계속 기다린다. 두 경로 모두 FINAL에서 고정한 target을
   다른 endpoint로 retarget하지 않는다. ID `0` 또는 ADMITTED 이후에는 application
   record copy·ACK·dedup shadow를 남기거나 reconnect 뒤 replay하지 않는다. Request는
   admission 이후 payload를 replay하지 않지만 correlation과 남은 timeout budget은 유지하고,
   reply도 admission 이후 replay하지 않는다. 아직 반환하지 않은 NONE wait의 explicit
   removal·peer-type reject·context/socket lifecycle·internal failure는 draft §3.2의 동기 submit
   result·errno, ID 0, no completion으로 끝내고 request provisional reservation을 반납한다.
5. Application/Completion lane association에서 public pair ID·generation과 ZMP property를 제거한다.
   Connector는 configured endpoint, acceptor는 adopted logical RID를 내부 소유 기준으로 사용한다.
6. Send deadline scheduler와 cancel resolver를 제거하되 request timeout scheduler는 유지한다.
   Reply FINAL은 logical RID의 reconnect를 `SNDTIMEO` 범위에서 기다리고 successful admission에서만
   token을 소비한다. Reply의 timeout·allocation·runtime·context·socket lifecycle을
   draft §7의 `BACKPRESSURED`/`OUT_OF_MEMORY`/`INTERNAL_ERROR`/`TERMINATED`·exact errno에
   연결한다. 모든 call의 part 소비, successful MORE staging, 실패한 sequence prefix
   폐기·checkout 해제·live token 유지, RID/socket lifecycle token 무효화를 하나의
   registry owner에서 처리한다. Concurrent second sequence의 `EBUSY`는 기존 staging·checkout을
   건드리지 않고, 진행 중 sequence의 후속 RID/token mismatch는 그 sequence를 폐기하고
   original checkout을 해제한다. Public token abandon/cancel은 두지 않고 unreplied token은 responder
   registry 65,536 상한으로 제한한다. Registry가 차면 새 REQUEST는 전역 cap 때문에 모두
   대기하되 blocked REQUEST가 head인 source pipe를 건너뛰어 다른 pipe의 admissible DATA를
   fair-queue 순서로 진행시킨다. 같은 pipe의 DATA는 REQUEST를 앞지르지 않고, application
   queue가 비었으며 모든 readable head가 token-blocked REQUEST일 때 `POLLIN`을 내린다.
   Slot 해제 시 paused pipe를 round-robin redrive한다.
   `zlink_router_recv_part()`는 DATA token 0과 REQUEST의 socket-owned nonzero token을 만들고,
   `zlink_reply_part()`의 checkout·retry·successful FINAL consumption을 같은 registry owner에 둔다.
7. `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`는 DONTWAIT SEND·REQUEST가 하나의 socket-local
   pending pool로 공유한다. `MAX_MSGS`는 완성 multipart record, `MAX_BYTES`는 part마다
   `max(payload size, sizeof(zlink_msg_t))`를 더한 overflow-saturating 값이다. `MORE`에서는
   예약하지 않고 FINAL에서 전체 charge를 원자적으로 검사·예약하며 admission·terminal에서
   해제한다. 실행 중 상한 축소는 기존 reservation을 eviction하지 않고 새 reservation에만
   적용한다. 이 pool은 completion 65,536 reservation과 별도 자원이다.
8. Recv가 반환하는 borrowed RID는 TLS가 아닌 socket-owned storage에 보관한다. 같은
   socket의 다음 data recv API 진입—성공·실패 관계없음—이나 close에서 이전 view를
   무효화하고 binding은 반환 즉시 owned RID로 복사한다. 같은 thread의 다른 socket
   recv는 해당 view를 무효화하지 않는다.
   네 recv family의 필수·선택 output과 part ownership을 한 receive owner에 연결하고 SUB·XPUB
   buffer-too-small은 required length만 써서 record를 보존한다. Empty topic zero-capacity success와
   positive-capacity NULL buffer failure도 여기서 처리한다.
9. REQUEST error reply의 내부 errno part를 제거한 public payload는 새 contiguous
   `zlink_msg_t[]`의 base index 0부터 정규화한다. Middle pointer를 공개하지 않고 allocation
   실패는 payload 없는 `ZLINK_REQUEST_INTERNAL_ERROR`로 완료해 close가 항상 정확한 allocator
   base를 한 번 해제하게 한다.
10. STREAM decoder가 완성한 packet을 bounded receive queue에 넣고 `POLLIN`을 level-trigger한다.
    Queue가 차면 read를 멈추며 hidden queue·drop을 만들지 않는다. First bind/connect 전 mode
    validation, first success 뒤 mode·NOTIFY freeze, RAW/PACKET family 배타, MAXMSGSIZE snapshot과
    header/body output validation도 이 STREAM owner에서 구현한다.
11. §4.2의 direct admission, EAGAIN-only copy, reservation reuse와 필요한 경우에만 redrive하는
    fast path를 보존한다.
12. Socket close와 context termination은 pending·unread packet/completion을 내부 정리하고 새
    terminal completion 전달을 보장하지 않는다. Binding은 자신의 waiter를 각각 shutdown·
    terminated error로 끝낸다. Close를 위해 poller remove를 `EBUSY`로 pin하지 않는다.

Race test는 admission/detach/reconnect, reply/timeout, drain/close, packet enqueue/close를 각각
반복한다. Admission 전 pending의 같은 logical target FIFO·exactly-once admission,
admission 이후 disconnect의 no-replay, 서로 다른 target의 독립 진행을 함께 확인한다.

Phase 3 gate에서 draft §12의 Core contract test를 모두 활성화해 public call과 관찰 결과로
검증한다. 최소 묶음은 다음과 같다.

- NONE/DONTWAIT·MORE/FINAL·`SNDTIMEO`의 immediate/pending/synchronous-failure/terminal 행렬,
  특히 pre-return `OUT_OF_MEMORY`+`ENOMEM`/`INTERNAL_ERROR`+`EIO` 분리, pending option
  rename·공유 count/byte·release와 completion 65,536 상한, send·request ID output
  NULL/non-NULL·validation 전 zeroing·output 생략 시 internal ID·successful request context echo
- Mixed SEND/REQUEST linearized drain, ID/context/`peer_rid`, exact result·errno와
  close/context no-delivery, completion output empty predicate·close·error-reply allocator base
- Core/C poller level readiness·single owner, `RCVTIMEO`와 blocking context `TERMINATED`+`ETERM`/
  socket `INVALID_STATE`+`ESHUTDOWN`, 네 recv output, socket-owned borrowed RID,
  SUB/XPUB nonempty-topic buffer retry와 empty-topic zero-capacity success
- STREAM 첫 bind/connect 전 mode, 첫 성공 뒤 mode/NOTIFY freeze, MAXMSGSIZE, packet output,
  malformed-peer isolation과 RCVHWM backpressure
- DEALER known-ROUTER selection·weight·no-retarget, request admission 전 retry/후 no-replay,
  reply timeout·OOM·runtime·context/socket exact result·part consumption, successful MORE·
  second-sequence EBUSY·follow-up mismatch의 checkout/token 행렬, late reply와 65,536 registry
  fairness/POLLIN
- Monitor·timer pull, callback·cancel·pair/generation·old pending-name symbol 부재

위 targeted suite가 모두 green이어야 Phase 4의 반복 race와 Core 전체 suite로 넘어간다.

### Phase 4 — Core 기능 검증과 리팩토링

가장 작은 검증부터 실행한다.

```bash
cmake --build core/build -j2
ctest --test-dir core/build \
  -R 'send|completion|request|reply|stream|router|poller|monitor|timer' \
  --output-on-failure
ctest --test-dir core/build --output-on-failure
```

STREAM wakeup·detach·packet queue race는 최소 20회 반복한다. 첫 실제 실패부터 기존 message
tracking과 file log를 켜고 run directory를 보존한다.

기능 test가 green이면 Core 변경 범위만 다음 순서로 리팩토링한다.

1. POSDDD 위험 신호와 ZLink Z1~Z5를 훑는다. Callback 삭제 뒤 남은 handler state, pass-through
   adapter, duplicate queue, deadline/cancel branch와 사용하지 않는 type·test helper를 제거한다.
2. Completion queue가 SEND·REQUEST의 ownership·bounded admission·cleanup을 한 소유자 안에
   숨기는지 확인한다. 실행 단계별 얕은 class로 다시 나누지 않는다.
3. Hot path에서 allocation·copy·contention을 측정한다. 먼저 얕은 layer·중복 확인을 제거하고,
   그래도 측정된 병목이 남을 때만 자료구조 대안 두 개를 비교한다.
4. Benchmark 전용 branch, payload size 전용 특례와 public contract를 약하게 만드는 최적화는
   적용하지 않는다. 5% 미만인 micro-optimization은 제거한다.
5. Refactor 뒤 targeted test와 Core 전체 suite를 다시 한 번 실행한다.

Core source·test가 green이고 diff review가 끝나면 관련 변경만 commit하고 push한다.

```text
core: unify socket completion and receive pulling
```

### Phase 5 — C STREAM 검증과 직전 release 성능 비교

#### 5.1 STREAM 기능·perf matrix

`bindings/c/perf/AGENTS.md`에 따라 `core/build`를 먼저 갱신하고 runner가 출력한 실제
`libzlink.so`가 source보다 새 것인지 확인한다.

```bash
cmake --build core/build -j2
bindings/c/perf/run_benchmarks_multi.sh \
  --pattern STREAM \
  --transports tcp,tls,ws,wss \
  --msg-sizes 64,256,1024,65536 \
  --runs 1
```

16개 cell이 모두 실행되고 skip/fail과 server hang이 없어야 한다. Report 경로와 resolved
library 경로를 기록한다.

#### 5.2 C 전체 1024-byte 비교

비교 기준은 직전 Core release `core/v0.15.1`로 고정한다. Baseline과 0.16.0은 같은 host,
CPU governor, build profile, I/O thread, HWM, timeout과 runner revision에서 직렬로 실행한다.

0.16.0에서 public C API가 바뀌므로 변경된 perf source가 0.15.1 header로 다시 compile된다고
가정하지 않는다. Baseline은 `core/v0.15.1`의 detached worktree에서 그 release의 C perf source로
실행하고, candidate는 현재 `main` worktree에서 실행한다. 두 runner의 scenario·process orchestration·
metric 계산이 같은지 먼저 diff로 확인한다. API 이름을 맞추기 위한 adapter 차이 외에 측정 방식이
달라졌다면 baseline을 그대로 비교하지 않고 harness 차이를 먼저 제거한다.

Baseline worktree와 실행:

```bash
zlink_perf_baseline_dir="$(mktemp -d /tmp/zlink-perf-core-0.15.1.XXXXXX)"
git worktree add --detach "$zlink_perf_baseline_dir" core/v0.15.1

(cd "$zlink_perf_baseline_dir" && \
  bindings/c/perf/run_benchmarks.sh \
    --core-version 0.15.1 \
    --pattern ALL \
    --transports tcp,tls,ws,wss,inproc,ipc \
    --msg-sizes 1024 \
    --runs 5 \
    --results-tag core-0.15.1-baseline-1024)

(cd "$zlink_perf_baseline_dir" && \
  bindings/c/perf/run_benchmarks_multi.sh \
    --core-version 0.15.1 \
    --pattern ALL \
    --transports tcp,tls,ws,wss \
    --msg-sizes 1024 \
    --runs 5 \
    --results-tag core-0.15.1-baseline-1024)
```

0.16.0 workspace Core:

```bash
cmake --build core/build -j2

bindings/c/perf/run_benchmarks.sh \
  --pattern ALL \
  --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 \
  --runs 5 \
  --results-tag core-0.16.0-candidate-1024

bindings/c/perf/run_benchmarks_multi.sh \
  --pattern ALL \
  --transports tcp,tls,ws,wss \
  --msg-sizes 1024 \
  --runs 5 \
  --results-tag core-0.16.0-candidate-1024
```

C single `ALL`은 PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, ROUTER_ROUTER,
DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP와 Linux transport 여섯 개를 포함한다. Multi `ALL`은
DEALER_DEALER, 두 SENDSEND, 두 REQREP, PUBSUB, STREAM과 network transport 네 개를 포함한다.
Runner가 지원하지 않는 조합은 expected unsupported로 별도 기록하고, 지원 대상의 unexpected
skip·missing·fail을 통과로 계산하지 않는다.

현재 runner에는 두 report의 5% 회귀를 자동 판정하는 기능이 없다. 기존 report parser를 재사용해
single·multi report를 cell key `(pattern, transport, 1024, metric)`로 비교하는 gate를 C perf에
추가하고 unit test를 둔다. 판정은 평균으로 나쁜 cell을 숨기지 않고 각 cell에 적용한다.

- Throughput·bandwidth처럼 높을수록 좋은 값: `candidate / baseline >= 0.95`
- Latency처럼 낮을수록 좋은 값: `candidate / baseline <= 1.05`
- Baseline 0, 누락, 중복 cell, unexpected skip/fail: gate 실패

경계 밖으로 실패한 cell은 같은 조건의 5-run pair를 두 번 더 실행하고 median-of-medians로
환경 변동과 반복 회귀를 구분한다. 결과를 고르거나 실패 cell만 다른 옵션으로 실행하지 않는다.
반복해도 5%를 넘게 나빠지면 release를 중단하고 profiler로 원인을 찾은 뒤 Phase 4의 성능
리팩토링 규칙을 적용한다.

### Phase 6 — First-party binding 반영

Binding code나 perf를 build하기 전에 개발 중인 Core candidate의 header와 runtime을 정식
local-development 동기화 경로로 배치한다. 이 단계는 release 전이므로 검증된 release
prefix인 척 `ZLINK_CORE_PACKAGE_PREFIX`를 설정하지 않고 `core/build/lib`와 `core/include`를
사용한다. Phase 10에서는 반대로 release provenance가 있는 absolute prefix만 허용한다.

```bash
unset ZLINK_CORE_PACKAGE_PREFIX
cmake --build core/build -j2
scripts/local-package/native/sync-local-core-libs.sh \
  c cpp dotnet go java node python rust
```

동기화 script는 C·C++·Go·Rust raw header mirror와 각 언어의 Linux native runtime을 갱신한다.
다른 언어의 수동 FFI declaration은 struct layout·enum·symbol contract test로 대조한다. 다음
gate는 x86_64 WSL에서 script가 배치하는 모든 복제본이 방금 build한 동일 runtime임을
byte-for-byte로 검증한다.

```bash
zlink_core_dev_version="$(bash core/version.sh)"
zlink_core_dev_runtime="$(readlink -f core/build/lib/libzlink.so)"
zlink_binding_runtime_dirs=(
  bindings/cpp/native/linux-x64
  bindings/cpp/native/linux-x86_64
  bindings/dotnet/native/linux-x64
  bindings/go/native/linux-x64
  bindings/go/native/linux-x86_64
  bindings/java/native/linux-x64
  bindings/java/native/linux-x86_64
  bindings/java/src/main/resources/native/linux-x64
  bindings/java/src/main/resources/native/linux-x86_64
  bindings/node/native/linux-x64
  bindings/node/native/linux-x86_64
  bindings/node/prebuilds/linux-x64
  bindings/python/src/zlink/native/linux-x86_64
  bindings/rust/native/linux-x64
  bindings/rust/native/linux-x86_64
)
for zlink_binding_runtime_dir in "${zlink_binding_runtime_dirs[@]}"; do
  zlink_binding_runtime="${zlink_binding_runtime_dir}/libzlink.so.${zlink_core_dev_version}"
  test -f "$zlink_binding_runtime"
  cmp -s "$zlink_core_dev_runtime" "$zlink_binding_runtime"
done
sha256sum "$zlink_core_dev_runtime"

zlink_header_mirror_dirs=(
  bindings/c/include bindings/cpp/include bindings/go/include bindings/rust/include
)
while IFS= read -r zlink_core_header; do
  zlink_header_relative="${zlink_core_header#core/include/}"
  for zlink_header_mirror_dir in "${zlink_header_mirror_dirs[@]}"; do
    cmp -s "$zlink_core_header" \
      "${zlink_header_mirror_dir}/${zlink_header_relative}"
  done
done < <(rg --files core/include -g '*.h')
```

배치된 native binary는 개발·검증 입력이지 commit 대상이 아니다. Staging 전 `git status`로
해당 파일을 분리하고, 동기화 전부터 있던 user dirty 파일을 일괄 restore하지 않는다.

Binding은 언어의 Task·Future·Promise·coroutine·Context 표면을 유지하되 native callback과 Core
cancel에 의존하지 않는다. 각 socket owner가 poller readiness를 받고 completion queue를 DONTWAIT로
끝까지 비운다. Caller-side wait cancellation 뒤에도 늦은 native completion은 drain·cleanup한다.
Awaitable send/request, blocking request와 Go `Submit(context.Context)`는 native `FINAL`
전에 stable context-keyed provisional state를 registry에 등록한다. Submit 실패 ID 0은
unregister 후 exact error, successful send ID 0은 inline success, successful request와 그 밖의
nonzero ID는 이미 등록한 state에 atomically publish한다. 다른 drain owner가 native
submit 반환 전 completion을 꺼내면 context로 result/ownership을 capture하고, submit
outcome publish와 합류한 뒤에만 user-visible terminal을 exactly once settle·remove한다.
Completion이 없는 blocking send/reply는 이 registry에 등록하지 않는다.
Pre-call cancellation은 Core를 호출하지 않고, native call 중 cancellation은 반환까지
pending claim으로 두어 synchronous submit failure가 우선하게 한다. Successful submit 후
cancellation/drop이 이기면 waiter만 한 번 canceled/detached하고 state는 late completion
또는 lifecycle cleanup까지 유지한다. Late result와 cancellation이 이미 이긴 ID0
success는 waiter를 재-settle하지 않는다. Socket/context 종료는 live waiter만
shutdown/terminated로 끝내고 모든 registry state를 정리한다. Go request non-OK는
`(nil, typed request error)`, Context cancellation 승자는 `(nil, ctx.Err())`로 고정한다.

Binding public API는 확정 draft §11을 규범적 입력으로 삼아 다음 표면으로
고정한다. 이 phase에서 flags 존치, callback 호환 overload, completion channel 또는
alias를 다시 선택하지 않는다.

| Binding | 0.16.0 send terminal | 0.16.0 request terminal | Native completion 노출 |
|---|---|---|---|
| C | `zlink_send_part*()` direct `flags`·`user_context`·ID output | `zlink_request_part()` direct `flags`·`timeout`·`user_context`·ID output | Public `zlink_completion_recv/close` |
| C++ | `void submit() &&`, `async_result_t<void> async() &&` | `vector<message_t> submit() &&`, `async_result_t<vector<message_t>> async() &&` | ID·context·raw drain은 internal |
| .NET | `SendSubmitOperation`: `void Submit()`, `Task Async(CancellationToken)` | `RequestSubmitOperation`: `IReadOnlyList<Message> Submit()`, `Task<IReadOnlyList<Message>> Async(CancellationToken)` | internal |
| Java/Kotlin | `CompletionStage<Void> submit()`, `void submit_sync()` | `CompletionStage<List<Message>> submit()`, `List<Message> submit_sync()` | internal |
| Node | `Promise<void> submit()`, `void submit_sync()` | `Promise<Message[]> submit()`, `Message[] submit_sync()` | internal |
| Python | `Awaitable[None] submit()`, `None submit_sync()` | `Awaitable[list[Message]] submit()`, `list[Message] submit_sync()` | internal |
| Go | `Submit(context.Context) error` | `Submit(context.Context) ([]*Message, error)` | internal; Core DONTWAIT 후 goroutine wait |
| Rust | `Future<Result<(), SubmitError>> submit()`, `Result<(), SubmitError> submit_sync()` | `Future<Result<Vec<Message>, ZlinkError>> submit()`, `Result<Vec<Message>, ZlinkError> submit_sync()` | internal |

Socket이 operation builder를 만드는 exact 이름은 다음과 같이 고정한다. Go만 overload 대신
기존 `SendTo`를 유지한다.

| Binding | PAIR | DEALER | ROUTER | STREAM |
|---|---|---|---|---|
| C++ | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| .NET | `Send()` | `Send()`, `Request()` | `Send(rid)`, `Request(rid)`, `Reply(rid, token)` | `Send(rid)` |
| Java/Kotlin | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Node | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Python | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Go | `Send()` | `Send()`, `Request()` | `SendTo(rid)`, `Request(rid)`, `Reply(rid, token)` | `SendTo(rid)` |
| Rust | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |

모든 언어의 `Received.send/Send()`와 `Received.reply/Reply()` 편의 builder는 유지한다. DATA에서
reply를 요청하면 language invalid-state로 실패한다. Reply token과 STREAM exact 표면은 다음
이름으로 구현하고 세부 class signature는 draft §11.3~§11.9를 그대로 따른다.

| Binding | Reply token accessor | STREAM mode option | Packet output / recv |
|---|---|---|---|
| C++ | `received_t::reply_token()` | `stream_socket_options_t::recv_mode()` getter/setter | `stream_packet_t`, `recv_packet(out, flags)` |
| .NET | `Received.ReplyToken` | `StreamSocketOptions.ReceiveMode` | `StreamPacket.Create()`, `IStreamSocket.RecvPacket(result, flags)` |
| Java/Kotlin | `Received.replyToken()` | `StreamSocketOptions.recvMode()` getter/setter | `new StreamPacket()`, `StreamSocket.recvPacket(out, flags)` |
| Node | `Received.replyToken` | `StreamSocketOptions.recvMode` | `new StreamPacket()`, `StreamSocket.recvPacket(out, flags)` |
| Python | `Received.reply_token` | `StreamSocketOptions.recv_mode` | `StreamPacket()`, `StreamSocket.recv_packet_into(out, flags=...)` |
| Go | `Received.ReplyToken()` | `ReceiveMode()` / `SetReceiveMode()` | zero-value `StreamPacket`, `RecvPacket(out, flags)` |
| Rust | `Received::reply_token()` | `recv_mode()` / `set_recv_mode()` | `StreamPacket::default()`, `recv_packet(out, flags)` |

`ReplyToken`은 `(responder socket instance, opaque value)` equality/hash만 제공하고 public numeric
constructor·raw accessor·serialization·ordering·close를 제공하지 않는다. 다른 socket owner token과
언어에서 만들 수 있는 zero/default invalid token은 native 호출 전에 거부한다. `StreamPacket`은
empty reusable output이며 recv 진입 시 이전 payload를 먼저 정리한다. 성공하면 owned RID copy와
header/body를 소유하고 `NO_DATA`·오류면 empty다. 같은 output의 concurrent recv는 invalid-state이고
message reference는 다음 recv/close 전까지만 유효하다. Rust의 consuming `close(self)`만 재사용하지
않고 나머지 언어의 `close/Dispose`는 idempotent reset 뒤 재사용을 허용한다.
C++·Rust의 token owner는 ROUTER wrapper가 생성 시 만드는 heap owner tag
(C++ `std::shared_ptr<const void>`, Rust `Arc`)이고 token이 tag를 공유 보유해
close/recreate 뒤 주소 재사용 ABA가 없다. 별도 nonce 발급·process-lifetime 재사용
금지·소진 오류 규칙은 두지 않는다.

Reply는 C의 `zlink_reply_part(router, source_rid, reply_token, part, part_flag)`와 모든
고수준 binding의 flag-없는 `ReplySubmitOperation.Submit/submit()`으로 고정한다. C++·Go·
Node·Python·Rust reply builder의 `flags` step을 제거하고, .NET·Java/Kotlin의 기존
flag-없는 terminal은 유지한다. Reply FINAL은 socket `SNDTIMEO`를 따르는 synchronous
logical-route admission이다.

C++·.NET·Java/Kotlin·Node·Python·Rust의 sync terminal은 Core `NONE`, awaitable
terminal은 Core `DONTWAIT`를 사용한다. Go는 단일 `Submit(context.Context)`에서
Core `DONTWAIT`로 접수한 뒤 internal completion을 기다린다. 모든 고수준 binding에서
send/request flags, reply flags, send operation timeout, request callback terminal을 제거한다. Publish flags와
request reply timeout은 별도 계약이므로 유지한다. Publish가 공용 send operation type을
재사용하는 Go·Python은 draft §11.7·§11.8의 별도 `PublishOp`로 분리해 기존 publish
flags·submit 의미를 보존한다.

모든 binding은 언어별 opaque `ReplyToken`을 추가하고 public numeric constructor·raw
변환을 두지 않는다. `request_seq` 표면은 `reply_token`으로 바꾸고 ROUTER REQUEST
recv만 token을 만든다. Internal 생성은 draft §11.1의 C++ friend, .NET internal constructor,
Java non-exported `ContractAccess`, Node module-private closure, Python module-private factory,
Go unexported literal, Rust `pub(crate) from_native` 경로로만 구현하고 public test/raw factory를
만들지 않는다. STREAM은 `UNSPECIFIED/RAW/PACKET` mode, mode getter/setter,
reusable packet output과 packet recv를 추가한다. Setter는 `UNSPECIFIED`를 거부하고
첫 bind/connect 전 `RAW`·`PACKET`만 받는다.

C++·.NET·Go·Java/Kotlin·Node·Python·Rust에서 routed/non-routed send operation
type을 각각 하나의 `SendOperation` family로 통합하고 target은 socket의 `send(...)`가
만든 builder 내부에 capture한다. 기존 언어별 message ownership은 유지하여
lvalue·managed input의 submit 실패 복구와 rvalue·move input 소비 규칙을 바꾸지 않는다.

`PollCompletion`은 C에서는 non-consuming level readiness이고 고수준 binding에서는 public
completion progress event다. Public poller에 socket을 등록하면 그 wait thread가 native Core
readiness를 관찰하고 queue를 `NO_DATA`까지 비워 live waiter settle 또는
detached state cleanup을 끝낸 뒤, 실제 한 건 이상 완전 처리했을 때만 event를
반환한다. Cleanup-only event는 public waiter의 새 상태 변화를 보장하지 않고,
pre-return capture는 submit publish와 join/cleanup 전 event를 반환하지 않는다. 따라서 event
뒤 raw completion recv가 성공한다는 뜻이 아니며
`POLLIN` DATA도 소비하지 않는다. 미등록 socket은 binding runtime owner가 drain하며 등록·수정·제거에서
owner를 원자적으로 이전한다. Public poller가 owner인 동안에는 모든 completion-backed
terminal이 그 poller의 `wait()` drain에 의존한다. Blocking request와 Go `Submit(ctx)`를
사용할 때는 다른 thread·goroutine이 지속적으로 `wait()`해야 하며 같은 실행 thread에서
두 호출을 직렬로 사용하지 않는다. Binding은 blocking terminal에 owner를 몰래 이전하거나
helper drain thread를 만들지 않는다. Monitor·timer의 기존 create/open·recv·status·start·stop·
close/destroy/Drop lifecycle은 유지하고 handler 등록만 제거한다. 언어별 exact method는
draft §11.10을 따른다.

Pending option rename은 Core enum과 native header mirror·수동 FFI/internal generated constant에만
반영한다. `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`의 값은 `0x303A/0x303B`이고 old
`SEND_PENDING` 이름은 남기지 않는다. 0.15에 없던 고수준 option façade를 새로 만들지 않는다.

0.15 compatibility shim·alias·dual ABI를 두지 않고 모든 0.16.0 binding package가
Core 0.16.0 exact dependency를 갖게 한다.

| Binding | 주요 변경 경로 | 핵심 확인 |
|---|---|---|
| C | `bindings/c/include/zlink/socket/api.h`, `eventing/api.h`, `zlink_enum.h` | Core header mirror, raw pull example |
| C++ | `bindings/cpp/include/`, `src/Runtime/Messaging/send_operations.cpp`, `async_operation_state.hpp` | callback registry 제거, poller drain과 Future correlation |
| .NET | `NativeMethods.Socket.cs`, `NativeMethods.Core.cs`, `NativeTypes.cs`, `SendCompletionRegistry.cs` | P/Invoke completion layout, `CancellationToken`과 Core ownership 분리 |
| Go | header mirror, native contract와 operation code | Context가 Core cancel을 호출하지 않고 drain owner가 cleanup |
| Java/Kotlin | `Native.java`, `SendCompletionRegistry.java`, async contracts·enum codec | FFM layout, CompletableFuture/coroutine와 pull loop |
| Node | `native/src/addon_core.cc`, `runtime/messaging/send_completion.ts`, operation contracts | addon callback bridge 제거, Promise state와 drain |
| Python | `_native/ffi.py`, `_runtime/messaging/routed_async.py`, socket base | FFI layout, Future wait cancellation 뒤 cleanup |
| Rust | `runtime/native/ffi.rs`, `send_ops.rs`, `internal/send_completion.rs`, handle storage | Future registry, Drop에서 Core cancel 제거, reply ownership |

각 binding은 다음을 함께 고친다.

- 위 표의 exact send/request public signature와 completion tagged record FFI
- Go·Python publish의 별도 `PublishOp` 분리와 기존 publish flags·submit 의미 보존,
  .NET `IStreamSocket.RecvPart`의 `Recv(Received, RecvFlags)` 대체
- STREAM RAW/PACKET option과 packet recv
- Monitor·timer·recv callback 제거와 exact pull wrapper
- Exact pair/generation type·field·method 제거
- `ReplyToken` strong type과 request sequence rename
- ROUTER recv-only internal token factory, same-owner equality/hash·different-owner inequality,
  default/zero invalid와 raw conversion·serialization 부재
- C++·Rust shared owner tag의 wrapper move 후 유지, ROUTER close/recreate 뒤 stale token의
  owner 불일치·reply pre-native 거부
- Python `ReplyToken`의 public construction 실패와
  `copy.copy()`·`copy.deepcopy()`가 반환한 동일 valid token의 reply 허용
- Operation factory 반환 type과 유지할 모든 builder method—Python `message/messages` 포함—의
  compile/contract test
- `StreamPacket` success 뒤 reuse reset, `NO_DATA`/error empty·concurrent output rejection test.
  C++·.NET·Java/Kotlin·Node·Python·Go는 close/Dispose idempotent reset·close 후 reuse,
  Rust는 consuming `close(self)`의 populated/empty exactly-once cleanup·error 후 Drop 이중 해제 부재,
  close 없이 Drop cleanup·use-after-close/double-close compile-fail test
- Public `PollCompletion` owner-transfer·drain·settlement test
- Public poller owner에서 다른 thread·goroutine의 `wait()`가 blocking request·Go `Submit(ctx)`를
  진행시키고, `wait()`가 없으면 queue를 임의로 훔쳐 drain하지 않는 progress test
- Awaitable send/request·blocking request·Go `Submit(ctx)` state/context를 native FINAL 전
  provisional 등록하고 submit 실패 ID 0은 exact error·completion 없음, successful send
  ID 0은 inline success, successful request는 nonzero, nonzero ID는 submit 반환 전 eager
  drain을 binding-internal barrier로 강제해 2-phase publish/capture·exactly-once settle하는 race test
- C의 non-OK error-reply payload는 raw completion으로 유지하되, 나머지 binding은
  새 payload accessor 없이 기존 typed request error만 노출한다. 2-part 이상 error
  payload·empty error·pre-return drain·wait cancellation/Future drop·public poller·wrapper allocation
  실패에서 user settle 전 `zlink_completion_close()`로 native part/array를, language
  RAII/finally로 partial wrapper를 leak·double-close 없이 정리하는 ASan/언어 allocator test
- Public API·contract test·sample·README·perf의 stale symbol
- Native struct size·alignment·offset과 enum value test

Binding에 자체 retransmit queue, POLLOUT retry loop, operation deadline thread 또는 Core cancel
대체물을 만들지 않는다. Core가 retry를 소유하고 binding은 language operation state와 drain만
소유한다.

Binding별 focused test 뒤 전체 runner를 실행한다.

```bash
bindings/c/tests/run_tests.sh
bindings/cpp/tests/run_tests.sh
bindings/dotnet/tests/run_tests.sh
bindings/go/tests/run_tests.sh
bindings/java/tests/run_tests.sh
bindings/node/tests/run_tests.sh
bindings/python/tests/run_tests.sh
bindings/rust/tests/run_tests.sh
```

기능 test가 green이면 binding 경계마다 POSDDD scan을 수행한다. 같은 completion drain을 언어별
호출부에 반복하지 않고 runtime owner 한 곳에 모으며, 제거한 callback registry·cancel map·unused
FFI·generated stale mirror를 정리한다. Public language abstraction과 native adapter가 같은 인자를
그대로 넘기는 얕은 layer라면 합치거나 실제 language lifetime 변환 책임을 명확히 한다. Refactor
뒤 해당 binding runner를 다시 실행한다.

### Phase 7 — Binding perf 전체 smoke

모든 perf는 public binding API만 사용한다. 각 runner의 현재 `--help`와 지원 pattern manifest를
먼저 확인하고, Phase 6에서 확정한 Core runtime absolute path·version·SHA-256를 runner가 실제
load한 값과 대조한다. 이 metadata를 출력하지 않거나 stale runtime을 허용하는 runner는 perf
전에 수정하고 runner contract test를 추가한다. Node runner의 `--help`는 incremental build를
수행할 수 있으므로 read-only 조사로 가정하지 말고 동기화 뒤 build-capable preflight로 한 번만
실행한다. 모든 언어에서 single은
해당 언어가 지원하는 모든 pattern과 Linux transport `tcp,tls,ws,wss,inproc,ipc`, multi는 모든
지원 pattern과 `tcp,tls,ws,wss`를 1024 byte로 실행한다. `UNSUPPORTED`는 언어별 manifest와
일치할 때만 허용하며 unexpected skip/fail/hang은 실패다.

API 변경과 함께 확인할 perf source는 다음과 같다. 실제 symbol 검색으로 영향 파일을 더 좁히되,
benchmark 전용 native 우회 경로는 만들지 않는다.

| Binding | perf 변경·검토 경로 |
|---|---|
| C | `bindings/c/perf/`, 특히 multi STREAM server·공통 session |
| C++ | `bindings/cpp/perf/multi/src/perf_stream_server.cpp`, single·multi runner |
| .NET | `bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfSocketIo.cs`, multi STREAM server |
| Go | `bindings/go/perf/multi/perf_multi_stream.go`, single·multi runner |
| Java/Kotlin | `bindings/java/perf/multi/`, routed coordinator와 STREAM server |
| Node | `bindings/node/perf/multi/perf_multi_stream_server.ts`, generated tools mirror |
| Python | `bindings/python/perf/multi/perf_multi_stream_server.py`, routed sender |
| Rust | `bindings/rust/perf/multi/src/perf_multi_stream_server.rs`, send operation path |

아래 명령의 공통 smoke 조건은 `--duration 1 --runs 1`, multi는 `--clients 1`이다. Go·Rust와
지원하는 Python runner에는 `--smoke`도 사용한다.

```bash
# C
bindings/c/perf/run_benchmarks.sh --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 --duration 1 --runs 1
bindings/c/perf/run_benchmarks_multi.sh --pattern ALL --transports tcp,tls,ws,wss \
  --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# C++
bindings/cpp/perf/run_benchmarks.sh --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 --duration 1 --runs 1
bindings/cpp/perf/run_benchmarks_multi.sh --pattern ALL --transports tcp,tls,ws,wss \
  --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# .NET
bindings/dotnet/perf/run_benchmarks.sh --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 --duration 1 --runs 1
bindings/dotnet/perf/run_benchmarks_multi.sh --pattern ALL --transports tcp,tls,ws,wss \
  --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# Go
bindings/go/perf/run_benchmarks.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1
bindings/go/perf/run_benchmarks_multi.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# Java/Kotlin
bindings/java/perf/run_benchmarks.sh --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 --duration 1 --runs 1
bindings/java/perf/run_benchmarks_multi.sh --pattern ALL --transports tcp,tls,ws,wss \
  --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# Node
bindings/node/perf/run_benchmarks.sh --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc \
  --msg-sizes 1024 --duration 1 --runs 1
bindings/node/perf/run_benchmarks_multi.sh --pattern ALL --transports tcp,tls,ws,wss \
  --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# Python
bindings/python/perf/run_benchmarks.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1
bindings/python/perf/run_benchmarks_multi.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1

# Rust
bindings/rust/perf/run_benchmarks.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1
bindings/rust/perf/run_benchmarks_multi.sh --smoke --pattern ALL \
  --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1
```

언어마다 지원 pattern 수가 다르므로 C matrix를 다른 binding에 강제로 맞추지 않는다. 완료 보고는
언어별 expected cell 수, pass, expected unsupported, unexpected skip/fail과 report 경로를 따로
기록한다.

Binding code·test·perf와 리팩토링이 green이면 관련 변경만 commit하고 push한다.

```text
bindings: consume pull completion and stream packet APIs
```

### Phase 8 — Core·binding 사용자 가이드 반영

구현과 test가 확정된 뒤에만 가이드를 고친다. 먼저 `rg`로 실제 stale API가 있는 문서를 좁히고
관련 챕터만 수정한다.

Core 반영 대상:

- `core/doc/guide/`
- `core/doc/reference/03-socket-lifecycle.{ko,en}.md`
- `core/doc/reference/06-pair.{ko,en}.md`
- `core/doc/reference/13-stream.{ko,en}.md`
- Core README와 public example

Bindings 반영 대상:

- `bindings/doc/guide/{cpp,dotnet,go,java,node,python,rust}/`
- `bindings/doc/reference/{c,cpp,dotnet,go,java,node,python,rust}/`
- C는 `bindings/doc/reference/c/README.{ko,en}.md`와 `bindings/c/samples/`;
  그 밖은 각 binding README, Go godoc, Java/Kotlin·Node/JavaScript
  guide와 sample

가이드는 다음 사용 판단을 설명한다.

- 일반 send 하나에서 blocking과 nonblocking/awaitable을 고르는 방법
- Backpressure 재전송은 Core가 처리하므로 caller가 retry queue를 만들지 않는 이유
- C의 `PENDING_MAX_MSGS/BYTES`로 DONTWAIT SEND·REQUEST 공유 pending memory를 제한하는 방법과
  구 `SEND_PENDING` 이름이 0.16.0에 없는 이유
- Admission completion과 peer delivery의 차이
- Poller readiness 뒤 completion queue를 비우는 C 예제
- STREAM RAW와 PACKET 중 첫 bind/connect 전에 하나를 고르는 기준
- Language cancellation은 submit 전·Framework queue·Core ownership 뒤에 의미가 달라지는 경계
- Request reply는 DATA recv가 아니라 completion/Task/Future로 도착하는 경로
- Public `PollCompletion`을 직접 소유할 때 awaitable·blocking request 진행을 위해
  `wait()` loop를 계속 실행해야 하는 규칙

예제는 실제 sample 또는 contract fixture에서 컴파일·실행되는 호출을 가져온다. 가이드별 스펙·
interface index·검증 class 링크와 한국어·영어 parity를 확인하고 독립 2축 review, link·render·
`git diff --check`를 통과시킨다. 문서 commit은 code commit과 분리해 push한다.

```text
docs: update core and binding pull API guides
```

### Phase 9 — 0.16.0 version과 Core release

Core 전체 test, C 5% performance gate, 모든 binding test·perf smoke와 Core·binding guide review가
통과한 뒤에만 version을 바꾼다. 원본은 root 두 파일이다.

```text
VERSION:
  LIBZLINK_VERSION_MAJOR=0
  LIBZLINK_VERSION_MINOR=16
  LIBZLINK_VERSION_PATCH=0
  LIBZLINK_VERSION=0.16.0

BINDINGS_VERSION:
  ZLINK_BINDINGS_VERSION=0.16.0
```

Manifest를 손으로 흩어 고치지 않고 공식 동기화 경로를 사용한다.

```bash
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions
rg -n "0\.15\.1|zlink_send_async|zlink_send_async_cancel|zlink_send_(async_)?options_t|zlink_request_options_t|zlink_send_complete_handler|zlink_reply_handler_fn|zlink_stream_packet_handler|transport_pair_(id|generation)|ZLINK_OPT_SEND_PENDING_MAX_(MSGS|BYTES)|RoutedSend|RequestCallback|RequestReplyCompletion|request_seq|RequestSeq|TrySend|send_async|sendAsync|onPacket|on_packet|onEvent|on_event|onFire|on_fire" \
  VERSION BINDINGS_VERSION core bindings scripts \
  -g '!**/build/**' -g '!**/target/**' -g '!**/dist/**' -g '!**/node_modules/**'
```

남은 이전 version이나 symbol은 historical fixture인지 누락인지 파일별로 판정한다. Version test와
public header/export 검사를 다시 실행한다. Release commit에는 요청 범위 파일만 stage하고 생성된
artifact·perf report·다른 dirty 변경을 넣지 않는다.

```text
release: prepare core and bindings 0.16.0
```

Commit을 `main`에 push한 뒤 그 exact commit에 tag를 만든다.

```bash
git tag core/v0.16.0 <release-commit>
git push origin core/v0.16.0
GH_REPO=zlink-systems/zlink gh workflow run build.yml \
  --ref core/v0.16.0 -f libzlink_version=0.16.0
GH_REPO=zlink-systems/zlink gh release view core/v0.16.0
```

Release asset, checksum과 provenance가 확인되기 전에는 local binding package나 Framework 단계로
넘어가지 않는다.

### Phase 10 — Binding package 생성과 로컬 배포

Core source local fallback은 사용하지 않는다. `scripts/local-package/README.ko.md`에 따라 release
Core를 내려받아 검증한 뒤 package 여덟 개를 만든다.

```bash
scripts/local-package/build-wsl.sh --verify-versions
scripts/local-package/build-wsl.sh c cpp dotnet go java node python rust
```

확인할 local artifact:

| Binding | 0.16.0 artifact |
|---|---|
| C | `.artifacts/wsl/c/zlink-c-0.16.0.tar.gz` |
| C++ | `.artifacts/wsl/install/zlink-cpp/0.16.0/` |
| .NET | `.artifacts/wsl/nuget/Systems.Zlink.0.16.0.nupkg` |
| Go | `.artifacts/wsl/go/zlink-go-0.16.0.tar.gz` |
| Java | `.artifacts/wsl/maven/systems/zlink/zlink/0.16.0/` |
| Node | `.artifacts/wsl/npm/zlink-systems-zlink-0.16.0.tgz` |
| Python | `.artifacts/wsl/python/zlink-0.16.0-*.whl`과 source archive |
| Rust | `.artifacts/wsl/rust/zlink-0.16.0.crate` |

각 package의 clean consumer smoke에서 binding version `0.16.0`, loaded Core version `0.16.0`,
release provenance와 실제 native library 경로를 기록한다. Package 생성 결과는 외부 registry에
publish하지 않는다.

### Phase 11 — Framework package 전환과 구현

Framework는 binding source나 `core/build`를 직접 참조하지 않는다. Version sync가 갱신한 다음
package pin과 local artifact를 확인한다.

| 언어 | package pin |
|---|---|
| C++ | `framework/languages/cpp/CMakeLists.txt`의 Core/C++ version |
| .NET | `framework/languages/dotnet/Directory.Packages.props`의 `ZLinkBindingsPackageVersion` |
| Java/Kotlin | `framework/languages/java/gradle/libs.versions.toml`의 `zlinkBindings` |
| Node | `framework/languages/node/package.json`의 local `.tgz` dependency |

Framework runtime 변경:

1. C++·.NET·Java·Kotlin·Node의 Core notification callback bridge를 native Core poller readiness + drain loop로
   바꾼다. Core callback thread에서 application handler를 실행하는 경로를 남기지 않는다.
   Binding request callback·routed send type·public flags 호출은 각 언어의 확정
   `SendOperation` sync/awaitable terminal과 request sync/awaitable terminal로 바꾸고, ROUTER
   request recv에서 받은 opaque `ReplyToken`을 reply builder에 그대로 전달한다.
2. STREAM host는 bind 전에 PACKET mode를 설정하고 `zlink_stream_recv_packet()`을 호출한다.
   C++·.NET·Java·Node에 중복된 `[u16 BE][u32 BE]` assembler는 public behavior를 보존하면서
   제거한다.
3. Framework managed queue permit을 먼저 확보한 뒤 Core에서 packet 한 건을 꺼낸다. Queue에 넣을
   수 없는데 packet을 계속 drain해 Core RCVHWM backpressure를 무력화하지 않는다.
4. Monitor callback 사용 경로를 monitor handle의 poller+`zlink_socket_monitor_recv()`로 바꾼다.
   Framework에 timer callback 사용이 새로 생기지 않게 한다.
5. Framework application handler는 managed queue와 serial execution gate 뒤에서 그대로 호출한다.
   Core callback 제거를 public Framework handler 제거로 확대하지 않는다.
6. Core ownership 전 cancellation과 Framework queue wait cancellation은 유지한다. Core submit 뒤
   caller wait만 canceled가 될 수 있고 늦은 completion은 drain·dispose한다. Formal Framework
   spec과 test에서 “cancellation 뒤 late admission 없음” 보장을 제거한다.
7. Framework request 오류는 고수준 binding의 기존 typed error만 받고 Core/C의
   non-OK error-reply payload를 해석하거나 별도 public accessor로 우회하지 않는다. Error
   payload native ownership은 binding drain adapter가 user-visible settle 전 정리한다.
8. C++ unit·E2E·cross·sample이 하나의 configured build directory를 쓰도록 모든 공통
   runner와 child runner가 `ZLINK_CPP_BUILD_DIR`를 우선하게 맞춘다. 현재 `build`를
   hard-code하거나 다른 `BUILD_DIR`만 받는 runner도 포함하며 runner contract test로 검증한다.
   Clean candidate build를 독립 임시 경로에서 재구성하는 `SubmitAdmission`만 의도된 예외로
   남기고, 같은 0.16.0 package·commit·provenance를 쓰는지 따로 확인한다.

주요 STREAM 경로:

- C++: `framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp`
- .NET: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkStreamNodeRuntime.cs`,
  `ZLinkStreamReceiveBuffer.cs`
- Java: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java`,
  같은 directory의 `ZLinkStreamReceiveBuffer.java`
- Node: `framework/languages/node/packages/framework/src/runtime/streams/stream-session-runtime.ts`

Framework focused test가 green이면 언어별 runtime 경계에서 POSDDD scan을 수행한다. 중복 assembler,
callback adapter, pass-through completion wrapper, unused cancellation registry와 dead code를 제거한다.
Performance trace로 packet당 allocation·copy·lock이 증가하지 않았는지 확인하고, managed queue
backpressure와 ownership을 한 runtime owner 안에 둔다. Refactor 뒤 focused unit test를 다시
실행한다.

### Phase 12 — Framework unit·E2E·cross-language 검증

#### 12.0 C++ clean configure와 단일 build directory

Clean workspace에서 `CMakePresets.json`의 Linux vcpkg Debug preset을 사용한다. `VCPKG_ROOT`와
Phase 10의 C++ 0.16.0 local install을 먼저 확인하고 test·sample·E2E target을 모두 켠다.
아래 `ZLINK_CPP_BUILD_DIR`는 같은 shell에서 12.1~12.3과 Phase 13 sample까지 유지한다.

```bash
test -n "${VCPKG_ROOT:-}"
test -d .artifacts/wsl/install/zlink-cpp/0.16.0

(cd framework/languages/cpp && \
  cmake --preset linux-ninja-vcpkg-debug \
    -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON \
    -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=ON \
    -DZLINK_FRAMEWORK_CPP_BUILD_E2E=ON)

zlink_cpp_framework_build_dir="$(realpath \
  framework/languages/cpp/build/linux-ninja-vcpkg-debug)"
export ZLINK_CPP_BUILD_DIR="$zlink_cpp_framework_build_dir"
cmake --build "$ZLINK_CPP_BUILD_DIR" -j2

cmake -LA -N "$ZLINK_CPP_BUILD_DIR" | \
  rg 'ZLINK_FRAMEWORK_CPP_(BUILD_TESTS|BUILD_SAMPLES|BUILD_E2E)|ZLINK_FRAMEWORK_CPP_(ZLINK_CPP_VERSION|LOCAL_ZLINK_CPP_PREFIX)'
```

Configure log에서 C++ binding version이 0.16.0이고 prefix가 Phase 10 artifact임을 확인한다.
Aggregate E2E가 호출하는 child runner도 `ZLINK_CPP_BUILD_DIR`를 실제 binary·`cmake --build`
경로에 사용하는지 runner contract test로 먼저 검증한다.

#### 12.1 Unit test

```bash
ctest --test-dir "$ZLINK_CPP_BUILD_DIR" \
  -L framework-unit --output-on-failure

dotnet test \
  framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj

(cd framework/languages/java && \
  ./gradlew --no-daemon :zlink-framework-core:test)

(cd framework/languages/node && npm test)
```

C++ build의 contract/foundation target도 포함됐는지 test 목록으로 확인한다. Kotlin은 Java binding과
runtime을 공유하지만 Kotlin exact interface·coroutine fixture를 별도로 확인한다.

#### 12.2 언어별 E2E

```bash
ZLINK_CPP_BUILD_DIR="$ZLINK_CPP_BUILD_DIR" \
  bash framework/languages/cpp/e2e/run_e2e_all.sh
bash framework/languages/dotnet/e2e/run_e2e_all.sh
bash framework/languages/java/e2e/run_e2e_all.sh
bash framework/languages/java/e2e-kotlin/run_e2e_all.sh
bash framework/languages/node/e2e/run_e2e_all.sh
```

#### 12.3 Cross-language E2E

Java host를 먼저 다시 만든다.

```bash
(cd framework/languages/java && \
  ./gradlew --no-daemon -p cross-language :Host:installDist)

ZLINK_CPP_BUILD_DIR="$ZLINK_CPP_BUILD_DIR" \
  bash framework/languages/cpp/cross-language/run_cross_language_smoke.sh
bash framework/languages/node/cross-language/run_cross_language_smoke.sh
```

C++ runner는 C++↔.NET, C++↔Node, Java cross와 relocation/user-spot-join stage를 포함한다. 첫
실패부터 flow file과 run directory를 보존한다. 통과 기대치를 낮추거나 timeout을 늘려 결함을
숨기지 않는다.

Framework code·test·refactor와 문서가 green이면 검토 가능한 단위로 commit하고 `main`에 push한다.

```text
framework: consume zlink 0.16.0 pull APIs
```

### Phase 13 — Framework 가이드와 공통 sample

Framework public 동작이 달라지는 cancellation·STREAM guide와 language reference를
`guide-writing-guide.ko.md`에 따라 갱신한다. 반영 대상은 다음과 같다.

- `framework/doc/framework/common/guide/server/09-stream.{ko,en}.md`
- `framework/doc/framework/{cpp,dotnet,java,kotlin,node}/guide/server/09-stream.{ko,en}.md`
- `framework/doc/framework/{cpp,dotnet,java,kotlin,node}/reference/06-stream-session.{ko,en}.md`
- `framework/doc/framework/*/guide/stream-connector/`

Public signature가 바뀌지 않은 문서는 내부 Core 전환 이력을 쓰지 않는다. 사용자에게 보이는
cancellation 경계나 STREAM 선택·오류 처리만 설명한다. 실제 sample의 코드를 사용하고 스펙·exact
interface·검증 class 링크, 한국어·영어 parity와 독립 2축 review를 확인한다.

마지막 기능 gate로 다음 공통 sample을 실행한다.

| Sample | 확인할 대표 흐름 |
|---|---|
| `TicTacToe` | session·request/reply |
| `Bingo` | 다중 participant message flow |
| `DeliveryDispatch` | dispatch와 상태 변경 |
| `SupportChat` | stream/session message |
| `GameQuest` | actor·request flow |
| `ShoppingMall` | 복합 service flow |
| `ZoneWorld` | multi-node·routing flow |

각 언어 runner가 위 일곱 sample을 모두 실행하는지 목록을 먼저 확인한 뒤 실행한다.

```bash
ZLINK_CPP_BUILD_DIR="$(realpath \
  framework/languages/cpp/build/linux-ninja-vcpkg-debug)" \
  bash framework/languages/cpp/samples/run_samples.sh
bash framework/languages/dotnet/samples/run_samples.sh
bash framework/languages/java/samples/run_samples.sh
bash framework/languages/node/samples/run_samples.sh
```

Java runner는 Java와 Kotlin sample 집합을 함께 검증한다. “runner 성공”만 기록하지 않고 언어별
sample 이름 일곱 개의 시작·정상 종료를 report에 남긴다. Framework guide·sample 문서 commit은
구현 commit과 분리해 push할 수 있다.

```text
docs: update framework pull and cancellation guides
```

## 6. Commit·push와 refactor 원칙

사용자는 이 계획의 실행 중 적절한 commit·push를 요청했다. 다음 규칙을 적용한다.

- Branch는 `main`을 유지한다. 새 branch 생성·전환·merge는 별도 요청 없이는 하지 않는다.
- Commit 전에 staged 파일을 전부 확인하고 사용자 dirty 변경이나 생성 artifact를 섞지 않는다.
- Red test, 미완성 migration과 서로 다른 layer 변경을 한 commit에 넣지 않는다.
- Formal spec, Core, bindings, guide, release preparation, Framework를 각각 review 가능한 commit으로
  나눈다. 작은 기계적 rename은 소유 phase commit에 포함한다.
- 각 commit 직전에 관련 회귀 test, `git diff --check`, stale symbol 검색과 diff review를 한다.
- Green commit만 `origin/main`에 push한다. Push 실패는 강제로 우회하지 않고 원인을 보고한다.
- Release tag는 모든 사전 gate가 통과한 `release: prepare ... 0.16.0` commit 하나를 가리킨다.
- 외부 binding registry publish, force push, history rewrite와 destructive checkout은 하지 않는다.

POSDDD refactor는 기능이 green인 시점에 Core·bindings·Framework 경계별로 수행한다. 범위를 넓혀
unrelated 모듈을 정리하지 않는다. 불필요 코드 제거는 callback·cancel·pair/generation 전환 때문에
도달하지 않거나 중복된 코드와 test fixture로 제한한다. 성능 변경은 측정 → 대안 비교 → 적용 →
같은 측정 재검증 순서를 지킨다.

## 7. 실패 처리

- 첫 실제 실패에서 원인을 분리하고 unrelated baseline failure를 임의로 수정하지 않는다.
- 간헐 실패는 root `AGENTS.md`의 message tracking·file log 절차를 첫 재현부터 적용한다.
- Performance gate가 실패하면 평균으로 상쇄하거나 unsupported로 바꾸지 않는다. 같은 cell을 같은
  조건으로 재측정하고 profiler 근거가 나온 경로만 고친다.
- Binding perf를 통과시키려고 private Core helper, raw frame 해석, benchmark 전용 queue와 retry를
  추가하지 않는다.
- Framework는 release package만 소비한다. Local Core source나 binding source fallback으로 green
  결과를 만들지 않는다.
- Release asset·checksum·provenance가 확인되지 않으면 package·Framework 단계로 진행하지 않는다.
- 외부 service 상태나 권한 때문에 tag/workflow/push가 실패하면 이미 통과한 로컬 결과와 정확한
  blocker를 보존하고 destructive retry를 하지 않는다.

## 8. 최종 완료 체크리스트

- [ ] 확정 draft의 Core·C ABI·binding 공개 계약에 구현자 선택 항목이 남아 있지 않다.
- [ ] Core·binding·Framework 정식 스펙 한국어·영어 문서가 확정 draft와 일치한다.
- [ ] Core send/request는 options 구조체 없이 direct `flags`·`timeout`·`user_context`·ID
      output을 사용하고, send/request ID output은 optional·validation 전 zeroing이며
      기존 part 입력 소비 계약을 유지한다.
- [ ] `NONE`은 snapshot한 `SNDTIMEO` 동안 같은 logical target admission만 기다리고,
      DONTWAIT의 `PENDING_MAX_MSGS/BYTES`는 SEND·REQUEST 공유 계산·release와 old-alias 부재를
      exact contract test로 고정한다.
- [ ] Public notification callback과 separate send async/cancel·pair/generation API가 없다.
- [ ] Core/C `ZLINK_POLLCOMPLETION` level readiness와 고수준 binding progress event,
      unified SEND/REQUEST drain·single-owner test가 각각 확정 의미로 통과한다.
- [ ] Completion `peer_rid`·terminal mapping, output validation·idempotent close와 error-reply
      base allocation ownership test가 통과한다.
- [ ] 일반 recv output·socket-owned borrowed RID·SUB/XPUB retry와 STREAM 첫 bind/connect 전
      RAW/PACKET mode·첫 성공 뒤 freeze·
      packet output/size/backpressure test가 통과한다.
- [ ] Admission 전 logical-target reconnect와 admission 후 no-replay, DEALER request selection,
      request/reply 방향, reply OOM/runtime/lifecycle·checkout/token 행렬과 reply-token fairness
      test가 통과한다.
- [ ] Core targeted·전체 test와 반복 race test가 통과한다.
- [ ] C STREAM 16-cell matrix가 pass 16, skip/fail 0이다.
- [ ] C single·multi 전체 1024-byte baseline/candidate cell이 throughput·bandwidth -5%, latency +5%
      경계 안에 있다.
- [ ] First-party binding 여덟 개의 contract/unit/sample runner가 통과한다.
- [ ] Binding 여덟 개의 public send/request terminal·`ReplyToken`·STREAM mode·eventing pull·
      `PollCompletion`이 draft §11의 exact 표면과 일치하고 0.15 compatibility alias가 없다.
- [ ] 각 binding의 socket operation 시작 이름, owner-aware opaque `ReplyToken`, reusable
      `StreamPacket` lifecycle이 plan Phase 6 표와 draft §11.3~§11.9에 정확히 일치한다.
- [ ] Public `PollCompletion` owner에서 completion-backed terminal의 progress와 blocking
      request·Go `Submit(ctx)`의 별도 `wait()` thread 규칙이 contract test로 고정됐다.
- [ ] 모든 completion-bearing high-level terminal의 context-keyed 2-phase pre-registration이
      pre-return drain을 exactly once settle하고 synchronous failure에 completion이 없다.
- [ ] Non-OK error-reply payload는 C만 노출하고 나머지 binding은 typed error settle 전
      native aggregate를 한 번 정리하며 새 error-payload API가 없다.
- [ ] C++·Rust token owner tag가 close/recreate 뒤 stale token의 owner 불일치 거부를 지키고,
      Rust `StreamPacket::close(self)`는 다른 binding의 idempotent close test와 분리된다.
- [ ] Binding 여덟 개의 모든 지원 pattern·transport 1024-byte perf smoke가 통과한다.
- [ ] Core·binding guide가 실제 public sample과 일치하고 독립 2축 review를 통과한다.
- [ ] POSDDD·성능·dead-code refactor 뒤 각 layer의 회귀 test가 다시 통과한다.
- [ ] `VERSION`, `BINDINGS_VERSION`과 모든 관리 pin이 `0.16.0`이다.
- [ ] Core `core/v0.16.0` release asset, checksum과 provenance를 확인했다.
- [ ] Binding package 여덟 개가 local artifact 경로에 있고 clean consumer가 Core 0.16.0을 load한다.
- [ ] Framework가 local binding package 0.16.0만 소비한다.
- [ ] Framework C++·.NET·Java/Kotlin·Node unit와 언어별 E2E가 통과한다.
- [ ] C++·Node cross-language runner와 Java host rebuild가 통과한다.
- [ ] C++·.NET·Java/Kotlin·Node에서 공통 sample 일곱 개가 모두 정상 종료한다.
- [ ] 계획된 green commit과 push, Core release tag가 완료됐고 외부 binding registry는 publish하지 않았다.
- [ ] 최종 보고에 commit, release URL, package provenance, test·perf·E2E·sample report 경로와 남은
      실패만 기록했다.
