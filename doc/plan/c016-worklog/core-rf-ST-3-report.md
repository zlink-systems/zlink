# core-rf-ST-3 — review-st2 차단 항목(B-ST2-1~4) 수정 보고서

- 작업 트리: `/home/hep7hep7/project/zlink-work/st1`, HEAD `1696ed55e5b9c6928c6be62c15932c530c5521cf`. **커밋하지 않았다.** 변경분은 uncommitted diff로 남는다.
- 착수 전 스냅샷: `<scratch>/st2-before-st3.patch`(ST-2 상태 diff), `<scratch>/st2-before-st3-test.cpp`(ST-2 상태 신규 테스트). ST-3 최종본은 `<scratch>/st3/st3-full.patch`.
- 스펙 파일은 읽기만 했고 **수정하지 않았다**(`core/doc/spec/core/systems/11-synchronization-model.ko.md` §3.1/§3.4/§5/§6).
- `core/include/**`·`core/src/libzlink.vers` 변경 없음(§6 검증표).
- 이하 행 번호는 대상 worktree 최종 상태 기준. `<scratch>` = `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad`.

---

## 1. 리뷰 항목별 조치

| 항목 | 조치 | 위치 |
|---|---|---|
| **B-ST2-1** async→available 전환이 진행 중인 mutex 수신을 배제하지 않음 | `release_receive_sync_from_async_owner()`가 `receive.sync`를 잡고 store 하도록 변경. 호출자 4곳은 그대로 두고 전환 자체를 프로토콜 안으로 넣었다. 불변식을 함수 주석과 프로토콜 주석에 명시. | `socket_runtime.hpp:452-469`(주석+`scoped_lock_t lock (sync)`), 호출자 `socket_base_lifecycle.cpp:1009`(설치 실패), `:1337`(idle detach), `:1604`(종료 detach), `:1633`(quiesce detach) |
| **B-ST2-2** `progress_epoch` data race | `progress_epoch`→`std::atomic<uint64_t>`, `waiters`→`std::atomic<uint32_t>`. lock-free lease 분기의 snapshot은 acquire load, 발행은 release RMW. | 선언 `socket_runtime.hpp:582-587`, 발행 `:545-554`, snapshot `socket_base_msg.cpp:72`·`:108`, 테스트 훅 snapshot `socket_base_lifecycle.cpp:936`, 대기측 `socket_base_lifecycle.cpp:1661-1670` |
| **B-ST2-3** Asio socket이 io_context보다 오래 삶 | client thread가 자기 `io_context`와 `tcp::socket`을 **선언 순서로** 소유한다(context 먼저 선언 → socket 나중 파괴 후 context 파괴). socket을 harness의 `shared_ptr` 목록에 넣던 경로(`publish_client_socket`/`client_sockets`)를 삭제해 socket이 thread 밖으로 새지 않는다. | `test_stream_concurrent_pull_send.cpp:143-148`(소유 선언+주석), harness에서 `client_socket_mutex`/`client_sockets`/`close_client_sockets` 제거 `:44-61` |
| **B-ST2-4** 실패 정리의 close/read 동시 호출 | main thread의 `close_client_sockets()` 삭제. 실패·정지 경로는 `harness.stop` 플래그만 세우고, client가 **자기 thread에서** `socket.cancel()` 한다. 동기 `net::read`/`net::write`를 `async_read`/`async_write` + `io_context::run_for()` 루프로 바꿔 취소 지점을 소유 thread 안에 두었다. | `run_bounded()` `:79-112`(취소는 `:105`), `write_all`/`read_all` `:114-135`, main 정리 `:325-330` |
| **W-ST2-3** command CV의 recursive lock | command 대기를 `receive.sync`(recursive)에서 분리해 **비재귀 `mutex_t lease_handoff_sync` + 전용 `lease_handoff_cv`** 로 옮겼다. 이 lock은 어떤 실행도 중첩 획득하지 않는다(§5/§6 준수). `receive.sync`는 count-1 completion drain이 재진입하므로 CV를 실을 수 없어 그대로 두었다. | 선언 `socket_runtime.hpp:588-602`, 대기 `:526-543`, 깨우기 `:572-577` |
| **W-ST2-3** 부정확한 주석 | release exchange가 mutex 밖에서 먼저 실행될 수 있다는 점, 지연되는 것은 broadcast뿐이라는 점, "아무도 spin하지 않는다"가 아니라 "parked turn이 word를 polling하지 않는다"라는 점으로 정정. blocking receiver가 lease를 쥐고 잠든다는 설명도 "receiver는 lease 밖에서 progress 채널에 park한다"로 정정. | `socket_runtime.hpp:396-401`, `socket_base_lifecycle.cpp:101-110` |
| **W-ST2-3** progress 발행 중복 | ST-2가 새로 만든 `broadcast_receive_progress_locked()`를 삭제하고, epoch 증가·waiter 확인·broadcast 규칙을 `socket_receive_runtime_t::publish_receive_progress_locked()` **한 곳**에 두었다. `socket_base_t::notify_receive_progress_locked()`가 이 함수로 위임하고, command turn의 종료 edge도 같은 함수를 부른다. 발행 구현 수 2 → 1. | 구현 `socket_runtime.hpp:545-554`, 위임 `socket_base_lifecycle.cpp:1650-1654`, command turn `socket_base_lifecycle.cpp:109` |
| **W-ST2-3** "모든 receive-state 접근의 유일한 gate" 주석 범위 | control attach가 아직 포함되지 않음을 주석에 명시했다. | `socket_runtime.hpp:322-330` |
| **W-ST2-1** hot path 비용 | §5에 그대로 인용해 남겼다. 이번 변경은 lease take/release 명령 수를 바꾸지 않는다. | — |
| **W-1 / control attach** | **수정하지 않았다**(지시대로). §7의 남은 경계에 기술. | — |

