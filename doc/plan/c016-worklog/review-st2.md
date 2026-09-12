# review-ST-2 receive 소유권 프로토콜 차단 검증

현재 diff는 **채택 불가**다. 1차 B-1의 임시 command 상태와 지연된 mutex fallback 사이의 공백은 닫혔다. 그러나 실제 async 모드를 해제할 때 이미 진입한 mutex 보유자를 배제하지 않으므로 전체 receive 소유권 불변식은 성립하지 않는다. `progress_epoch`의 data race와 신규 Asio 테스트의 수명·동시 close 문제도 남는다.

- 대상: `/home/hep7hep7/project/zlink-work/st1`, HEAD `1696ed55e5b9c6928c6be62c15932c530c5521cf`의 미커밋 5개 파일과 untracked `core/tests/integration/test_stream_concurrent_pull_send.cpp`.
- 기준: `_common-rules.md`, `review-st1.md`, `core-rf-ST-1-report.md`, `core-rf-ST-2-report.md`, synchronization model §3.1/§3.3/§3.4/§4/§5 및 Polling owner 절. B는 채택 차단, W는 비차단 주의, S는 확인·제안이다.
- 소스·스펙·테스트 수정, 빌드·바이너리·테스트·벤치마크 실행, 커밋은 하지 않았다. 기존 로그와 소스를 읽었으며, 이 보고서와 `progress-review-st2.md`만 작성했다.
- 이하 `core/...`와 소스 basename의 행 번호는 대상 worktree 기준이다. `doc/...`는 메인 저장소 기준이다. `<scratch>`는 `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad`다.

## 항목별 판정

| 검증 항목 | 판정 | 근거·잔여 사항 |
|---|---|---|
| B-1: 1차 리뷰의 5단계 command/fallback 순서 | **해소** | `socket_runtime.hpp:405`, `socket_base_lifecycle.cpp:111`. mutex 획득 후 public 상태를 보면 다시 lease를 취득한다. |
| B-1: async 설치자가 임시 command를 async로 오인 | **해소** | `socket_runtime.hpp:423`, `:461`. command와 async 값이 분리돼 설치자는 command가 끝나기 전 성공하지 않는다. |
| B-1: 모든 모드 전환을 포함한 receive 배타 | **부분** | 실제 async 해제가 `sync`를 잡지 않는다. 이미 재검증을 끝낸 mutex entrant와 새 lease가 겹친다. **B-ST2-1**. |
| W-1: `has_in()`과 public lease | **부분** | `socket_base_api.cpp:1039`에서 공통 진입 사용. 안정된 모드에서는 해소되지만 async 해제 공백은 공유한다. control attach(`:478`)는 여전히 미해소. |
| W-2: public_waiting 등록과 lease 해제 알림 | **해소** | `socket_runtime.hpp:484`, `:498`, `:532`. 표시·CV 진입·broadcast가 같은 `sync`에서 연결된다. exchange 자체는 mutex 밖이다. |
| W-2: 종료·공정성의 무조건 보장 | **부분** | 정상 lease 시도 종료를 가정하면 진행한다. CV에 FIFO·기아 방지 보장은 없고 종료는 lease 보유자를 강제 취소하지 않는다. 두 command 후보의 동시 CV 대기는 바깥 mutex 때문에 발생하지 않는다. |
| S-1: RCVTIMEO 대기 전 lease 해제 | **해소** | `socket_base_msg.cpp:98`, `:756`, `:877`, `:1019`. continuation은 `:1065`의 단일 xrecv이며 blocking wait를 하지 않는다. |
| W-3: hot path 비용 | **부분** | public take CAS 1 + release exchange 1은 맞다. store→RMW와 has_in의 CAS+RMW 증가는 비용 0이 아니다. 측정 증거는 없다. |
| 잔여 TSan epoch 1건 | **미해소** | `socket_base_msg.cpp:71` ↔ `socket_base_lifecycle.cpp:1648`; 신규 `socket_runtime.hpp:511`도 같은 비원자 값을 쓴다. **B-ST2-2**. |
| B-2: Windows 전용 include/API 컴파일 문제 | **해소(정적)** | 신규 테스트 `:14`, `:97`, `:119`는 Boost.Asio 사용. 공통 CMake 등록도 유지한다. Windows 빌드는 실행하지 않았다. |
| B-2: 이식 후 테스트 자체의 유효성 | **미해소** | socket/io_context 수명 역전과 close/read 동시 접근. **B-ST2-3, B-ST2-4**. |
| W-4: 결정적 race 관측·수정 전 3/3 FAIL | **부분** | byte echo 검사는 유지되지만 내부 실행 순서 고정은 없다. baseline 로그가 빈 파일이라 3/3 종료 코드는 보고서 기록에 의존한다. |
| S-2: RAII·command 적용 범위 | **부분** | 새 command/entry scope의 정상 해제는 맞다. public recv는 여전히 수동 해제이며 전체 예외 안전성 증명은 아니다. |
| S-3: 송신 lb 대칭성 | **해소(기존 결론 유지)** | send/command의 public API turn 공유는 유지된다. 기존 transport readiness 우회 후보는 이 diff가 해결하지 않는다. |
| S-4: spec 명료화·완료 판정 | **부분** | 수신 상태 소유권 연속성의 명료화는 타당하다. 재검증 한 번만을 요구하는 문장으로는 async 해제 문제를 막지 못한다. 전체 §3.1 준수·D-e 완료 주장은 보류한다. |

