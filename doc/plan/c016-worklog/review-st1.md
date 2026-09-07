# review-ST-1 독립 리뷰

현재 diff는 **채택 불가**다. STREAM의 public receive와 mailbox command가 서로 다른 배타 장치를 사용한다는 원인 분석은 코드와 일치한다. 그러나 command가 `receive_owner_async`를 임시로 사용하고 해제하면, 기존 mutex 전용 수신 경로와 lease 전용 수신 경로가 다시 동시에 실행될 수 있다. 새 테스트의 Windows 빌드 호환성도 수정해야 한다.

- 대상: `/home/hep7hep7/project/zlink-work/st1`, detached HEAD `1696ed55e5b9c6928c6be62c15932c530c5521cf`의 미커밋 diff와 untracked `test_stream_concurrent_pull_send.cpp`.
- 코드·테스트의 `core/...:행`은 위 worktree 기준이다. 보고서·계획의 `doc/...:행`은 `/home/hep7hep7/project/zlink` 기준이다.
- B는 채택 차단, W는 비차단 주의·기존 미해결 항목, S는 확인 결과·제안이다. 실행 결과와 정적 분석을 구분했다.
- 소스·스펙·테스트 수정, 빌드·테스트·benchmark 실행, 커밋은 하지 않았다. 작성 파일은 이 보고서와 `progress-review-st1.md`뿐이다.

## B-1. 임시 async 상태 해제 뒤의 수신 배타 공백

**판정: 차단 / 정적 분석으로 가능한 실행 순서 확인.** `receive_owner_async`는 단순히 “누군가 lease를 보유한다”는 값이 아니다. 기존 public 경로에서는 “lease 취득을 포기하고 `receive.sync`만 잡아도 된다”는 의미다. 새 command scope가 이 값을 잠깐 사용하고 `available`로 돌리는 것은 그 전제를 깨뜨린다.

근거:

- `core/src/runtime/sockets/common/socket_runtime.hpp:389`: 새 command 취득이 `available → async` CAS를 수행한다.
- 같은 파일 `:326`: public 취득은 `async`를 보면 false로 반환한다.
- `core/src/runtime/sockets/common/socket_base_msg.cpp:100`: false를 받은 수신은 mutex만 획득하고, 상태어를 재확인하거나 lease를 취득하지 않은 채 `receive_()`를 호출한다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:1678`: count-1 Application lane의 completion drain도 같은 분기를 사용한다. `:1685` fallback은 mutex만 획득한다.
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:97`: command scope 소멸자는 mutex를 놓고 `release_receive_sync_from_async_owner()`로 상태어를 `available`로 돌린다. store 구현은 `socket_runtime.hpp:366`이다.

single-consumer 제약을 어기지 않는 실행 순서는 다음과 같다. R은 유일한 DATA receiver, P는 유일한 completion poller, C는 다른 thread의 command 처리다.

| 순서 | 주체 | 동작 |
|---|---|---|
| 1 | C | 실제 async executor가 없는 socket에서 command scope가 `async`를 설정하고 mutex를 보유한다. |
| 2 | P | count-1 completion drain의 public lease 취득이 `async`를 보고 false로 끝난다. mutex 획득 직전에 선점된다. |
| 3 | C | command를 끝내고 mutex와 임시 `async` 상태를 해제한다. |
| 4 | R | `available → public` 취득 후 ROUTER의 raw terminal DATA 수신에 들어간다. 이 경로는 mutex를 잡지 않는다. |
| 5 | P | mutex만 잡고 count-1 drain 및 fq 재분류에 들어간다. R의 lease를 보지 않으므로 두 실행이 겹친다. |

이 조합의 진입 경로도 확인했다. `socket_base_dispatch.cpp:222`의 completion poller 등록은 monitor가 없으면 async executor를 정지시킬 수 있다(`:240`, `:254`). `socket_base_api.cpp:924`의 poller 진입은 lifecycle admission만 취득하며, `get_events_internal()`의 command drain(`:838`)이 끝난 뒤 completion drain(`:887`)을 수행한다. 이 후반부 전체를 public API turn으로 감싸지 않는다. DATA 수신은 `socket_request_reply_runtime_io.cpp:1010` → `socket_base_msg.cpp:982` → `router_recv_path.cpp:397`로 진입한다. raw terminal은 `socket_request_reply_runtime_io.cpp:450`에서 whole-record 취득을 생략한다. completion drain은 `socket_base_api.cpp:1640` → `:1164`의 재분류에서 `xread_activated`/`xread_deactivated`를 호출하여 같은 fq를 변경한다.

