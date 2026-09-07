# G-1 — 공개 send/recv 경로의 mutex 트래픽 축소

> 2026-09-07. worktree `~/project/zlink-work/g1` (detached `2d56078977`). **커밋하지 않음.**
> 원본 데이터: `<scratchpad>/G1/` (`cg_before.out`, `cg_after.out`, `callers.py`, `lines.py`, `stream.sh`, `build2.log`).
> 검증 4항목(TSan delta, lost-wake until-fail:10, 57-suite 10회, with_stream)은 §7에 있다.

## 0. 측정 방법 — G-A 방법 + 라인 분해

G-A의 release(LTO) 덤프는 디버그 정보가 없어 `pthread_mutex_lock` 호출자가
`process_commands` 한 덩어리(2.009/msg)로만 보이고 그 안의 어느 잠금인지 구분되지 않았다.
그래서 같은 축소 셀(STREAM zlink, CCU 20 / 1024 B / 10 s, `--io-threads 4`, `--cache-sim=no`)을
**dev 트리(RelWithDebInfo, LTO OFF, `-g`) lib**로 다시 떴다. 벤치 바이너리는 G-A 워크트리의
`with_stream` 실행파일을 `LD_LIBRARY_PATH`만 바꿔 재사용했다(ABI 동일). 잠금 **횟수**는 인라이닝에
영향받지 않으므로 dev 덤프가 release 덤프보다 정확한 계측기다 — 실제로 dev before가 **17.396/msg**,
G-A release가 **17.335/msg**로 일치한다.

## 1. 결과 (수치)

| | before(`2d56078977`) | after | 변화 |
|---|---:|---:|---|
| `pthread_mutex_lock` 호출/msg | **17.396** | **15.122** | **−2.27** |
| 그중 이번 변경에 직접 귀속되는 감소 | — | — | **−1.503** |
| Ir/msg (idle 4.1 M 가정 보정) | ≈11,488 | ≈11,041 | ≈−3.9 % |
| 셀 메시지 수 | 16,886 | 53,311 | (부하 차이 — §7) |

**−1.503**은 아래 표의 두 행이 after 덤프에서 **완전히 사라진 값**이다(1.004 + 0.499).
나머지 −0.77은 before 판이 load avg 10.2, after 판이 6.5에서 돌아 셀당 고정비가
메시지 수에 다르게 분모화된 결과이므로 **이번 변경의 이득으로 주장하지 않는다**.
zmq는 같은 셀에서 1.73/msg다.

## 2. 메시지당 잠금 표 (dev 축소 셀, caller × 소스파일 분해)

| 잠금(획득 지점) | before /msg | after /msg | 지키는 불변식 | 조치 |
|---|---:|---:|---|---|
| `mailbox_t::send` — mailbox `_sync` | 2.013 | 2.003 | 명령 큐(cpipe)와 signaler 상태 | 유지 — 메시지당 2회 핸드오프는 구조적(S-1 §2(a)에서 이득 0으로 증명됨) |
| `socket_base_t::process_commands` — `public_api_sync`+`command_owner_sync`+명령당 `receive.sync` | 1.558 | 1.467 | 명령 배치의 단독 소유권, 명령 적용과 공개 API의 배타 | 유지 — §5 대안 (a) 기각 |
| `asio_poller_t::loop` — boost.asio scheduler mutex | 1.155 | 1.217 | asio 내부 | Core 밖 |
| **`process_deferred_socket_msg_pipe_terminations` — `deferred_socket_msg_termination_sync`** | **1.004** | **0** | 지연 종료 intrusive 큐의 head/tail/next 링크 | **제거**: head를 `std::atomic<pipe_t*>`로 두고 "빈 큐인가"만 잠금 밖 원자 로드로 답한다. 큐 변형은 전부 그대로 잠금 아래 |
| `pipe_t::flush` — `_out_sync` | 1.002 | 1.001 | 송신측 상태 클러스터(`_out_pipe`,`_state`,`_out_active`,`_peers_msgs_read`) | 유지 — §5 대안 (b) 기각 |
| `pipe_t::write` — `_out_sync` | 1.000 | 1.000 | 위와 같음 | 유지 |
| `pipe_t::write_single_message_and_flush_...` — `_out_sync` | 1.000 | 1.000 | 위와 같음 | 유지 |
| `stream_t::xsend_routed` — `route_shard_t::sync` | 1.000 | 1.000 | 라우트 샤드의 RID→pipe 표 | 유지 |
| `socket_base_t::read_activated` — 수신 파티션 잠금 | 0.999 | 0.999 | fq/route 활성 파티션 (POLLIN level의 근거) | 유지 — 계약 직결 |
| **`ctx_physical_queue_registry_t::refresh_application_hwm_if_drained` — ctx 전역 `_sync`** | **0.499** | **0** | 등록 표(`_directions`)의 존재·동일성·lane | **제거**: 이 함수의 **유일한 효과**는 `planned_hwm`을 `applied_hwm`에 싣는 것이므로, 둘이 같으면 등록 답이 무엇이든 할 일이 없다. 그 동치를 호출자가 이미 소유한 handle의 원자 둘로 잠금 **전에** 판정 |
| `socket_base_t::has_in` | 0.340 | 0.278 | 수신 파티션 | 유지 |
| `mailbox_t::recv` | 0.340 | 0.278 | mailbox `_sync` | 유지 (이미 lock-free 빠른 경로 있음) |
| `mailbox_t::reschedule_if_needed` / `signal_pollers` | 0.240 / 0.215 | 0.227 / 0.188 | mailbox `_sync` | 유지 |
| boost.asio `start_op`/`perform_io`/`task_cleanup`/`poll`/`post_immediate_completion` | 3.229 | 2.816 | asio 내부 | Core 밖 |
| `zlink_poller_wait` / `poller_acquire` — 공개 poller `std::mutex` | 0.513 / 0.171 | 0.420 / 0.140 | poller 핸들 표 | 유지 |
| 벤치 바이너리 자체 | 1.000 | 1.000 | — | 하네스 |