## S-ST2-1. 코드에서 재구성한 상태 전이

주요 파일: [socket_runtime.hpp](../../../core/src/runtime/sockets/common/socket_runtime.hpp), [socket_base_lifecycle.cpp](../../../core/src/runtime/sockets/common/socket_base_lifecycle.cpp).

`public_waiting`은 public lease의 소유자를 바꾸지 않고 command 대기 표시만 더한다. `async`는 executor 설치가 완료된 상태만 뜻하지 않는다. 설치 전에 먼저 발행하며 설치 실패·detach까지 유지하는 mutex 모드다(`socket_base_lifecycle.cpp:985`, `:995`, `:1005`).

| 출발 → 도착 | 실행 주체·연산 | mutex 조건·근거 |
|---|---|---|
| available → public | public recv, has_in, count-1 drain의 weak CAS, 성공 acquire | `sync` 없이 `socket_runtime.hpp:355`. |
| public → public_waiting | command 후보의 strong CAS, acq_rel | `sync` 보유, `:484`. public 소유권은 그대로다. |
| public → available | public 해제 exchange, release | `:387`. whole-record/has_in은 자기 `sync` scope를 먼저 끝낸다. |
| public_waiting → available | 같은 exchange 뒤 waiter broadcast | exchange는 mutex 밖, broadcast는 `sync` 아래 `:532`. |
| available → command | command turn의 strong CAS, acquire | `sync` 보유, `:461`; 획득 성공 뒤 command 적용 동안 유지한다. |
| command → available | command 소멸자의 release store | **store 후 unlock**, `socket_base_lifecycle.cpp:113`, `:116`. |
| available → async | 설치자의 weak CAS, acquire | `sync` 없이 `socket_runtime.hpp:423`; 이후 command_owner_sync를 잡아 설치한다. |
| async → async | 설치자는 성공으로 간주; command는 already_held; public entrant는 mutex+재검증 | `:429`, `:468`, `:413`. command/entrant는 async 값을 해제하지 않는다. |
| async → available | 설치 실패 또는 executor detach의 release store | `:435`; **receive.sync 보유를 요구하거나 확보하지 않는다**. B-ST2-1. |

그 밖의 값에서 public 취득은 CAS 재시도하며, async/command를 관측하면 mutex 분기로 간다. 설치자는 public/public_waiting/command에서 계속 CAS 재시도한다. command 취득 실패 뒤 public 표시가 아니면 mutex를 놓고 재시도한다. 따라서 “모든 소유권 대기에 spin이 없다”는 설명은 틀리고, 없어진 것은 command의 public lease 대기 yield 루프다.

| receive 진입자 | 안정된 모드에서의 배타 | 경계 |
|---|---|---|
| public recv (`socket_base_msg.cpp:63`) | lease 단독 또는 mutex+재검증 | record 성공 시 소유권을 record scope로 넘긴다. |
| count-1 Application completion drain (`socket_base_api.cpp:1695`) | 같은 entry RAII | command 중첩에서는 recursive sync 재진입 후 command를 관측해 통과한다. |
| has_in (`socket_base_api.cpp:1039`) | 같은 entry RAII; lease이면 `sync`도 획득 | `:1020`의 buffered-part 조기 반환은 helper mutex만 쓰며 fq를 만지지 않는다. |
| async 설치 (`socket_base_lifecycle.cpp:985`) | 기존 lease/command 종료 후 available→async | 설치 자체는 fq를 만지는 실행자가 아니다. 이후 command 적용은 sync 아래다. |
| command (`socket_base_lifecycle.cpp:590`) | sync와 command claim 또는 async 모드의 sync | detach가 async 값을 지우는 경계까지는 보호하지 못한다. |