따라서 “public recv 두 개를 금지하면 해결”되지 않는다. DATA receiver와 Core completion drain이 같은 물리 수신 상태의 소유권을 공유하는 문제다. `05-polling.ko.md:122`의 completion owner 하나, `:135`의 poller별 직렬화 조건을 지키는 구성이다.

같은 상태 의미 충돌은 **async executor 설치**에도 나타난다. `require_receive_sync_for_async_owner()`는 `async`를 보면 이미 설치용 배타 상태가 확보됐다고 판단한다(`socket_runtime.hpp:354`). `ensure_completion_processing()`은 admission과 completion/progress gate만 취득하고 설치를 요청한다(`socket_base_dispatch.cpp:139`, `:147`, `:195`). 설치는 receive 상태를 먼저 확보한 뒤 command mutex를 기다린다(`socket_base_lifecycle.cpp:969`, `:978`). 이때 임시 command의 `async`를 차용하면, command 소멸자의 store가 설치자가 확보했다고 믿는 상태를 지워 버린다. 이 경로에는 새 테스트의 STREAM echo만으로 확인할 수 없는 전환이 있다.

**수정 요구:** mutex fallback을 허용하는 기간과 lock-free 진입을 재개하는 경계를 하나의 소유권 프로토콜로 맞춰야 한다. 소멸자의 unlock/store 순서만 바꾸어도, 이미 false를 받고 지연된 fallback 진입자는 남으므로 해결되지 않는다. 공유 상태의 기존 소유자를 통합하는 방향이 우선이며, 새 timeout·재시도 횟수나 fq 전용 lock으로 보상해서는 안 된다. 이 경계와 count-1 completion drain을 고정된 순서로 교차시키는 회귀 검사가 필요하다.

## B-2. 새 통합 테스트의 Windows 컴파일 실패

**판정: 차단 / 소스와 CMake 등록으로 확인, 빌드 미실행.**

`core/tests/integration/test_stream_concurrent_pull_send.cpp:14`는 `arpa/inet.h`, `netinet/in.h`, `netinet/tcp.h`, `sys/socket.h`, `unistd.h`를 조건 없이 포함한다. `:105`의 `int` socket, `:112`의 `timeval` socket timeout, `:123`의 `close()`도 POSIX 전용 사용이다. 반면 `core/tests/CMakeLists.txt:161`은 이 파일을 공통 tests 목록에 넣고, `:343`에서 Windows에도 executable을 만든다. Windows 지원은 같은 파일 `:276`, `:379`에 명시돼 있다.

**수정 요구:** 기존 `test_stream_multiclient_delivery.cpp:6`, `:20`처럼 이미 의존하는 Boost.Asio 또는 저장소의 이식 가능한 socket 도구를 사용한다. Windows에서 테스트를 제외하는 것으로 회귀 검사의 범위를 줄일 이유는 없다. `MSG_NOSIGNAL` 자체는 `testutil.hpp:105`에 fallback이 있으므로 별도 결함으로 세지 않았다.

## S-1. 대기와 lock 순서

**판정: 조사한 정상 receive 경로에서 command 적용을 기다리는 새 순환 교착은 발견하지 못했다. §3.4의 대기 전 해제는 아래 경로에서 지켜진다. B-1의 배타 공백과 W-2의 공정성 한계는 별개다.**