## 3. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/sockets/common/socket_runtime.hpp` | `deferred_socket_msg_termination_head`를 `std::atomic<pipe_t *>`로. 새 멤버·플래그 없음(타입만 바뀜, S-11의 `_in_active`·`_state`와 같은 방식) |
| `core/src/runtime/sockets/common/socket_base_dispatch.cpp` | enqueue는 release store, drain은 루프 진입 전 acquire 로드로 빈 큐를 판정하고 그때만 반환. 큐 pop은 그대로 잠금 아래 |
| `core/src/runtime/core/ctx_physical_queue_registry.cpp` | `refresh_application_hwm_if_drained`의 `planned == applied` 조기 반환을 `_sync` 밖으로 |

공개 헤더·`libzlink.vers`·계약 테스트 기대값 변경 **0**(`git diff --stat`이 위 3파일뿐).
브리프가 피하라고 한 영역(R3의 `pipepair` 옵션 구간, R4의 `socket_base.hpp`/monitor/msg)은 건드리지 않았다.

## 4. 두 변경의 안전성 논증

**(1) 지연 종료 큐.** 생산자와 소비자가 **같은 스레드**다: enqueue는
`socket_base_api.cpp:1763`의 `pipe_terminated` 명령 처리 안에서만 일어나고
(PAIR/DEALER/ROUTER/SUB/XSUB, `completion`이 아닐 때), 그 명령을 적용한 바로 그 명령 소유자가
같은 배치 끝에서 `process_deferred_...`를 부른다. 따라서 자기 store를 자기 load가 반드시 본다.
다른 스레드의 enqueue는 **잠금 아래 읽었어도 마찬가지로 놓쳤을** 시점의 것이다(잠금은 큐의 원자성을
지킬 뿐 "그 순간까지의 모든 enqueue"를 약속하지 않는다) — 즉 드레인 경계는 그대로다.
누락된 항목은 다음 드레인이 집는다: `process_deferred_...`는 명령 배치마다,
그리고 async 실행기 종료 경로(`socket_base_lifecycle.cpp:1506`)에서도 호출된다.
STREAM은 위 소켓 타입 목록에 없어 이 큐가 **항상 비어 있는데도** 명령마다 ctx 잠금 하나를 물었다.

**(2) auto-HWM refresh.** 함수의 관측 가능한 효과는 `applied_hwm.store(planned)` 하나뿐이다.
`planned == applied`이면 잠금을 잡고 등록을 확인해도 그 store에 도달할 수 없다(잠금 안에서 다시
같은 두 값을 읽어 `planned == applied`면 return). 따라서 조기 반환은 **같은 함수의 기존 분기를
앞으로 옮긴 것**이며, 두 값은 이미 원자이고 handle 수명은 호출자의 shared_ptr가 보장한다.
등록 표가 이 사이에 바뀌어도 결과는 같다: 새 plan은 `planned_hwm`을 올리므로 다음 호출이 집는다.

## 5. 설계 비교와 선택 이유

- **(a) `process_commands`의 `receive.sync`를 명령 루프 밖으로 올리기 — 기각.**
  잠금은 명령당 1회이고 이 셀의 소켓 드레인은 배치당 명령 ~1개(`process_commands` 0.555회/msg,
  `receive.sync` ~1.0회/msg)라 이득이 **최대 −0.45/msg**로 작다. 반면 (i) 배치 사이에 일부러
  잠금 밖으로 뺀 `process_deferred_socket_msg_pipe_terminations`(주석에 이유가 명시돼 있다)를
  다시 잠금 안으로 끌어들이게 되고, (ii) `ZLINK_BUILD_TESTS` 아래의
  `receive.sync.try_lock()` 경합 프로브가 항상 "경합"으로 뒤집힌다. 이득 대비 계약·테스트
  위험이 커서 채택하지 않았다.