### 1차 B-1의 5단계 순서

1. C가 `sync`를 잡고 available→command를 수행한다.
2. P가 command를 보고 mutex fallback을 택한 뒤 획득 직전에 지연된다.
3. C가 command→available을 **sync를 놓기 전에** 수행한다.
4. R이 available→public lease를 취득하고 fq를 처리한다.
5. P가 sync를 잡아도 word는 public/public_waiting이다. `enter_receive_exclusion():411`에서 이를 확인하고 sync를 놓으며, R이 끝나기 전 fq에 진입하지 않는다.

이 반례는 차단된다. 같은 이유로 **sync를 보유하고 command를 읽으면 그 command는 같은 thread의 것**이라는 추론은 count-1 drain에 한정하지 않아도 맞다. 다른 thread는 C의 최종 unlock 전 sync를 획득할 수 없고, C의 clear는 mutex 획득 뒤 관측보다 앞선다. 새 command가 그 사이 설치되더라도 그 설치자 역시 sync를 보유한다. 다만 이것은 command 상태에 대한 증명이며, mutex 밖에서 해제되는 async 상태에는 적용되지 않는다.

## B-ST2-1. async 해제가 진행 중인 mutex 수신을 배제하지 않음

**차단 / 정적 반례. 기존 해제 경로의 결함이며, 이번 프로토콜 완료 주장에 직접 포함되는 전환이다.**

[release_receive_sync_from_async_owner()](../../../core/src/runtime/sockets/common/socket_runtime.hpp)는 release store만 한다. 호출자도 receive.sync를 잡지 않는다. 설치 실패 `socket_base_lifecycle.cpp:1005`, idle detach `:1333`, 종료 detach `:1600`, 일반 quiesce detach `:1629`를 확인했다. idle 경로의 `_transport_pair_owner_progress_sync`(`:1269`)는 has_in/public recv가 취득하는 mutex가 아니다.

| 순서 | 실행 |
|---|---|
| 1 | async 모드에서 P(별도 POLLIN poller)가 `has_in()`에 진입한다. sync를 잡고 word==async를 재검증한다(`socket_runtime.hpp:410`). |
| 2 | P가 sync를 계속 쥔 채 xhas_in/fq 처리 직전에 지연된다. |
| 3 | A(async executor)가 idle 또는 quiesce detach에서 word를 available로 저장한다(`socket_base_lifecycle.cpp:1333` 또는 `:1629`). receive.sync를 기다리지 않는다. |
| 4 | R(유일한 DATA receiver)이 available→public CAS에 성공해 mutex 없이 fq에 진입한다. |
| 5 | P가 계속 실행한다. STREAM이면 `stream.cpp:1049` → pump `:623`이 같은 fq를 만진다. R과 P가 겹친다. |

A가 나중의 `notify_receive_progress()`에서 sync를 기다리는 것은 늦다. public 진입은 이미 허용됐다. 이 반례에는 public DATA receiver 두 명이나 completion owner 두 명이 필요하지 않다. Polling `05-polling.ko.md:122`, `:135`의 owner 제약도 위반하지 않는다. readiness 모드의 기존 entrant도 소유권 전환이 보호해야 할 실행이다.

수정 요구는 async→available 전환이 **이미 mutex 방식으로 진입한 실행의 종료까지** 보장하도록 같은 소유권 프로토콜에 포함하는 것이다. 진입 시 재검증만 추가하거나 store의 memory order를 강화해서는 해결되지 않는다. command 전이의 수정은 유지할 수 있지만, async 설치 실패·idle/명시적 detach를 포함한 해제 경계 검증 없이는 B-1 전체를 닫을 수 없다.

## S-ST2-2. W-2 대기·종료·공정성