| 경로 | 근거 파일:행 | 판정 |
|---|---|---|
| 일반 blocking recv | `socket_base_msg.cpp:90`, `:748` | 실패한 `receive_once_guarded()`가 lease/mutex를 해제한 뒤 `wait_receive_progress()` 또는 `process_commands(timeout)`로 대기한다. |
| routed/pipe blocking recv | `socket_base_msg.cpp:869`, `:1011` | 같은 구조다. 실패한 record admission도 `:69`, `:82`, `:114`에서 rollback과 해제를 수행한다. |
| mailbox wait | `socket_base_lifecycle.cpp:611`, `:619` | command 적용 guard와 public API/command mutex scope를 벗어난 뒤 signaler에서 기다린다. |
| receive progress CV | `socket_base_lifecycle.cpp:1641`, `:1648` | lease를 가진 상태로 호출하지 않는다. 이 호출의 mutex 획득 깊이는 1이며 CV 대기에서 놓는다. `sync` 타입 자체가 recursive인 것은 기존 구조다(`socket_runtime.hpp:410`). |
| whole-record receive | `socket_runtime.hpp:540`, `:570`; `socket_base_msg.cpp:1048` | 첫 frame 성공 후 lease+mutex를 보유하지만 continuation은 직접 `xrecv()` 한 번을 호출한다. 실패 시 mailbox 처리나 대기를 하지 않고 오류로 반환한다. |
| record tail·오류 drain | `socket_request_reply_runtime_io.cpp:498`, `:536`, `:567` | 이미 공개된 record의 tail을 직접 읽는다. 부족하면 실패로 끝내며 command를 기다리지 않는다. 큰 multipart의 처리 시간만큼 command 지연은 가능하다. |
| public multipart part 사이 | `socket_message_api.cpp:192`; `socket_request_reply_runtime_io.cpp:1248`; `socket_runtime.hpp:455` | 물리 record를 먼저 수집하고 반환할 때 record scope를 해제한다. caller가 다음 part를 요청할 때까지 receive lease를 유지하지 않는다. |
| STREAM packet header/body | `stream.cpp:731`, `:739` | guarded header 수신이 끝난 뒤 body를 이동한다. packet 반환 전체를 lease로 감싼 구조는 아니지만 body 이동 중 대기는 없다. |
| poller wait | `socket_base_api.cpp:924`, `:942`; `runtime/core/recv_internal.cpp:354` | readiness 함수가 반환된 뒤 poller가 대기한다. receive lease를 poller wait까지 넘기는 scope는 없다. readiness 자체의 race는 W-1 참조. |

새 command의 실제 순서는 `public_api_sync → command_owner_sync → receive.sync → lease 취득 시도`다(`socket_base_lifecycle.cpp:475`, `:477`, `:575`, `:82`). public whole-record 경로는 `lease → receive.sync`다(`socket_base_msg.cpp:61`, `:65`). command가 busy에서 **mutex를 놓고** 재시도하므로 이 두 자원만으로는 순환 대기가 성립하지 않는다. 코드 주석 `socket_base_lifecycle.cpp:65`의 “receive exclusion first and sync second”는 실제 구현과 반대이므로 수정해야 한다.

command가 public lease를 보유한 동일 thread로 재진입하면 owner 식별 없이 busy를 반복할 수 있지만, 조사한 일반 recv·record continuation은 command를 lease 밖에서 처리한다. completion drain의 중첩은 새 scope가 `async`를 보므로 mutex 재진입으로 진행한다. 이것을 전체 호출 그래프의 무교착 증명으로 확대하지 않았다.

## W-2. yield 재시도의 종료와 공정성

**판정: 명시적인 시간·횟수 상한과 공정성 보장 없음. 일반 STREAM receive가 무한히 command를 굶긴다고 단정할 근거도 없음.**

- `socket_base_lifecycle.cpp:81`의 루프에는 timeout·종료 플래그 검사·대기 순번이 없다. yield 중에도 바깥의 public API turn과 command mutex는 유지한다. `wait_timeout_budget_t`는 이 생성자 내부를 제한하지 않는다.
- 단일 public DATA receiver는 `socket_command_runtime.cpp:21`의 tick에 따라 command 처리에 들어간다. `runtime/utils/config.hpp:31`의 기준은 1536회이고, 그 전에 lease를 놓는다(`socket_base_msg.cpp:713`). STREAM pump도 한 시도에서 raw chunk 64개로 제한한다(`stream.cpp:600`). 따라서 정상 진행과 thread scheduling을 가정하면 command가 들어갈 기회가 생긴다. 이것은 벽시계 상한이나 mutex 공정성 보장은 아니다.
- `stop()`은 command보다 먼저 `_ctx_terminated`를 발행한다(`socket_base.cpp:346`). 다음 receive 시도는 종료를 본다(`socket_base_msg.cpp:704`). 그러나 이미 lease를 가진 실행이 반환하지 못하면 새 command loop는 그 종료를 직접 검사하지 않는다. close는 진행 중 API를 취소하지 않고 `EBUSY`로 거부한다(`socket_lifecycle_runtime.cpp:295`). reaper도 이 loop의 별도 탈출을 제공하지 않는다(`socket_base_lifecycle.cpp:1535`).