- **(b) `pipe_t::write` + `session->flush()`를 하나의 `_out_sync`로 접기 — 기각.**
  엔진은 `process_input()`에서 여러 프레임을 write한 뒤 배치 끝에 한 번 flush한다.
  둘을 접으면 프레임마다 flush가 되어 ypipe의 sleep/awake 전이 횟수, 즉 **activate_read 발신
  조건이 바뀐다**. 05-polling의 wake 진리표를 건드리는 변경이므로 하지 않았다.
- **채택한 두 건의 공통 형태**: "새 상태를 추가"하지 않고 **이미 있는 질문을 이미 있는 원자로 먼저
  답한다". POSDDD의 "규칙 수 줄이기"에 맞고, 두 경우 모두 잠금이 지키던 불변식은 그대로 잠금 아래 남는다.

## 6. 재확인한 스펙 절 — 어느 문장도 다른 동작이 되지 않았다

- 05-polling: "`ZLINK_POLLIN` level은 소켓이 프레임을 가지고 있는 동안 유지된다" — `read_activated`·
  `has_in`·`xhas_in` 경로와 그 잠금은 손대지 않았다.
- 04-socket §4.1(공개 API 직렬화): `public_api_sync`/`command_owner_sync`/`receive.sync`의
  획득 순서와 범위는 한 글자도 바뀌지 않았다.
- 08-stream §5: STREAM은 지연 종료 큐를 쓰지 않는 소켓 타입이며(위 목록에 없음),
  READY/DISCONNECTED·completion 순서 경로에 접근하지 않았다.
- auto-HWM: `applied_hwm`은 여전히 `planned != applied`이고 `current <= planned`일 때만 갱신된다.

## 7. 검증 (감독관 요청 4항목 포함)

### (1) TSan — **신규 경고 0 (delta 0)**

`~/project/zlink-work/s2/core/build-tsan`과 같은 구성으로 별도 트리를 만들었다
(`ENABLE_TSAN=OFF` + 수동 `-fsanitize=thread -fno-omit-frame-pointer`, `ENABLE_LTO=OFF`,
`RelWithDebInfo`, `BUILD_TESTS=ON`). 실행: `setarch $(uname -m) -R ctest --test-dir build-tsan -R wake`.
**before(패치 제외한 clean `2d56078977`)와 after(패치 적용)를 각각 빌드해 같은 5개 테스트를 돌렸다.**

| | before | after |
|---|---:|---:|
| `WARNING: ThreadSanitizer` 총 건수 | **12** | **12** |
| `ypipe.hpp:104` | 10 | 10 |
| `mailbox.cpp:120` / `:208` | 10 / 10 | 10 / 10 |
| `socket_base_msg.cpp:68` / `:997` | 2 / 2 | 2 / 2 |
| `socket_request_reply_runtime_io.cpp:1054` | 2 | 2 |
| `socket_base_dispatch.cpp` / `ctx_physical_queue_registry.cpp` / `deferred_socket_msg_*` 언급 | **0** | **0** |

**delta 0**. 경고 집합·건수·위치가 완전히 동일하며, 이번 변경이 만든 원자 접근
(`deferred_socket_msg_termination_head`)은 어느 판에서도 보고되지 않았다.
남은 12건은 mailbox/ypipe/명령 경로의 **기존** 경고로 이 job의 범위가 아니다.
로그: `<scratchpad>/G1/tsan-before.log`, `tsan-after.log`, `tsan-delta.txt`.

### (2) lost-wake 세트 `--repeat until-fail:10` — **통과**

`ctest -R 'wake' --repeat until-fail:10` (5 tests × 10회) → exit 0, 실패 0. 총 323.8 s.

### (3) 미규명 실패 — **10회 연속 clean, 재현 안 됨**

`ctest -R 'wake|poll|stream|pipe|mailbox|send|recv|router|dealer|pair' --repeat until-fail:10`
(57 tests × 10회, `--output-on-failure`를 파일로 저장) → **exit 0, 10회 전부 100 % pass**.
1차 보고에서 8회 중 1회 나온 실패는 재현되지 않았고 이름을 잡지 못했다.
그때 load avg가 6~10(다른 job 빌드 동시 진행)이었고 이번 10회는 load avg 2~3에서 돌았다.
**타이밍 flake로 판단하되 케이스 이름은 미확인으로 남긴다.**

### (4) with_stream (Release+LTO lib, CCU 1000, runs 1, flock, 측정 시작 load avg 0.53)