command가 sync 안에서 public→public_waiting을 표시한 뒤 `waiters`를 늘리고 CV wait에 들어간다(`socket_runtime.hpp:484`, `:500`). public release의 exchange는 **같은 mutex 아래가 아니다**. 대신 이전 값이 public_waiting이면 sync를 획득한 뒤 broadcast한다(`:532`). 따라서 표시가 성공한 뒤 release가 먼저 일어나더라도 broadcast는 waiter가 CV에 등록하며 sync를 놓은 뒤에야 가능하다. release가 표시보다 먼저면 CAS가 실패해 command가 잠들지 않고 다시 claim한다. 정상 단일 깊이의 wait에서는 이 handoff에 lost wake가 없다.

CV의 다른 progress broadcast나 spurious wake로 일찍 돌아와도 command 생성자의 바깥 루프가 claim/표시를 다시 검사한다. lease 해제를 무조건 소유권 양도로 보지는 않는다. 해제 직후 다른 public receiver/probe가 available을 먼저 잡을 수 있고 FIFO 순번도 없으므로, **wake 유실 방지와 공정성 보장은 다르다**.

서로 다른 두 `process_commands()` thread가 동시에 같은 public_waiting을 기다리는 상황은 현 호출 경로에서 발생하지 않는다. `socket_base_lifecycle.cpp:490`의 public API turn과 `:492`의 command_owner_sync를 잡은 후보 한 명만 `:590`까지 도달한다. 첫 후보가 CV에 있는 동안에도 이 바깥 자원들은 유지한다. 두 번째 후보는 바깥 gate에서 기다린다. 따라서 CV에서 두 command 후보의 wake 배분 공정성을 논할 필요는 없지만, 그 바깥 gate에도 FIFO 보장은 없다.

blocking public recv는 `receive_once_guarded():98`에서 lease를 해제하고 반환한 뒤 `socket_base_msg.cpp:756`의 progress/mailbox 대기를 시작한다. recv_pipe와 routed도 각각 `:877`, `:1019`에서 같다. whole-record scope는 성공한 첫 frame 뒤에만 유지되며 continuation `:1065`는 한 번의 xrecv 후 성공/오류로 반환한다. **RCVTIMEO 동안 lease를 쥐어 command가 같은 시간만큼 밀리는 경로는 확인되지 않았다.**

ctx stop은 `_ctx_terminated`를 먼저 발행하고 mailbox를 깨운다(`socket_base.cpp:354`). 정상 진행 중 receive 시도가 끝나면 release가 command waiter를 깨우고 이후 수신은 ETERM을 관측한다. close는 진행 중 API를 취소하는 경로가 아니라 EBUSY gate다(`socket_lifecycle_runtime.cpp:295`). command 생성자의 종료 검사(`socket_base_lifecycle.cpp:105`)는 lease를 강제로 회수하거나 wait에서 탈출시키지 않는다. 따라서 scheduler 진행과 유한한 receive 시도라는 전제까지 제거한 종료 보장은 없다.

## B-ST2-2. progress_epoch data race를 무해한 재확인으로 분류할 수 없음

**차단 / 기존 원시 TSan 로그와 코드에서 확인. 이번 patch에서 동기화 수정에 포함해야 한다.**

`progress_epoch`는 `socket_runtime.hpp:542`의 일반 uint64_t다. public lease 분기는 `socket_base_msg.cpp:71`에서 mutex 없이 읽는다. notify는 `socket_base_lifecycle.cpp:1648`에서 쓴다. `<scratch>/st2/tsan2_test_stream_packet_progress.log:351`의 read stack과 `:361`의 previous write stack은 각각 receive_once_guarded와 process_async_mailbox→notify_receive_progress다. `:415`에 이 쌍의 TSan summary가 남아 있다.

mutex 안에서 나중에 다시 비교한다는 것(`socket_base_lifecycle.cpp:1658`)은 **이미 발생한 비원자 read/write 경쟁의 happens-before를 만들지 않는다**. C++ data race가 있는 프로그램에 대해 “최악은 대기 한 번 추가/생략이고 lost wake는 불가능”이라는 상한을 증명할 수 없다. torn/stale snapshot과 컴파일러 최적화까지 허용하는 현 구현을 정상적인 atomic snapshot처럼 분석해서는 안 된다. 이 리뷰에서는 특정 release binary에서 영구 대기가 실제 발생했다고 주장하지 않는다.