보고서의 “재시도는 짧다”는 단정은 “정상 receive 시도가 끝나고 scheduler가 대기자를 실행시키면 진행한다”는 조건을 붙여야 한다. 추가 timeout으로 가리는 대신, 수정된 소유권 경계에서 waiter 진행과 종료를 고정된 순서로 검증하는 것이 맞다.

## S-2. command 범위와 해제 보장

**판정: `activate_read`에만 적용 범위를 제한할 이유는 없다. 다만 현재 RAII의 해제는 B-1의 소유권 의미까지 보장하지 않는다.**

- `activate_read`: `object.cpp:67` → `pipe.cpp:2824` → `socket_base_api.cpp:1766` → `stream.cpp:749`. public receive와 같은 fq partition을 변경하므로 배타가 필요하다.
- `bind`·pipe attach·termination: `object.cpp:130`, `:140`, `:144`; `socket_base_api.cpp:478`; `dealer.cpp:460`. fq membership과 pipe 수명이 바뀌므로 같은 배타가 필요하다.
- `hiccup`, write activation, stop·term: `object.cpp:71`, `:111`, `:135`, `:157`. 모든 command를 하나의 turn에서 처리한다는 spec §3.1(`11-synchronization-model.ko.md:80`)과 맞는다. 수신 field를 직접 바꾸지 않는 command만 별도 예외로 두는 것은 규칙을 다시 늘린다.
- reaper는 accepted close 이후 같은 `process_commands()`를 사용한다(`socket_base_lifecycle.cpp:1542`). 유효한 public API와 경쟁하지 않는 정상 종료에서는 추가 획득이 불필요한 비용일 수 있지만, correctness를 깨는 과잉 배타라고 보지는 않는다.
- RAII 생성 성공 뒤에는 `cmd.destination->process_command()`만 scope 안에 있고, 정상 종료·early return·C++ stack unwinding에서 소멸자가 호출된다(`socket_base_lifecycle.cpp:574`). `already_held`는 의도대로 타인의 lease를 해제하지 않는다. 단, 무엇을 “already held”로 볼 수 있는지가 B-1의 결함이다. abort나 생성자 루프의 비종료는 RAII가 해결하지 않는다.

## W-1. 다른 socket과 남아 있는 fq 접근 경계

**판정: mailbox에서 꺼내 적용하는 command와 public receive의 해당 쌍에는 공통 수정이 적용된다. fq 전체의 동시 접근이 닫혔다는 결론은 불가하다. 아래는 기존 경계이며 이번 리뷰에서는 보고만 한다.**

| socket | 직접 확인한 구현 | 수정의 적용 범위 |
|---|---|---|
| STREAM | `stream.cpp:623`, `:749` | 수신 pump와 mailbox read activation의 충돌을 배제한다. |
| DEALER | `dealer.cpp:469`, `:445`, `:460` | fq recv/activation/termination의 공통 command 경계에 적용된다. |
| ROUTER | `router_recv_path.cpp:397`, `:147`, `:206` | 같은 fq 경계에 적용된다. raw terminal의 지연 record admission 때문에 B-1도 해당한다. |
| SUB/XSUB | `pubsub/xsub.cpp:297`, `:135`, `:147` | 상속한 fq receive와 command activation/termination에 적용된다. |
| PAIR | `pair.cpp:30`, `:42`, `:57`, `:151` | **fq를 사용하지 않는다.** 단일 `_pipe`와 receive state의 attach/termination 배타에 공통 수정이 적용된다. |

남는 경계:

1. `socket_base_api.cpp:1031`의 `has_in()`은 여전히 mutex만 잡는다. STREAM `xhas_in()`은 실제 packet pump를 실행하고(`stream.cpp:1045`), DEALER `fq_t::has_in()`도 빈 pipe를 active partition에서 제거한다(`fq.cpp:401`, `:420`). DATA receiver 하나와 별도 poller 하나만 있어도 public lease 경로와 겹칠 수 있다. 새 테스트는 pull thread 하나가 poll과 recv를 순서대로 호출하므로 이 경계가 교차하지 않는다(`test_stream_concurrent_pull_send.cpp:153`, `:164`).
2. command loop 밖의 control attach도 있다. non-STREAM의 기본 connect는 `socket_base_endpoint.cpp:650`, `:698`에서 직접 `attach_pipe()`를 호출한다. 실제 `xattach_pipe()` 보호는 `socket_base_api.cpp:478`의 mutex뿐이다. public lease를 가진 수신과 동시 control connect가 실행되면 같은 fq partition 문제를 남긴다. control 동시 호출은 `socket/README.ko.md:54`에서 허용한다. 새 command scope만으로 이 경계까지 덮었다고 볼 수 없다.

이 기존 항목들을 ST-1의 새 결함으로 세지 않았다. 다만 계획 D-e를 “fq 접근의 소유권 정리 완료”라는 뜻으로 닫으려면 처리 범위를 별도로 확정해야 한다.

## S-3. lb 송신 partition 대칭성

**판정: 요청한 public send fast path ↔ mailbox command의 `lb::activated()` 쌍에는 같은 배타 공백을 발견하지 못했다. 기존 readiness 우회 경로는 별도 주의 대상이다.**

DEALER의 `xwrite_activated()`는 `_lb.activated()`를 호출한다(`dealer.cpp:455`), 실제 partition은 `lb.cpp:105`에서 바뀐다. public 송신은 `socket_base_msg.cpp:161`, `:171`, `:200`, `:302`의 `socket_public_send_scope_t(..., true)`로 public API turn을 취득한다. scope 구현은 `socket_lifecycle_runtime.cpp:430`, `:451`이며, command도 같은 turn을 취득한다(`socket_base_lifecycle.cpp:475`). 그러므로 해당 send/command 쌍에는 이미 공통 배타가 있다. receive와 달리 “send 성공 경로가 public API turn을 생략한다”는 전제는 성립하지 않는다.

별도로 `socket_base_api.cpp:950`의 transport readiness는 admission만 가진 poller에서 `transport_has_out()` → DEALER `_lb.has_out()`를 호출한다(`:1048`, `dealer.cpp:434`). `lb.cpp:752`, `:762`는 조회 중 partition을 변경한다. 이 경로와 send/command 사이에는 소스상 같은 turn이 없으므로 대칭적인 **기존 readiness race 후보**로 남긴다. 내부 transport poller 경로이며 이번 변경의 새로운 send race로 분류하지 않는다. 빌드나 재현은 하지 않았다.

## W-3. 성능 주장과 비용 범위

**판정: 경합 없는 기존 recv/send 성공 branch에 명령을 직접 추가하지 않았다는 주장은 맞다. 성공한 public recv 전체의 lock 추가가 항상 0이라는 주장은 틀리다.**

| 조건 | 코드로 확인한 추가 비용 |
|---|---|
| command 없음, public fast recv/send | 직접 추가된 atomic·mutex 연산 없음. 기존 source 경로는 diff에서 바뀌지 않았다. |
| async executor 없음, command 한 건 적용 | CAS 1회 + release store 1회. “CAS 2회”가 아니다(`socket_runtime.hpp:392`, `:368`). 기존 mutex lock/unlock은 유지한다. |
| 장기 async executor 있음 | 실패하는 CAS 1회로 `already_held` 판정, scope 소멸 시 store 없음. |
| public receive와 command 경합 | command의 mutex 재획득·실패 CAS·unlock·yield가 반복된다. 반대로 public recv가 임시 `async`를 보면 기존 mutex fallback으로 진입하므로 성공 수신에도 mutex 비용이 새로 발생할 수 있다(`socket_runtime.hpp:329`, `socket_base_msg.cpp:100`). |

