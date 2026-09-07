# ST-2 — ST-1 리뷰 차단 항목(B-1·B-2) 해소와 소유권 프로토콜 확정

- job: ST-2 (독립 리뷰 `doc/plan/c016-worklog/review-st1.md`의 지적 반영)
- worktree: `/home/hep7hep7/project/zlink-work/st1` (detached `1696ed55e5`), **커밋하지 않음**
- 변경 분류: **B (기존 결함)** — 공개 인터페이스·ABI·계약·옵션·스펙 변경 없음
  (`git diff --stat -- core/include core/src/libzlink.vers` 비어 있음)
- 사전 백업: `<scratch>/st1-before-st2.patch`(ST-1 상태), `<scratch>/test_stream_concurrent_pull_send.cpp.orig`
- `<scratch>` = `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad`

---

## 1. 수신 상태 소유권 프로토콜 (B-1의 답)

**불변식(코드 주석으로 `socket_runtime.hpp:325-347`에 명시):**

> 수신측 socket 상태(fair-queue 멤버십·partition, pipe activation, pipe termination,
> STREAM packet pump, readiness 조회)를 읽거나 쓰는 실행 주체는 어느 순간에도
> {public lease 보유자, async 실행자 모드에서 `receive.sync`를 쥔 주체, command turn}
> 중 **하나뿐**이다.

상태어 `receive_owner` 하나가 그 유일한 게이트다. 값의 의미는 서로 겹치지 않는다.

| 값 | 뜻 | 다른 진입자가 해야 할 일 |
|---|---|---|
| `available` | 주인 없음 | CAS로 취득 |
| `public` | lock-free public lease 보유자 1명 | CAS 재시도(기존 동작) |
| `public_waiting` | 위 + command turn 1명이 해제를 기다리는 중 | CAS 재시도 |
| `async` | async mailbox 실행자가 **설치되어 있는 지속 모드**. 이 모드에서는 모든 접근자가 `sync`를 잡는다 | `sync`만 잡고 진행 (안전) |
| `command` | command 1건 적용이 진행 중인 **일시 모드**. 그 주체는 상태어를 쥐는 동안 `sync`도 계속 쥔다 | `sync`에서 블록 → 획득 후 재검증 |

핵심 규칙 두 가지:

1. **command turn은 `async`를 빌려 쓰지 않는다.** 자기 값 `receive_owner_command`를 쓴다
   (`socket_runtime.hpp:461-470`, `:528`). 따라서 `async`의 의미는 다시 하나(설치된 실행자)뿐이다.
2. **mutex 전용으로 내려가기로 한 진입자는 mutex를 잡은 **뒤** 상태어를 재검증한다**
   (`enter_receive_exclusion()`, `socket_runtime.hpp:405-420`). `sync`를 잡고 나서도 상태어가
   여전히 `async`/`command`일 때만 mutex 전용으로 진행하고, 그 사이 일시 모드가 끝났으면
   `sync`를 놓고 lock-free lease를 다시 잡는다. 리뷰 B-1의 실행 순서(2단계에서 false를 받고
   지연된 진입자가 4단계의 새 lease 보유자와 겹치는 것)는 이 재검증에서 끊긴다.

**획득·해제 순서가 재검증을 의미 있게 만든다.** command turn은 `sync`를 **먼저** 잡고
상태어를 CAS하며(`socket_base_lifecycle.cpp:84-92`), 해제는 상태어를 먼저 지우고 `sync`를
나중에 놓는다(`:113-117`). 그러므로 "**`sync`를 쥔 상태에서 상태어가 `command`로 보이면
그것은 자기 thread의 command turn뿐**"이 성립한다 — 재검증이 같은 thread 재진입
(command 적용 중의 count-1 completion drain: `socket_base_api.cpp:1690` → `:1695`)을
교착 없이 통과시키는 근거다.

**command turn의 대기에는 spin이 없다**(W-2). public lease가 상태어를 쥐고 있으면
`public → public_waiting`으로 표시하고(`socket_runtime.hpp:484`) 수신 progress 채널에서
잔다(`:498`). lease 해제는 store가 아니라 exchange(RMW)이고, 이전 값이 `public_waiting`이면
그 자리에서 대기자를 깨운다(`socket_runtime.hpp:382-391`, `:532`). 표시부터 CV 대기 진입까지
`sync`를 놓지 않으므로 wake 유실이 불가능하고, timeout도 재시도 횟수도 필요 없다.