---

## 2. `receive_owner` 전이표(최종)

값은 `socket_runtime.hpp:558-570`의 5개다.

| 출발 → 도착 | 실행 주체 · 연산 | 배타 근거 |
|---|---|---|
| available → public | public recv / has_in / count-1 drain 의 weak CAS(acquire) | `sync` 없음. CAS 성공 자체가 배타. `socket_runtime.hpp:367` |
| public → public_waiting | command 후보의 strong CAS(acq_rel) | `sync` 보유. 소유권은 여전히 lease 보유자. `:512` |
| public / public_waiting → available | lease 보유자의 exchange(release) | mutex 밖. previous==public_waiting이면 `lease_handoff_sync` 아래 broadcast. `:394-406` |
| available → command | command turn의 strong CAS(acquire) | `sync` 보유. 적용 내내 유지. `:474` |
| command → available | command 소멸자의 release store | **store 후 `sync` unlock** `socket_base_lifecycle.cpp:115-120` |
| available → async | 설치자의 weak CAS(acquire) | `socket_runtime.hpp:443`. 이후 command_owner_sync 아래 설치 |
| async → async | 설치자=성공 간주, command=already_held, entrant=`sync`+재검증 | `:444`, `:475`, `:428` |
| **async → available** | 설치 실패 / idle·종료·quiesce detach 의 release store | **`receive.sync` 보유 아래 store**. `socket_runtime.hpp:462-469`. 이번 수정 지점 |

### 각 값에서 fq/pipe receive state를 만질 수 있는 주체

| word 값 | fq·pipe receive state를 만질 수 있는 실행 | 근거 |
|---|---|---|
| `available` | 없음(누구도 진입 중이 아님). 다음 진입자는 CAS나 `sync`+재검증으로 값을 먼저 바꾼다 | `enter_receive_exclusion():420` |
| `public` | CAS에 성공한 **그 한 명**의 public receive / has_in / count-1 drain. 다른 entrant는 CAS 실패 후 `sync`를 잡아도 word가 public이므로 `sync`를 놓고 재시도한다 | `:368`, `:428-434` |
| `public_waiting` | `public`과 동일한 한 명. 추가된 것은 "command turn이 park해 있다"는 표시뿐 | `:506-519` |
| `command` | `sync`를 쥔 **그 command turn** 하나. 같은 thread의 중첩 count-1 drain은 recursive `sync` 재진입 후 word==command를 관측해 통과한다 | `socket_base_lifecycle.cpp:74-121`, `socket_runtime.hpp:426` |
| `async` | `sync`를 쥔 실행 아무나(하나씩). async executor의 command 적용, public recv/has_in의 mutex 분기, control attach가 모두 `sync` 아래 직렬화된다 | `:428`, `socket_base_api.cpp:1041` |