`stream_tcp`의 command당 비용은 message당 고정 비용이 아니다. queue sleep 전이, credit, attach·term 빈도에 따라 상각 비율이 바뀐다. 작은 payload와 여러 thread가 같은 socket을 사용하는 경우에는 소유권 상태어의 cache line 왕복과 fallback 비율이 중요하고, queue에 충분히 쌓인 batch를 연속 처리하는 경우에는 영향이 작아질 수 있다. reqrep은 장기 async executor가 있으면 command당 실패 CAS 하나가 주된 증가이고, public completion poller가 executor를 넘겨받은 경우에는 임시 획득·해제와 B-1 경계를 모두 겪는다. 정량 하락률은 코드만으로 산정할 수 없다.

원 보고서 `core-rf-ST-1-report.md:194`는 성능 미측정을 명시한다. spec §8의 셀별 명령 수·lock 횟수 요구(`11-synchronization-model.ko.md:261`)는 아직 충족했다고 볼 수 없다. 이번 리뷰에서는 callgrind나 benchmark를 실행하지 않았다.

## S-4. 스펙과 D-e 판정

**판정: 수정 목적은 B(기존 결함)이고 공개 계약 문장을 바꿀 필요는 없다. 현재 구현으로 D-e를 완료 처리하는 것은 보류한다.**

- 소유 계층: Core socket semantic 상태와 socket 쪽 pipe 끝. 근거는 synchronization model §3.1(`:71`, `:78`), §3.2(`:101`), §3.3(`:149`).
- command/public 수신을 같은 배타 영역으로 만드는 목적은 §3.1과 §5(`:211`, `:212`)에 이미 있다. busy 시 mutex를 놓는 설계는 §3.4(`:170`)를 보존하려는 조치다.
- §4는 새 per-message lock의 소유 근거를 요구한다(`:191`). 기존 `receive_owner`를 재사용했다는 사실만으로 §3.1의 turn bit와 동일한 구현이 된 것은 아니다. §3.1(`:89`)의 public admission word turn과 별도의 receive word/mutex가 여전히 존재한다.
- 계획 `core-refactor-stream-perf-0.17.0-plan.ko.md:255`의 D-e 자체도 “계약 문장 변경이 아니라 소유 규칙 결정 + 성능 예산”이라고 쓴다. ST-1은 원인에 접근했지만 B-1, W-1, W-3 때문에 그 결정을 완료했다고 판정할 수 없다.

스펙을 수정하지 않았다. 필요하다면 구현 세부 이름을 넣지 않고 다음 **명료화 초안**을 검토할 수 있다. 기존 §3.1·§3.4의 의미를 보충하는 문장이며 이 patch를 허용하기 위한 계약 완화가 아니다.

> 공개 수신, readiness 검사, 같은 물리 수신 queue를 사용하는 completion 처리와 command 적용은 수신 상태의 같은 소유권 경계를 따른다. 소유권 전환 중 이전 방식으로 진입을 결정한 실행 주체도 그 경계에 포함하며, 이 실행이 끝나기 전에 다른 배타 방식의 진입을 허용하지 않는다.

설계 대조: public recv 모두에 mutex를 추가하는 안은 배타를 단순화하지만 성공 경로 비용을 바꾼다. 기존 소유권을 통합하고 command가 같은 소유권에 참여하는 안은 비용 목표에 맞지만, 이번처럼 지속적인 async mode를 임시 command mode로 해석해서는 안 된다. 선택 기준은 atomic 상태어 수가 아니라 전환을 포함해 설명할 배타 규칙이 하나인지다.

수정 전/후 규칙 수: 수신 실행의 배타 방식 **2개(public lease / async mutex) → 2개 유지**, `async` 값의 의미는 **1개(장기 executor) → 2개(장기 executor 또는 임시 command)**. 따라서 “새 상태·규칙 0개” 중 상태 필드 0개는 맞지만 의미상 규칙 0개는 성립하지 않는다.

교차언어 대조: Framework runtime 변경이 아니므로 언어별 Framework 변경은 대상이 아니다. 결함 보고 `2026-09-08-core-stream-packet-pump-stall.ko.md:43`, `:49`의 native C 대체 재현과 C/C++의 thread 구성 차이는 Core 경계라는 분석에 부합한다. 해당 성능 실행을 독립 재실행하지 않았다.