### 설계 비교

| 안 | 내용 | 판단 |
|---|---|---|
| A | 공개 receive 빠른 경로가 `receive.sync`를 잡는다 | 기각 — message마다 lock, hot path 회귀 |
| B | ST-1처럼 command가 `async` 값을 빌린다 | 기각 — 리뷰 B-1. `async`의 의미가 2개가 되고 지연된 fallback 진입자가 남는다 |
| **C (채택)** | 상태어에 `command`(+대기 표시 `public_waiting`)를 두고, mutex 전용 진입은 **획득 후 재검증** | 배타 규칙이 하나("상태어가 유일한 게이트, 놓친 자는 `sync`에서 기다렸다 재검증")로 서술된다. hot path take 1 CAS 유지 |
| D | fq 전용 lock | 기각 — 같은 불변식을 lock 2개로 나눔(§5 2행) |

상태 필드 수는 그대로 1개(`receive_owner`)다. 값은 3→5개로 늘었지만 **의미가 겹치는 값이
없어졌다**(리뷰가 지적한 "`async`의 의미 1개 → 2개"를 되돌리고, 대신 서로 배타적인 값으로
분리). 배타 방식은 여전히 2개(lock-free lease / `sync`)이며, 그 사이 전환 규칙이 하나
추가되는 대신 "전환 중 진입자"의 예외가 사라졌다.

---

## 2. 리뷰 항목별 처리

| 항목 | 처리 | 파일:행 |
|---|---|---|
| **B-1** 임시 async 해제 뒤의 배타 공백 | 해소. command 전용 상태값 + mutex 획득 후 재검증. 공개 receive·count-1 completion drain·`has_in()`이 모두 같은 진입 함수를 쓴다 | `socket_runtime.hpp:325-347`(불변식), `:349-380`(lease 취득), `:382-391`(해제/handoff), `:405-420`(재검증 진입), `:461-491`(command 취득/해제/대기표시), `:498-515`, `:519-537`(값), `:567-596`(RAII); `socket_base_lifecycle.cpp:60-122`, `:590`; `socket_base_msg.cpp:63`, `:103`; `socket_base_api.cpp:1039-1044`, `:1690-1701` |
| **B-2** Windows 컴파일 실패 | 해소. `arpa/inet.h`·`netinet/*`·`sys/socket.h`·`unistd.h`·raw `int` fd·`close()`·`MSG_NOSIGNAL` 제거, Boost.Asio(`test_stream_multiclient_delivery.cpp:6`와 같은 의존)로 재작성. 재현 파라미터(clients=100, M=40, HWM=4096, 64 B)와 무-sleep 동기화 유지 | `core/tests/integration/test_stream_concurrent_pull_send.cpp:14`(include), `:31-32`(net/tcp alias), `:60-81`(실패 경로 socket 목록·닫기), `:97-110`(write_all/read_all), `:113-142`(client thread), `:292-295`(정지 시 socket 닫기) |
| **W-2** yield 루프의 종료 관측 | 해소 — **spin 자체를 없앴다**. lease 해제가 직접 깨우는 handoff 대기(`progress_cv`)로 교체, timeout·재시도 카운터 없음. 추가로 종료가 관측되면(`_ctx_terminated`) 대기 전에 수신 progress 를 broadcast 하여, 종료 발행 전에 시작된 수신이 깨어나 ETERM을 보고 lease를 놓게 한다 | `socket_base_lifecycle.cpp:93-109`(대기), `:590`(`_ctx_terminated` 전달), `socket_runtime.hpp:484-515` |
| **W-1** `has_in()` | 해소(hot path 비용 증가 없음). 같은 진입 함수를 쓰고, lease를 잡은 경우에는 pump가 `sync` 아래에서 publish 하는 수신 progress 때문에 `sync`를 **추가로** 잡는다(lease → `sync` 순서, whole-record public receive와 동일). 이전에도 잡던 mutex 하나 그대로다 | `socket_base_api.cpp:1031-1045` |
| **W-1** control attach 경계 | 보고만 함. §5 참조 | — |
| **S-2** RAII·lock 순서 | 확인. §4 | — |
| **W-3** 비용 | §3 | — |
| **S-1** 주석 오류("receive exclusion first and sync second"가 구현과 반대) | 수정. 이제 주석과 구현이 모두 "`sync` 먼저, 상태어 나중, 해제는 역순"이고 그 이유(재검증의 근거)를 적었다 | `socket_base_lifecycle.cpp:69-73` |