**모드 전환 불변식(주석에 기술, `socket_runtime.hpp:452-469`)**: *어떤 배타 형식으로도, 이전 형식으로 진입한 실행이 끝나기 전에 새 형식의 실행이 시작될 수 없다.* available→public/command 방향은 CAS 자체가, available→async 와 async→available 방향은 `sync` 가 이를 보장한다. B-ST2-1의 반례(1: P가 `sync`를 쥐고 async 재검증 → 2: P 지연 → 3: A가 detach store → 4: R이 lease 취득)는 3번에서 A가 P의 `sync`를 기다리게 되어 성립하지 않는다.

---

## 3. lock 순서

```
_transport_pair_owner_progress_sync  ─┐
_completion_owner_sync               ─┼─→  receive.sync  ─→  lease_handoff_sync
public API turn / command_owner_sync ─┘                  ─→  monitor.sync(기존)
```

- `receive.sync` 를 쥔 상태에서 위쪽 lock 들을 잡는 경로는 없다. 이번에 새로 생긴 방향은 `_transport_pair_owner_progress_sync → receive.sync`(idle detach `socket_base_lifecycle.cpp:1337`) 하나이며, 역방향은 코드에 없다.
- `lease_handoff_sync` 는 **최하위**다. 잡은 채로 다른 lock을 잡지 않으며, 대기측은 `sync`를 잡은 뒤 `lease_handoff_sync`를 잡고 **`sync`를 먼저 놓는다**(`socket_runtime.hpp:531-543`). 해제측은 `lease_handoff_sync` 하나만 잡는다(`:572-577`).
- public 분기의 순서는 여전히 word(lease) → `sync` 이며(whole-record, has_in), command turn 은 `sync` → word 다. 순환이 없는 이유는 "CAS 실패 후 `sync`를 실제로 놓는다"(`:434`)와 "command 대기가 `sync`를 놓는다"(`:533`)는 두 조건이며, 주석에 그대로 적어 두었다.

---

## 4. TSan 잔여 경고(GCC `-fsanitize=thread`, LTO OFF, `setarch x86_64 -R`, **suppression 파일 없음**)

로그: `<scratch>/st3/tsan_*.log`.

| 테스트 | 종료 | 경고 수 | 분류 |
|---|---|---|---|
| `test_stream_concurrent_pull_send` | 2/2 OK | **0** | — |
| `test_stream_socket` | 11/11 OK | **0** | — |
| `test_stream_threadsafe` | 5/5 OK | **0** | — |
| `test_stream_packet_progress` | 8 중 1 FAIL(`test_shutdown_during_drain`) | **8** | 8/8 전부 `zlink::mailbox_t::activate_if_command_pending(bool)` ↔ `reschedule_if_needed()` 쌍. **기존 mailbox 부채** |
| `unittest_phase3_request_reply_owners` | 16/16 OK | **3** | `mailbox_t::reschedule_if_needed()` 1건, `mailbox_t::recv(command_t*,int,bool)` 2건. **기존 mailbox 부채** |
| `test_two_poller_wake` | 4/4 OK | **0** | — |

- **receive 소유권 관련 경고: 0건.** ST-2가 남긴 `progress_epoch` read/write 쌍(`socket_base_msg.cpp` ↔ `notify_receive_progress`)이 사라졌다(`test_stream_packet_progress` 9 → 8).
- 남은 11건의 스택은 모두 `zlink::mailbox_t` 안이며 접근 주소도 mailbox heap block(`socket_base_t::socket_base_t` 안 `mailbox_t::mailbox_t`)이다. receive runtime(`receive_owner`, `progress_epoch`, `waiters`, fq)에 대한 경고는 하나도 없다.
- `test_shutdown_during_drain` FAIL은 ST-1이 `core/src` 되돌리기로 기존 항목임을 확인한 것과 같은 서명(`transport did not queue all fragments`)이다. 이번 job은 그 대조를 재실행하지 않았다.