## W-4. 테스트 관측력과 기존 실행 증거

**판정: 실용적인 stress 회귀 테스트다. 결정적으로 race 순서를 고정하는 테스트는 아니다.**

`test_stream_concurrent_pull_send.cpp:262`는 pull·send·client thread를 시작하지만, `_fq.recvpipe()`의 partition 변경과 command activation이 반드시 겹치도록 rendezvous하지 않는다. `:272`의 yield는 완료 대기이며 내부 경합의 발생 조건을 고정하지 않는다. bounded case는 100×40, unbounded case는 40×40으로 workload도 다르다(`:311`, `:318`). 따라서 둘의 차이만으로 HWM이 race의 필수 조건이라고 증명할 수 없다. `:307`의 “Backpressure ... is what ...” 주석은 실측 재현 조합과 원인 조건을 구분해야 한다.

마지막 frame까지 client가 byte 단위 echo를 대조하는 검사(`:128`, `:131`, `:289`)는 유실·오배송 검출에 유효하다. 다만 오류 종류별 표면 검사는 부족하다. pull의 poll/recv 오류는 대부분 break로만 처리하며(`:154`, `:166`), 성공 여부는 echo 수로 판단한다. 새로운 임시 async fallback, async 설치, record 소유권 전환은 검사하지 않는다.

30초는 모든 실패 경로의 종료 상한이 아니다. send thread는 timeout을 설정하지 않은 server에 blocking `zlink_send_part_rid()`를 호출하고(`:219`, `:234`), main은 stop flag를 설정한 뒤 그 thread를 join한다(`:281`, `:286`). 소유권 교착이나 종료 불능은 내부 stop flag로 중단되지 않는다. 최종 외부 상한은 CTest 180초(`CMakeLists.txt:618`)이며, 바이너리를 직접 실행하면 이 상한도 없다. timeout 증가로 고칠 항목은 아니다.

기존 실행 증거는 다음 범위만 확인했다.

| 자료 | 독립 리뷰에서 확인한 내용 |
|---|---|
| 원 보고서 `:216`, `:217` | 수정 전 4/4 FAIL, 수정 후 4/4 PASS라는 작성자의 기록. 원시 4회 로그는 이번에 확인한 scratch 파일 집합에 없어서 횟수를 독립 검증하지 않았다. |
| 원 보고서 `:218`, `:222` | 20회 반복 통과와 suppression 없는 TSan 경고 0이라는 기록. 새로운 실행은 하지 않았다. 이 결과가 모든 소유권 전환을 덮었다는 뜻은 아니다. |
| scratch `st1/suite.log` | 실제 로그 끝에 57개, 실패 0, 315.58초를 확인했다. |
| scratch `st1/tsan_pp.log:685`, `:690` | `test_shutdown_during_drain` 실패와 TSan 8건을 확인했다. 수정 전에도 같았다는 대조 판단은 원 보고서 `:237`의 기록이며 이번에 양쪽 로그를 대조하지 않았다. |
| 원 보고서 `:243` | ASan 미완료. 성능 측정도 미실행 상태다. |

scratch 기준 경로는 `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/st1/`이다.

필요한 보강은 STREAM 부하량 증가가 아니다. 기존 `unittest_receive_transaction.cpp:300`의 record contention hook, `fq.cpp:305`의 수신 hook 같은 장치로 **command 임시 상태 → fallback 진입 결정 → command 해제 → 다른 수신 소유자 진입** 순서를 고정해야 한다. ROUTER count-1 DATA/completion 조합은 B-1 때문에 필요하다. DEALER는 공통 소유권 전환과 multipart continuation을, SUB/XSUB는 수신 중 control attach·activation을, PAIR는 fq 변형 대신 단일 pipe attach·termination을 확인하는 것이 적절하다. 기존 `unittest_receive_transaction`은 보고서의 `stream|pipe|wake|hwm|flow|credit|poll` 정규식에 이름이 맞지 않으므로 그 suite 통과에 포함됐다고 간주하지 않았다.

차단 항목 수 / 채택 가능 여부: 2 / 현재 diff 채택 불가