더구나 ST-2가 새로 추가한 `broadcast_receive_progress_locked()`도 `socket_runtime.hpp:511`에서 같은 값을 쓴다. command는 public lease가 실행 중인 상태를 표시한 뒤 종료 시 이 함수를 부른다(`socket_base_lifecycle.cpp:101`). 이것은 reader를 배제하지 않은 새 writer 경로다. “읽는 줄이 기존 코드”라는 이유만으로 이번 변경의 검증 대상에서 제외할 수 없다. synchronization model §3.2 `:114`, §5 `:213`, §8 `:260`에 어긋난다.

최소한 epoch의 mutex 밖 snapshot을 atomic acquire로, 발행을 release로 통일하거나 모든 접근이 실제로 공유하는 소유권 안으로 옮겨야 한다. 다만 **atomic 타입 변경만으로 receive progress 전체를 검증 완료했다고 해서는 안 된다**. public packet recv도 `stream.cpp:731` → `socket_base_msg.cpp:725` → `stream.cpp:990`, `:692` → pump `:604`에서 `notify_receive_progress_locked()`를 호출할 수 있다. 이때 일반 lease 분기는 sync를 잡지 않는다. 이 기존 경로에서는 epoch뿐 아니라 `waiters` 읽기와 CV 발행 전제도 다시 확인해야 한다. has_in에만 sync를 추가한 수정은 public pump의 이 경로를 닫지 않는다.

## B-ST2-3. Asio socket이 소유 io_context보다 오래 존재함

**차단 / 새 테스트에 생긴 수명 결함. Windows backend까지 소스로 확인했다.**

신규 테스트 `core/tests/integration/test_stream_concurrent_pull_send.cpp:116`의 io_context는 client thread 지역 변수다. `:117`의 socket은 이 context를 참조하지만 `:126`에서 harness의 shared_ptr 목록(`:66`, `:71`)에도 저장된다. client가 `:139`에서 socket을 close하고 반환해도 목록의 shared_ptr은 남는다. **close는 socket C++ 객체의 파괴가 아니다.** 따라서 context/service가 먼저 파괴되고 socket은 join 이후 harness 파괴 때까지 남는다(`:269`, `:296`, `:317`). 성공 경로에서도 발생한다.

실제 의존성은 bundled Boost이며 `core/build-dev/tests/CMakeFiles/test_stream_concurrent_pull_send.dir/flags.make:7`에 그 include 경로가 있다. 다음 bundled 코드로 수명 문제를 확인했다.

- `core/external/boost/boost/asio/detail/io_object_impl.hpp:58`: context의 service 포인터 저장.
- 같은 파일 `:93`: socket 구현 파괴 시 저장한 service로 `destroy()` 호출.
- `core/external/boost/boost/asio/impl/execution_context.ipp:32`, `:44`: context 파괴가 service들을 파괴한다.
- `core/external/boost/boost/asio/detail/impl/win_iocp_socket_service_base.ipp:148`, `:154`: 닫힌 socket이라도 destroy는 service의 mutex와 implementation 목록을 사용한다. 이미 파괴된 service에 접근한다.

Linux의 reactive backend는 닫힌 descriptor에서 destroy 본체를 건너뛸 수 있다(`detail/impl/reactive_socket_service_base.ipp:83`). 그러므로 Linux ASan 2/2 PASS가 이 수명 결함이나 Windows 안전성을 반증하지 않는다. context가 연결 객체·socket보다 오래 유지되도록 실제 소유자를 바로잡아야 한다.

## B-ST2-4. 실패 정리의 close/read 동시 호출

**차단 / 새 테스트의 실패 관측·이식성 결함.**

`test_stream_concurrent_pull_send.cpp:295`의 main thread는 client가 `:107`의 동기 read 또는 `:100`의 write 중일 때 같은 Asio socket에 `:79`의 close를 호출한다. client 자체의 `:139` close와도 겹칠 수 있다. `client_socket_mutex`는 목록 조회·삽입만 보호하며 client의 read/write/close는 이 mutex를 사용하지 않는다.

bundled `core/external/boost/boost/asio/basic_stream_socket.hpp:46`의 thread safety 절은 일부 동기 send/receive/connect/shutdown 조합을 허용하지만 **close는 thread-safe가 아니라고 `:54`에 명시**한다. 따라서 “다른 thread에서 close하면 portable하게 blocking read가 해제된다”는 테스트 `:62`의 주석은 성립하지 않는다. 이 실패 경로 자체가 race·정리 정체를 만들 수 있어, watchdog 종료만으로 Core fq race의 재현을 식별하기도 어렵다.