`--stack zlink,asio --size all --ccu 1000 --runs 1`, 결과
`bindings/c/bench/with_stream/results/20260907_110844`.

| size | zlink kops | asio kops | 비(zlink/asio) | Phase 0 기준 zlink | S-11 판 비 |
|---|---:|---:|---:|---|---:|
| 64 B | **284.15** | 357.56 | 0.795 | 268.9 대비 **+5.7 %** | 0.799 |
| 1024 B | **272.33** | 323.38 | **0.842** | 243.0 대비 **+12.1 %** | 0.820 |
| 65536 B | **25.80** | 31.81 | 0.811 | 30.4 대비 −15.1 % | 0.821 |

mismatch 전 셀 0. 64 KiB 절대값이 기준보다 낮지만 **같은 판의 asio도 39.2 → 31.81로 함께
−19 % 내려갔다** — 셀 전체가 눌린 것이고 zlink/asio 비 0.811은 S-11의 0.821과 같은 수준이다.
회귀로 읽지 않는다. 1024 B의 비 0.842는 이 캠페인에서 기록된 값 중 가장 좋다.

### 그 밖에

- 빌드 경고 1건(`__atomic_store_8 … region of size 0`)은 추적 결과
  `core/tests/unittest/contract_zmp_engine_fixture.hpp:55`의 테스트 픽스처에서 나오는 **기존 경고**로,
  이번 변경과 무관하다(`<scratchpad>/G1/build2.log`).

## 8. 사고 보고 — 공유 stash 오염 (복구 완료)

TSan before 판을 만들 때 `git stash push`로 패치를 잠시 내렸는데, **stash 스택은 저장소 전체가
공유**한다. 그 사이 G-2가 자기 stash를 올렸고, 내 `git stash pop`이 **G-2의 stash
(`msg.cpp`/`msg.hpp`/`pipe.cpp`/`unittest_pipe_byte_charge.cpp`)를 내 워크트리에 꺼내고 스택에서 지웠다.**
즉시 복구했다:

1. 떨어진 stash 커밋 `5e56ede765`를 `git stash store`로 되돌려 스택에 재등록(내용 동일, 메시지에 경위 주석).
2. 내 워크트리의 G-2 파일을 `git checkout -- core/src core/tests`로 되돌림.
3. 내 patch는 `git stash apply c8f0bd51da` 후 그 stash만 삭제.

현재 `git diff HEAD --stat`은 §3의 3개 파일뿐이고, `git stash list`의 `stash@{0}`이 G-2의 것이다.
**이후 `git stash`는 사용하지 않았다.**

## 9. 왜 zmq는 1.7회/msg로 되는가

zmq의 메시지당 잠금 1.73회는 사실상 **mailbox `send` 하나**다. libzmq는 파이프의 양 끝을 각각
**정확히 한 스레드에 고정**한다: `zmq::pipe_t`의 송신측은 그 소켓을 쓰는 스레드가, 수신측은 I/O
스레드가 단독 소유하고, 둘 사이의 자료구조는 lock-free `ypipe_t`(CAS 하나)와 원자 카운터뿐이라
`write`/`flush`/`read`에 뮤텍스가 없다. 소켓 상태도 "명령을 받은 스레드만 바꾼다"는 규칙 하나로
배타를 얻으므로 `receive`/`command_owner`/`public_api` 같은 소켓 레벨 잠금이 아예 없고,
reactor(`zmq::poller`)는 자체 구현이라 연산마다 뮤텍스를 잡지 않는다. 남는 유일한 공유 지점이
스레드 간 명령 전달용 `mailbox_t::_sync`이고 그게 1.7회다. zlink는 반대로 **같은 소켓을 여러 앱
스레드가 동시에 쓸 수 있다는 공개 계약**을 갖고(04-socket §4.1), 그 계약을 pipe의 `_out_sync`,
소켓의 3중 API/명령/수신 잠금, 라우트 샤드 잠금으로 지불한다. 여기에 reactor를 boost.asio로
쓰면서 asio scheduler·reactor 뮤텍스가 연산마다 4회 더 붙고, 공개 poller의 `std::mutex`가 0.7회
더 붙는다. 즉 17.3 대 1.7의 10배는 "같은 일을 비싸게 한다"가 아니라 **다른 계약(멀티스레드 소켓)과
다른 reactor(외부 라이브러리)를 골랐기 때문**이며, 계약을 유지한 채 줄일 수 있는 것은 이번 두 건처럼
**불변식을 지키지 않던 잠금**과, 남은 후보 중 asio reactor 사용 방식뿐이다.

## 10. 변경 분류

**B(기존 결함)** — 두 잠금 모두 "그 잠금이 답할 필요가 없는 질문"에 대해 획득되고 있었다.
계약 적응(A)도, 우회(C)도, spec gap(D)도 아니다.