---

## 5. 검증표

| 항목 | 명령 | 결과 |
|---|---|---|
| dev 빌드 | `JOBS=4 scripts/build-core.sh dev` | 성공(경고 없음) |
| 신규 테스트 | `ctest -R stream_concurrent_pull_send --repeat until-fail:20` | **20/20 PASS**(5.3 s) |
| 관련 suite | `ctest -R 'stream\|pipe\|wake\|hwm\|flow\|credit\|poll\|completion\|router\|dealer\|sub\|pair' --repeat until-fail:3` (98개) | **100% passed, 0 failed / 98**(345.9 s) |
| 전체 | `ctest -E hotpath_gate` (210개) | **100% passed, 0 failed / 210**(244.5 s) |
| lost-wake | `ctest -L wake-invariant --repeat until-fail:20` (4개) | **exit 0**(632.1 s) |
| TSan | 위 §4 | receive 소유권 경고 0, 잔여 11건 전부 mailbox |
| ASan/UBSan | `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./core/build-asan/bin/test_stream_concurrent_pull_send` | **2/2 PASS, 보고 0건**(B-ST2-3 수정 후) |
| 공개 인터페이스 | `git diff --stat -- core/include core/src/libzlink.vers` | **비어 있음** |

### 5.1 재현력 재확인(리뷰 W-ST2-2·W-4에 대한 보강)

리뷰가 "baseline 로그가 빈 파일"이라 지적했으므로, **재작성한 테스트로** 대조를 다시 실행했다. `core/src` 만 `git checkout -- core/src` 로 되돌리고(테스트·CMake는 유지) dev 재빌드 후 직접 실행:

| run | 종료 코드 | 관측 |
|---|---|---|
| 1 | 134 (abort) | `unbounded_hwm` FAIL — recv 1574/1600 에서 정체, 이어서 `Assertion failed: _pipes.empty () (core/src/runtime/sockets/internal/fq.cpp:39)` |
| 2 | 134 (abort) | `bounded_hwm` FAIL — recv 3964/4000, 같은 `fq.cpp:39` assertion |
| 3 | 134 (abort) | `bounded_hwm` FAIL — recv 3947/4000, 같은 `fq.cpp:39` assertion |

원시 로그: `<scratch>/st3/baseline_run{1,2,3}.log`. ST-2 판의 watchdog SIGALRM(142)과 달리 이번에는 **fq 자체의 assertion**이 남아 실패 지점이 fq 소유권임을 직접 보여 준다. 되돌린 뒤 patch 재적용·재빌드하고 신규 테스트 20/20 을 다시 확인했다.

이것은 여전히 확률적 stress 검사다. 특정 interleaving 검출 보장을 주장하지 않는다.

---

## 6. hot path 비용

- **경합 없는 public receive 한 번**: take CAS 1(`socket_runtime.hpp:367`) + release exchange 1(`:402`). ST-2 대비 **변화 없다**. 이번 수정은 lease 경로에 명령을 추가하지 않았다.
- **epoch snapshot**: `progress_epoch` 가 일반 `uint64_t` 에서 `std::atomic<uint64_t>` 로 바뀌었다. x86-64 에서 acquire load 는 일반 mov 이고 release RMW(`fetch_add`)는 `lock xadd` 다. lease 경로가 하는 것은 **load 쪽**이므로 성공 경로 명령 수는 그대로이고, 컴파일러 재배치 자유도만 줄어든다. 발행 쪽(`publish_receive_progress_locked`)은 이미 `sync` 아래이며 `++` 가 `lock xadd` 로 바뀐다.
- **async→available 전환**: `sync` lock/unlock 1쌍이 추가된다. 이 경로는 executor 설치 실패와 detach 뿐이며 **수신 hot path가 아니다**.
- **command 대기**: `receive.sync` CV 대신 `lease_handoff_sync` lock/unlock 1쌍 + CV. 대기가 실제로 일어날 때만 발생하며 lease 해제 측은 previous==public_waiting 일 때만 이 lock 을 잡는다(기존에도 `sync`를 잡았으므로 lock 수는 동일).
- W-ST2-1 이 지적한 **ST-1→ST-2 의 비용 증가(release store→RMW, has_in 의 CAS+RMW 추가)는 그대로 남는다.** 이번 job 은 그 항목을 줄이지도 늘리지도 않았고, 정량 측정도 하지 않았다(브리프의 검증 목록에 벤치가 없다). `hotpath_gate` 는 제외한 채 전체 ctest 를 돌렸다.