socket 실행·취소·파괴가 Asio가 허용하는 동시성 모델을 따르도록 정리해야 한다. timeout을 늘리거나 Windows 테스트를 제외하는 것은 해결이 아니다. B-ST2-3의 context 수명과 이 동시 close 문제는 별개의 결함이다.

## W-ST2-1. hot path 비용과 5셀의 관측 범위

| 조건 | 코드에서 센 비용 |
|---|---|
| 경합 없는 일반 public receive 한 시도 | `socket_runtime.hpp:355`의 성공 CAS 1, `:387`의 release exchange 1, waiter 분기. 새 mutex는 없다. weak CAS의 spurious failure까지 포함한 절대 횟수 상한을 뜻하지 않는다. |
| ST-1 대비 public 해제 | release store가 release RMW로 바뀐다. 소스 연산 수가 하나라는 사실은 CPU 비용·명령 수가 같다는 뜻이 아니다. |
| command 한 건, async 없음 | 기존 sync 획득/해제 + command CAS 1 + release store 1. |
| command와 public 경합 | 실패 claim/대기표시 CAS, CV 대기; public release는 sync 획득과 broadcast를 추가한다. |
| 경합 없는 has_in의 xhas_in 경로 | 기존 mutex 1에 **CAS 1 + release RMW 1** 추가. buffered-part 조기 반환은 별도다. |

`get_events_internal()`은 POLLIN 검사를 요청받으면 `socket_base_api.cpp:911`에서 has_in을 호출한다. 따라서 실제 poller workload에서는 메시지가 없는 readiness 검사에도 추가 atomic 비용이 들 수 있다. 한 wait에서 여러 source를 재검사하거나 batch 없이 poll/recv를 반복하는 구성은 회귀 후보다. ST-2 보고서의 “has_in hot path 비용 증가 없음”은 mutex 개수만 같다는 뜻으로 한정해야 하며 비용 전체에 대한 설명으로는 틀리다.

현재 5셀의 **stream_tcp와 router_router_tcp는 public POLLIN poller를 사용하지 않는다**. `core/tests/perf/hotpath_bench.cpp:778`은 router_router_tcp를 run_one_way로 연결하고, `:177`은 blocking router_recv_part를 호출한다. stream_tcp는 `:579` → stream_recv_exact `:404`의 blocking recv_part다. 파일에 public poller 호출이 없으며 해당 recv 구현에도 has_in 호출이 없다. 그러므로 두 셀에서 has_in의 추가 비용이 직접 관측된다고 추정해서는 안 된다. release RMW·command 비용은 두 셀에서도 별도 회귀 후보이며, 정량 판정은 이번 읽기 리뷰에서 하지 않았다.

## W-ST2-2. 테스트 관측력과 기존 로그

echo의 payload 대조(`test_stream_concurrent_pull_send.cpp:129`, `:133`)와 총 완료 수 검사는 유지됐다. 그러나 `:271`의 thread 시작과 `:285`의 yield는 내부 fq 변경 순서를 고정하지 않는다. command 진입 결정·해제·새 lease·mutex 재검증·async 설치/해제를 교차시키는 rendezvous도 없다. 따라서 **실용적 stress 검사이며 결정적 소유권 프로토콜 검사는 아니다**. 3회 실패와 20회 성공만으로 특정 interleaving의 검출 보장을 주장할 수 없다.

bounded 100×40과 unbounded 40×40은 workload도 다르다(`:324`, `:331`). HWM을 재현에 유리한 조건으로 기록하는 것은 타당하지만 필수 원인이라고 단정할 수 없다. poll/recv 오류도 대부분 break로 합쳐진다(`:156`, `:168`). SNDTIMEO 30초 추가는 blocking send의 한 실패 상한을 제공하나 command 소유권 대기, client join, Core teardown 전체의 상한은 아니다. CTest 180초와 watchdog은 외부 종료 장치다.