### B-2 재현력 확인

재작성한 테스트가 재현력을 잃지 않았음을 **수정 없는 트리에서 직접 확인**했다
(`git checkout -- core/src` 후 dev 재빌드, `git stash` 미사용, patch는 `<scratch>/st2-current.patch`에 보관 후 복원).

| 트리 | 실행 | 결과 |
|---|---|---|
| 미수정(core/src revert) | `./bin/test_stream_concurrent_pull_send` ×3 | **3/3 FAIL** — 정체 후 완료하지 못하고 testutil의 watchdog `alarm(121)`(`core/tests/testutil.cpp:217`)에 SIGALRM으로 죽는다(exit 142). 로그 `<scratch>/st2/baseline*_run*.log` |
| 수정(ST-2 patch) | 같음, `--repeat until-fail:20` | 20/20 PASS (총 2.97 s, 회당 0.16 s) |

정체 시 blocking echo send가 영원히 park 하지 않도록 server socket에 `ZLINK_OPT_SNDTIMEO`
(=실패 상한 30 s)만 추가했다(`test_stream_concurrent_pull_send.cpp:250-258`). 성공 경로는
이 값에 도달하지 않는다. 다만 미수정 트리에서는 fq 손상 자체로 teardown이 끝나지 않아
watchdog 종료로 FAIL 하며, 이는 "실패를 메시지로 보고"가 아니라 "실패를 종료 코드로 보고"다.

---

## 3. 비용 (W-3)