---

## 7. 남은 경계

1. **control attach (W-1 / W-ST2-3 첫 항목) — 지시대로 수정하지 않았다.** `socket_base_endpoint.cpp:650`, `:704` 의 non-STREAM 직접 attach 와 `socket_base_api.cpp:478` 계열은 `receive.sync` 만 잡고 `receive_owner` word 를 잡지 않는다. 따라서 word 가 `public` 인 동안(= lock-free lease 진행 중) 이 attach 가 `sync` 만으로 fq 등록을 바꿀 수 있다. 이는 async 모드에서만 안전하고 lease 모드에서는 배타가 아니다. 이번 프로토콜은 "public API 수신 / command turn / async executor" 세 주체에 대해서만 완결이며, 주석(`socket_runtime.hpp:322-330`)에 그 범위를 명시했다. 닫으려면 attach 경로도 `socket_receive_entry_scope_t` 를 쓰도록 바꿔야 하고, 그 경우 attach 가 pipe lifecycle lock 과 receive lease 를 함께 잡게 되므로 lock 순서를 다시 세워야 한다.
2. **공정성·기아**: lease 해제는 소유권 양도가 아니다. 해제 직후 다른 public receiver 가 available 을 먼저 잡을 수 있고 `lease_handoff_cv` 에도 FIFO 보장은 없다. wake 유실은 없지만 command turn 의 유한 시간 진입은 "receive 시도가 유한하다"는 전제 위에서만 성립한다(S-ST2-2 유지).
3. **`receive_once_guarded()` 의 예외 안전성**: `socket_base_msg.cpp:74`, `:110` 부근에서 receive 호출 전에 수동 취득한 자원은 아직 RAII 가 아니다. ST-2 가 지적된 그대로 남는다.
4. **mailbox TSan 부채 11건**: `activate_if_command_pending` ↔ `reschedule_if_needed` ↔ `recv`. receive 소유권과 무관한 별도 항목이며 이번 범위에서 손대지 않았다.
5. **`_ctx_terminated` 는 `std::atomic<bool>` 이 아닌 곳에서도 읽힌다**(`socket_base_lifecycle.cpp:1673`). 기존 코드이며 TSan 경고로 나타나지 않았다. 이번 audit 범위(lock-free lease 분기가 `sync` 밖에서 읽는 값)에는 해당하지 않는다 — 그 읽기는 `sync` 아래다.
6. **`receive_owner` word 와 §3.1 admission turn bit 의 공존**(1차 S-4): 스펙상 상태어가 둘이라는 지적은 그대로다. 스펙 수정 금지 지시에 따라 손대지 않았다.

---

## 8. 변경 파일

| 파일 | 요지 |
|---|---|
| `core/src/runtime/sockets/common/socket_runtime.hpp` | async 해제를 `sync` 아래로, epoch/waiters atomic 화, 비재귀 handoff lock/CV, 단일 progress 발행자, 프로토콜·비용 주석 정정 |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp` | command turn 이 단일 발행자를 사용, 대기 후 `sync` 재획득 구조, epoch/waiters atomic 접근, 종료 주석 정정 |
| `core/src/runtime/sockets/common/socket_base_msg.cpp` | epoch snapshot acquire load ×2 |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | ST-2 상태 그대로(이번 job 변경 없음) |
| `core/tests/integration/test_stream_concurrent_pull_send.cpp` | io_context 소유권 역전 제거, 소유 thread 취소 모델, 재현 파라미터 유지 |
| `core/tests/CMakeLists.txt` | ST-2 상태 그대로 |

**변경 분류: B(기존 결함 수정).** 공개 계약·옵션·플래그를 늘리지 않았고, 규칙 수는 progress 발행 구현 2 → 1, 배타 방식 2(lease / mutex) 유지, 상태값 5 유지다.