| 자료 | 이번 리뷰에서 확인한 범위 |
|---|---|
| ST-2 보고서 `:90`, `:199` | 수정 전 3/3 SIGALRM(exit 142), 수정 후 20/20 PASS라는 작성자 기록. |
| `<scratch>/st2/baseline_run1.log`~`baseline_run4.log`, `baseline2_run1.log`~`baseline2_run3.log` | 모두 0줄·빈 파일. 종료 코드·정체 위치를 독립 확인할 원시 출력이 없다. 보고서 기록을 부정하지 않지만 fq 손상 또는 teardown 지점을 재검증한 것으로 세지 않는다. |
| `<scratch>/st2/tsan2_test_stream_packet_progress.log:351`, `:415` | epoch read/write 경고 확인. |
| 같은 로그 `:753`, `:756`, `:758` | shutdown_during_drain 실패, 8 Tests 1 Failures, 9 warnings 확인. |
| `<scratch>/st2/asan_new_test.log:1`, `:5` | 새 테스트 2/2 PASS, 해당 로그에 ASan/UBSan 보고 없음. B-ST2-3/4 부재 증명은 아니다. |
| ST-2 보고서 `:201`, `:202`, `:203` | 관련 95개·전체 210개·wake 반복 통과는 보고서 기록. 이번 리뷰에서는 재실행하지 않았다. |

다음 검증의 목적은 부하량 증가가 아니라 상태 전환을 고정한 배타·진행 확인이다. command의 지연 fallback, async 설치/해제 중 이미 진입한 has_in 또는 count-1 drain, public_waiting 등록 직후 release, blocking recv의 lease 없는 대기를 구분해 관측할 필요가 있다.

## W-ST2-3. 잔여 경계·RAII·주석·규칙 중복

- **control attach:** `socket_base_endpoint.cpp:650`, `:704`의 non-STREAM 직접 attach는 `socket_base_api.cpp:478`의 sync만 잡는다. public lease와 같은 배타가 아니다. 1차 W-1의 기존 항목이며 이번 신규 차단 수에 중복 계상하지 않았다. `socket_runtime.hpp:321`의 “모든 receive-state 접근의 유일한 gate” 주석은 이 경계까지 완료됐다고 읽히므로 범위를 바로잡아야 한다.
- **RAII:** command 소멸자 `socket_base_lifecycle.cpp:111`과 entry 소멸자 `socket_runtime.hpp:578`의 정상/early-return/stack-unwind 해제는 맞다. has_in 내부 scoped_lock이 entry보다 먼저 파괴되고, count-1 drain의 return들도 entry를 지난다. 반면 `receive_once_guarded()`는 `socket_base_msg.cpp:81`, `:115`에서 receive 호출 전에 수동 취득한 자원을 아직 RAII에 맡기지 않는다. receive/admission이 throw하면 해제가 보장되지 않는 기존 예외 경계는 남는다. 새 scope를 근거로 전체 receive의 예외 안전성을 선언하면 안 된다.
- **lock 순서:** 일반 whole-record/has_in의 lease→sync와 command의 sync→claim은 CV가 sync를 실제로 놓기 때문에 조사한 경로에서 순환하지 않는다. 단순히 “public은 word를 먼저 쥔다”는 `socket_base_lifecycle.cpp:72`의 설명만으로는 교착 부재를 설명할 수 없으며, 실패 claim 뒤 CV가 sync를 놓는다는 조건이 필요하다.
- **recursive CV:** 새 command CV는 `socket_runtime.hpp:540`의 recursive_mutex_t를 쓴다. 확인한 정상 호출 깊이는 1이지만 synchronization model §5 `:215`, §6 `:224`의 “CV에는 재진입하지 않는 lock만”이라는 명시적 규칙과는 일치하지 않는다. 기존 receive progress CV도 같은 타입이므로 이번에 생긴 교착이라고 단정하지 않았다. 중첩 count-1 drain 자체는 CV를 호출하지 않는다. 이 제약을 기존 §3.4 준수와 혼동하면 안 된다.
- **부정확한 주석:** `socket_runtime.hpp:496`에서 release 함수가 sync 해제 전 실행될 수 없다고 설명하지만, 실제 exchange는 mutex 밖에서 먼저 실행 가능하다. 지연되는 것은 `wake_receive_owner_waiter()`의 broadcast다. `:386`의 “no waiter ever spins”도 public/async 설치 CAS 루프에는 맞지 않는다.
- **종료 설명과 중복:** `socket_base_lifecycle.cpp:101`은 blocking receiver가 lease를 쥐고 잠들어 있다고 설명하나 실제 public wait는 lease 밖이다. 새 `socket_runtime.hpp:509`의 epoch 증가·waiter 확인·broadcast는 기존 `socket_base_lifecycle.cpp:1645`의 발행 규칙을 그대로 복제한다. 동일 progress 발행은 소유자 한 곳으로 통합할 대상이며, 이것을 별도 종료 규칙으로 늘릴 근거는 보고되지 않았다.