| 경로 | ST-2가 추가한 것 |
|---|---|
| **(a) 경합 없는 public recv** (`zlink_stream_recv_packet` → `receive_once_guarded` → `xrecv`/pump → `_fq.recvpipe`) | 취득 **CAS 1회**(기존과 동일), 해제 **store → exchange 1회**(RMW 1개, 명령 수 동일) + 예측 가능한 분기 1개(`previous == public_waiting`, 경합 없으면 항상 false). mutex 없음. 재검증 분기는 CAS가 성공하면 실행되지 않는다 |
| **(b) command 적용 1건, async 실행자 없음** | 기존 `receive.sync` lock/unlock 옆에 **CAS 1회 + release store 1회**. ST-1과 동일 |
| **(b') command 적용 1건, 장기 async 실행자 있음** | 실패 CAS 1회(`already_held`), 해제 store 없음 |
| **(b'') command와 public receive 경합** | command 쪽: 실패 CAS 1회 + `mark_public_lease_waiter` CAS 1회 + CV 대기 1회(spin·yield 없음). public 쪽: 해제 exchange가 `public_waiting`을 보면 `sync` 획득 + broadcast 1회 |
| **public receive가 command turn과 만난 경우** | ST-1에서는 mutex 전용으로 내려가 성공 수신에 mutex 비용이 붙었다. ST-2에서는 `sync`에서 기다렸다가 **lease로 복귀**하므로, command turn이 끝난 뒤의 수신은 다시 lock-free다 |
| **`has_in()`** | 이전: mutex 1개. 이후: CAS 1개 + 같은 mutex 1개(경합 없을 때). mutex는 늘지 않았다 |
| pipe/engine/session I/O thread, public send | 없음 |

벤치마크는 실행하지 않았다(브리프 지시). 위 표는 코드로 셀 수 있는 연산만 적은 것이며,
스펙 §8의 셀별 명령 수 요구를 충족했다는 주장은 하지 않는다.

---

## 4. lock 순서와 해제 보장 (S-2)

**순서: `public_api_sync` → `command_owner_sync` → `receive.sync` → `receive_owner` 상태어.**

| 단계 | 근거 |
|---|---|
| `public_api_sync` 획득 | `socket_base_lifecycle.cpp:488-491` (`process_commands`의 turn) |
| → `command_owner_sync` | 같은 파일 `:492` (`scoped_lock_t command_owner (receive.command_owner_sync)`) |
| → `receive.sync` | `:81` (command turn 생성자의 `_runtime.sync.lock ()`) |
| → 상태어 | `:83-92` (`try_acquire_receive_owner_for_commands`) |

역방향 획득 없음을 확인한 근거:

- `command_owner_sync`를 잡는 곳은 3군데뿐이며 모두 `public_api_sync` 취득 이후이거나 그것을
  요구하지 않는 설치/정지 경로다: `socket_base_lifecycle.cpp:492`, `:994`
  (`start_async_mailbox_processing`), `:1042`. `receive.sync`를 쥔 채 잡는 곳은 없다
  (grep: `command_owner_sync` 전체 4건 중 선언 1건 제외).
- `receive.sync`를 쥔 채 `public_api_sync`를 잡는 경로 없음: `socket_public_api_lock_scope_t`
  사용처는 모두 공개 API 진입부다(`socket_base_api.cpp:668`·`:674`·`:682`·`:725`·`:740`·`:751`·
  `:756`·`:775`·`:790`·`:812`, `socket_base_endpoint.cpp:183`·`:223`·`:1134`·`:1166`,
  `socket_base_msg.cpp:679`, `socket_base_flow_state.cpp:54`, `socket_base_lifecycle.cpp:490`).
  이들 중 `receive.sync` 또는 lease를 보유한 상태에서 실행되는 것은 없다.
  특히 평범한 public recv(`socket_base_msg.cpp:699`)는 `public_api_sync`를 아예 잡지 않으므로,
  command turn이 그 두 mutex를 쥔 채 lease 해제를 기다려도 순환이 생기지 않는다.
- lease를 쥔 구간에서 실행되는 코드는 `receive_()`(xrecv/pump)와 count-1 completion drain뿐이고,
  그 안에서 `process_commands`나 `public_api_sync`를 잡지 않는다(`socket_base_api.cpp`의
  `process_commands` 호출부 `:741`·`:776`·`:838`·`:1354`는 모두 lease 밖이다).
- 상태어를 쥔 채 `sync`를 **나중에** 잡는 유일한 주체는 whole-record public receive
  (`socket_runtime.hpp` `socket_receive_record_scope_t`)와 `has_in()`의 lease 분기
  (`socket_base_api.cpp:1041-1043`)다. 둘 다 lease → `sync` 순서로 같다. command turn은
  반대 순서(`sync` → 상태어)지만 **상태어를 기다리며 `sync`를 쥐지 않는다**(대기 시 CV가
  `sync`를 놓는다), 그러므로 두 순서가 공존해도 순환 대기가 성립하지 않는다.

**해제 보장(RAII):**

- command turn: `socket_command_receive_turn_t` 소멸자(`socket_base_lifecycle.cpp:111-118`)가
  상태어 해제 → `sync.unlock()`을 수행한다. 생성자는 **성공적으로 반환할 때만** 두 자원을
  쥔 상태로 나오고, 재시도 경로는 매 반복에서 `sync`를 스스로 놓는다(`:98`, `:108`).
  scope 안에는 `cmd.destination->process_command (cmd)` 하나뿐이므로(`:591`) 정상 종료·
  early return·예외 stack unwinding 모두 소멸자를 지난다. `already_held`인 경우 남의
  상태어를 해제하지 않는다(`_owns_receive_owner == false`).
- 진입자: `socket_receive_entry_scope_t`(`socket_runtime.hpp:567-596`)가 lease면 lease를,
  `sync`면 `sync`를 해제한다. `has_in()`(`socket_base_api.cpp:1039`)과 count-1 completion
  drain(`:1695`)이 이 scope를 쓰므로, 이전에 `return` 지점마다 흩어져 있던 해제가 사라졌다
  (drain 쪽은 `release_public_receive_lease()` 직접 호출 2곳이 없어졌다).
- `receive_once_guarded`는 기존 구조를 유지한다(진입만 `enter_receive_exclusion()`으로 교체).

---

## 5. 남은 경계

1. **control attach (리뷰 W-1 2번, 보고만)** — non-STREAM의 기본 connect는 command loop 밖에서
   `socket_base_endpoint.cpp:650`·`:698`이 직접 `attach_pipe()`를 호출하고, 실제
   `xattach_pipe()` 보호는 `socket_base_api.cpp:478`의 `receive.sync` 하나뿐이다. 이 경로는
   상태어를 취득하지 않으므로, lease를 쥔 수신과 동시 control connect가 겹치면 같은 fq
   partition을 동시에 만질 수 있다. `socket/README.ko.md:54`가 control 동시 호출을 허용하므로
   실제 가능한 조합이다. **이번 patch는 이 경계를 닫지 않았다.** 닫으려면 attach 지점도
   §1의 진입 함수를 쓰면 되지만, 그 경로는 pipe 수명·endpoint 상태와 함께 움직여서
   `socket_base_endpoint.cpp` 전반의 순서 검토가 필요하다 — ST-2 범위를 넘는다.
2. **`progress_epoch`의 비원자 읽기(TSan 1건)** — `receive_once_guarded`의 lease 분기가
   `*observed_epoch_out_ = runtime_.progress_epoch;`를 `sync` 없이 읽는다(ST-1·ST-2가 건드리지
   않은 줄). 쓰기는 `notify_receive_progress_locked()`가 `sync` 아래에서 한다. 설계상 lock-free
   경로가 epoch를 mutex 없이 읽는 것이므로 **기존 성질**이지만, ST-2가 "일시 모드 뒤에는 lease로
   복귀"시키면서 이 분기가 도달 가능해진 조합이 늘어 TSan에 1건 노출된다(§6.3). 의미상
   최악의 결과는 CV 대기 1회 추가/생략이며(대기 판정은 `sync` 아래에서 다시 비교한다
   `socket_base_lifecycle.cpp:1658`), wake 유실은 되지 않는다. `progress_epoch`를
   `std::atomic<uint64_t>`(쓰기 release, lock-free 읽기 acquire)로 바꾸면 사라진다 — 별도 항목.
3. **`array_t` 자체는 여전히 비원자적** (ST-1 §8.3과 동일). 이번에도 "동시에 만지지 않는다"로 해결했다.
4. **reqrep의 readiness 우회 경로**(리뷰 S-3 후단, `socket_base_api.cpp:950` → `_lb.has_out()`)는
   송신측 대칭 후보로 남는다. 이번 범위 밖.

---

## 6. 검증

모두 포그라운드, `JOBS=4`, 빌드 전 `pgrep -c -x ninja` 확인.

### 6.1 기능

| 항목 | 명령 | 결과 |
|---|---|---|
| dev 빌드 | `JOBS=4 scripts/build-core.sh dev` | 성공 |
| 새 테스트 반복 | `ctest -R '^test_stream_concurrent_pull_send$' --repeat until-fail:20` | **20/20 PASS** (2.97 s) |
| 새 테스트 (미수정 트리) | 직접 실행 ×3 | 3/3 FAIL (watchdog SIGALRM) |
| 관련 suite | `ctest -R 'stream\|pipe\|wake\|hwm\|flow\|credit\|poll\|completion\|router\|dealer\|sub' --repeat until-fail:3` (95개) | **100% passed, 0 failed / 95** (349.7 s) — has_in 수정 후 재실행 |
| 전체 | `ctest -E hotpath_gate` (210개) | **100% passed, 0 failed / 210** (244.7 s) — has_in 수정 후 재실행 |
| lost-wake | `ctest -L wake-invariant --repeat until-fail:20` (4개) | 통과 (exit 0, 618.5 s) |
| 공개 인터페이스 | `git diff --stat -- core/include core/src/libzlink.vers` | **비어 있음** |

### 6.2 TSan (GCC `-fsanitize=thread`, LTO OFF, `setarch x86_64 -R`, **suppression 파일 없음**)

| 테스트 | 결과 | 경고 |
|---|---|---|
| `test_stream_concurrent_pull_send` | 2/2 OK | **0** |
| `test_stream_socket` | 11/11 OK | **0** |
| `test_stream_threadsafe` | 5/5 OK | **0** |
| `test_stream_packet_progress` | 8 중 1 FAIL (`test_shutdown_during_drain`) | 9 (8 = mailbox `activate_if_command_pending`↔`reschedule_if_needed` 기존 쌍, 1 = §5.2의 `progress_epoch`) |
| `unittest_phase3_request_reply_owners` | 16/16 OK | 2 (전부 위 mailbox 쌍) |

`test_stream_packet_progress`의 FAIL과 mailbox 경고 8건은 ST-1이 `core/src` 되돌리기 대조로
기존 항목임을 확인한 것과 동일한 서명이다(ST-1 보고서 §7.1). 이번 job은 그 대조를 재실행하지
않았다.

### 6.3 TSan이 잡아낸 회귀 1건(수정 완료)

W-1로 `has_in()`을 lease 화한 1차 구현에서 `test_stream_packet_progress`의 경고가 8 → 10으로
늘었다. 새 2건은 모두 `progress_epoch` 주소였고 그 중 하나가
`stream_t::pump_packet_receive_queue()` → `notify_receive_progress_locked()`를 **`sync` 없이**
실행하는 것이었다 — pump가 progress를 `sync` 아래에서 publish 한다는 전제를 lease 단독 진입이
깼다. `has_in()`이 lease를 잡은 경우 `sync`를 추가로 잡도록 고쳐(`socket_base_api.cpp:1041`)
재실행하니 그 경고가 사라졌다(10 → 9). 로그: `<scratch>/st2/tsan_*.log`(1차),
`<scratch>/st2/tsan2_*.log`(수정 후).

### 6.4 ASan (`-fsanitize=address,undefined`, ST-1이 남긴 `core/build-asan` 이어서 완료)

| 테스트 | 명령 | 결과 |
|---|---|---|
| `test_stream_concurrent_pull_send` | `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./build-asan/bin/...` | **2/2 PASS, AddressSanitizer/UBSan 보고 0건** |

---

## 7. 스펙 대조

`core/doc/spec/core/systems/11-synchronization-model.ko.md`를 다시 읽었고 **수정하지 않았다.**
§3.1(turn), §3.4(대기 전 해제), §4(per-message lock의 소유 근거), §5(위반 목록) 중 어느 문장도
이번 변경으로 다른 동작이 되지 않는다.

- §3.1 "command 적용은 turn 안에서 한다 / 자주 실행되니 lock 없이 둔다는 예외는 두지 않는다":
  ST-1이 시작한 방향을 유지하되, 예외가 "전환 중 진입자"로 되살아나던 구멍을 닫았다.
- §3.4 "대기 전에 놓는다": command turn은 상태어를 기다릴 때 `sync`를 CV에 넘기고
  (`socket_runtime.hpp:498-503`), lease 보유자는 대기 전에 lease를 놓는다(기존 구조 유지).
- §4 "per-message lock의 소유 근거": 경합 없는 public recv에 lock을 추가하지 않았다.

### 제안(스펙 편집 아님) — §3.1/§3.4 명료화 문장

> 공개 수신, readiness 검사, 같은 물리 수신 queue를 사용하는 completion 처리와 command 적용은
> 수신 상태의 같은 소유권 경계를 따른다. 소유권의 표시 방식이 바뀌는 동안에도 그 경계는
> 끊기지 않는다: 이전 표시를 보고 진입 방식을 정한 실행 주체는 실제로 진입하기 전에 그 표시를
> 다시 확인해야 하며, 표시가 바뀌었으면 새 방식으로 다시 진입한다. 소유권을 기다리는 주체는
> 기다리는 동안 다른 주체의 진행에 필요한 자원을 쥐지 않는다.

---

## 8. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/sockets/common/socket_runtime.hpp` | 소유권 프로토콜(값 2개 추가, 진입/해제/대기 함수, 불변식 주석), `socket_receive_entry_scope_t` |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp` | `socket_command_receive_turn_t`(sync→상태어, handoff 대기, 종료 관측), 호출부 |
| `core/src/runtime/sockets/common/socket_base_msg.cpp` | `receive_once_guarded`의 진입을 `enter_receive_exclusion()`으로 |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | count-1 completion drain, `has_in()` |
| `core/tests/integration/test_stream_concurrent_pull_send.cpp` | Boost.Asio 재작성(B-2), 실패 상한용 `SNDTIMEO` |
| `core/tests/CMakeLists.txt` | 테스트 등록(ST-1과 동일) |

## 9. 멈춘 지점 / 남긴 것

- 시간 상한(2.5 h)을 검증 단계에서 초과했다. 초과분은 (a) 미수정 트리 대조 재현, (b) TSan
  회귀 발견 후 재빌드·재실행, (c) has_in 수정 후 suite/전체 ctest 재실행에 썼다.
- 하지 않은 것: 성능 측정(브리프 지시), `test_stream_packet_progress` 기존 실패의 대조 재실행,
  §5.1 control attach 경계 수정, §5.2 `progress_epoch` 원자화.