## S-ST2-3. 스펙 대조와 명료화 초안

소유 계층은 Core socket receive 상태와 socket 쪽 pipe 끝이다. synchronization model §3.1(`:71`, `:78`), §3.3(`:149`)은 command와 public 접근을 같은 소유권 경계에 두며, §3.4(`:170`)는 대기를 끝낼 실행자의 자원을 유지하지 못하게 한다. §4(`:193`)는 mutex의 실제 공유 상태·상대 실행자 근거를 요구한다. has_in이 pump와 fq partition을 변경하므로 배타가 필요한 이유는 있지만, 그것만으로 추가 비용이 0이라는 결론은 나오지 않는다.

ST-2 제안(`core-rf-ST-2-report.md:246` 이후)의 첫 문장과 “다른 주체의 진행에 필요한 자원을 쥐고 기다리지 않는다”는 문장은 타당하다. 반면 “진입하기 전에 표시를 다시 확인하면 된다”는 설명은 재검증 이후 async 해제까지 이어지는 보장을 빠뜨린다. 특정 재검증 알고리즘을 모든 구현에 의무화하기보다 다음 의미를 규정하는 편이 맞다. **스펙 파일은 수정하지 않았다.**

> 공개 수신, readiness 검사, 같은 물리 수신 queue를 사용하는 completion 처리와 command 적용은 수신 상태의 같은 소유권 경계를 따른다. 소유권 표시 방식이 바뀔 때는 이전 방식으로 진입한 실행이 끝나기 전에 다른 배타 방식의 실행을 허용하지 않는다. 이전 표시를 보고 아직 진입하지 않은 실행도 전환 뒤의 소유권 경계를 따라야 한다. 소유권을 기다리는 동안에는 현재 소유자의 진행과 해제에 필요한 자원을 유지하지 않는다.

이 문장은 공개 계약 완화가 아니다. 현재 §3.1 `:89`의 admission 상태어 turn bit와 별도의 receive_owner word·sync가 공존한다는 1차 S-4의 지적도 남는다. 위 명료화만으로 그 구현 형태나 recursive CV가 현 스펙과 같아지는 것은 아니다. 따라서 ST-2 보고서의 “어느 문장도 다른 동작이 되지 않는다”를 전체 준수 확인으로 채택하지 않는다.

설계 비교: 모든 public receive에 mutex를 추가하면 모드 간 배타는 단순해지지만 성공 경로 비용을 바꾼다. 기존 lease/mutex 전환 프로토콜을 끝까지 완성하는 안은 성공 경로 목표를 유지할 수 있지만 이미 진입한 async-mode 실행을 해제 전에 배제해야 한다. fq 전용 lock, 별도 timeout·retry 증가는 같은 소유권 결함의 해결이 아니다.

수정 전/후 규칙 수: ST-1→ST-2의 배타 방식은 **2→2**, 상태값은 **3→5**, receive-progress 발행 구현은 **1→2**다. async 값의 임시 command 겸용은 없어졌지만 “규칙 수 감소 완료”로 판정할 수 없다.

- 소유 계층: Core socket receive 상태·socket 쪽 pipe 끝.
- spec 조항: synchronization model §3.1/§3.3/§3.4/§4/§5, Polling §3의 owner detach/re-arm와 §4 completion owner·§5 poller 직렬화.
- 교차언어 대조: Framework runtime 수정이 아니다. C API 위의 pull/submit thread 구성에서 발생하는 Core 소유권 문제이며 언어별 Framework 수정은 검토 범위에 없다. ST-1의 C/C++ 실행 구성 비교는 보고서 근거로만 취급했다.
- 변경 분류: 목적은 **B(기존 결함 수정)**. 구현의 잔여 배타 공백과 신규 테스트 결함 때문에 현재 결과물을 승인하지 않는다.

차단 항목 수 / 채택 가능 여부: 4 / 현재 diff 채택 불가
