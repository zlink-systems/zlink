# S-2 적대적 리뷰 — 비재귀 전환 15개 잠금의 재진입 독립 검증

> 대상: `~/project/zlink-work/s2` 미커밋 diff (detached `8b65c9b42c`) · 보고서 `core-rf-S-2-summary.md`
> 방식: 읽기 전용. 빌드·벤치 없음. `_out_sync` 67개 임계구역은 스크립트로 스코프를 파싱해
> (1) 같은 객체의 잠금 함수 호출, (2) 임계구역 밖으로 나가는 모든 호출 대상을 기계적으로 추출한 뒤
> pipe.cpp 내부 호출 그래프의 전이 폐포를 다시 계산해 후보를 직접 읽어 판정했다.

## 1. 판정표

| 잠금 | 판정 | 근거 file:line |
|---|---|---|
| `pipe_t::_out_sync` (67 지점) | **SAFE** | 스코프 스캔 결과 임계구역 안의 자기 잠금 함수 호출 0건. 전이 후보 3건 — `process_pipe_term_ack`→`detach_peer_link`(`pipe.cpp:2989`), `write_message_observed`→`terminate`(`:2704`), `write_owner_started_message_observed`→`terminate`(`:1731`) — 은 모두 잠금 블록이 닫힌 **뒤**(각각 `:2976-2987`, `:2686-2699`, `:1712-1726`)에 있다 |
| `pipe_transport_lifetime_t::transport_sync` | **SAFE (주의 조건부)** | 획득 10지점 전수 확인. 단, 이 잠금은 **pair의 두 pipe가 공유**한다(`pipe.cpp` `pipepair()`가 `make_shared<transport_lifetime_t>` 1개를 pipes[0]/[1]에 함께 전달). 아래 §2-(1) 참조 |
| `stream_t::route_shard_t::sync` (64샤드×11지점) | **SAFE** | 11개 임계구역(`stream.cpp:179,213,263,298,799,844,949,1008,1110,1133,1150`)이 부르는 것은 `pipe_t::terminate`·`write_single_message_and_flush_no_recursive_hwm_check`·`check_write_admission`·`is_lifecycle_active`·`retain_lifetime_ref`·`stream_exact_target_identity`뿐. `pipe_t::terminate`(`pipe.cpp:3076-3138`)는 명령 송신만 하고 sink 콜백이 없다. `close_stream_route`(transport_sync 획득)는 샤드 잠금 **밖**(`stream.cpp:241`). 한 임계구역이 두 번째 샤드를 잡는 경로 없음 |
| `mailbox_t::_sync` | **SAFE** | `mailbox.cpp` 전 지점 lock/unlock 평면 쌍. 밖으로 나가는 호출은 `schedule_if_needed_unlocked`(`:330-344`)의 `pre_post` 하나뿐이고 그 실체는 `socket_base_t::{reaper,async}_mailbox_pre_post`(`socket_base_lifecycle.cpp:309,325`) = `inc_mailbox_ref()`(원자)와 `io_thread.cpp:16`/`reaper.cpp:19`의 NULL. `boost::asio::post`는 인라인 실행이 없다. 시그널러 wake는 명령 처리를 부르지 않으며, **인라인 명령 디스패치 경로(`object_t::send_pipe_command` 자기 디스패치)는 메일박스를 아예 통과하지 않는다** — `_sync`를 다시 잡는 경로 0건 |
| ↳ `_command_wait_cv.wait (&_sync, …)` (`mailbox.cpp:276`) | **SAFE (정정 확인)** | pthread CV 구현이 `mutex_->get_mutex()`를 그대로 넘긴다(`condition_variable.hpp:217`). POSIX는 normal·errorcheck mutex에 대해 `pthread_cond_wait`를 정의하고 recursive에 대해서만 미정의로 둔다. 전환 전이 UB, 전환 후가 정상 — 보고서 §8 주장 그대로 |
| `dbuffer_t::_sync` | **SAFE** | `dbuffer.hpp:85-101,111,128,135,142,155,177` 전부 leaf. 콜백(`fn_`, `accounted_bytes_`, `counted_message_`)은 msg 술어뿐 |
| `random_init_sync` / `random_sync` | **SAFE** | leaf (`random.cpp:24,89`) |
| `compatible_get_tick_count64_mutex` | **SAFE** | leaf, Windows 전용 (`clock.cpp:52,60`) |
| `condition_variable_t::_listenersMutex` | **SAFE** | leaf, VxWorks 전용 (`condition_variable.hpp:184`) |
| `atomic_counter_t::sync` | **SAFE** | 단일 교환 leaf. `ZLINK_ATOMIC_COUNTER_MUTEX`에서만 컴파일 — 이 플랫폼 미컴파일 (`atomic_counter.hpp:189`) |
| `atomic_ptr_t::_sync` / `atomic_value_t::_sync` | **SAFE** | 동일. CAS leaf, 이 플랫폼 미컴파일 (`atomic_ptr.hpp:198,252`) |

**RISK 0건, UNSURE 0건.** (전수 확인용으로 `core/src`의 남은 `mutex_t` 선언을 다시 grep해
표 밖의 누락 전환이 없음을 확인했다. 다만 실제로 구분되는 잠금 **객체**는 12개이고 보고서의 "15개"는
집계 방식이 다르다 — 문서상 수치 불일치일 뿐 안전성과 무관.)

## 2. 보고서가 다루지 않았지만 판정에 결정적인 두 경로 (블로킹 아님, 문서화 권고)

**(1) `transport_sync`는 pair 공유 잠금이다.**
`pipepair()`가 `transport_lifetime_t` 하나를 두 pipe에 함께 넘기므로 A와 B의 `transport_sync()`는
**같은 mutex**다. 즉 프롬프트의 "A가 자기 잠금을 다시 잡는가" 시나리오가 실제로 성립할 수 있는 유일한 잠금이다.
현재는 안전하다:

- `pipe_t::process_peer_weight`(`pipe.cpp:1981`)가 이 잠금을 쥔 채 가상 `_sink->peer_weight_received`(`:1999`)
  → `socket_base_t::accept_peer_weight`(`socket_base_dispatch.cpp:440-468`) → 가상 `apply_peer_weight`로 나간다.
- 구현체는 셋뿐이고 셋 다 `transport_sync`를 잡지 않는다:
  base는 no-op(`socket_base_dispatch.cpp:475-480`), `router_t`(`router_admission.cpp:452-479`)와
  `dealer_t`(`dealer.cpp:478-495`)는 `_out_pipes_sync`/`_lb` → `notify_send_writable` → `emit_peer_weight_changed`까지만 간다.
  `notify_send_writable`(`socket_send_complete.cpp:136-167`)이 부르는
  `xsend_writable_target_for_pipe`/`_ready`도 `transport_sync`를 잡지 않는다.
- router의 `apply_recorded_peer_weight`(`socket_base_dispatch.cpp:482-490`, 역시 `transport_sync` 획득)는
  `router_recv_path.cpp:138,197`에서 블록을 **닫은 뒤** 호출된다 — 보고서 서술과 일치.
- `stream_t::identify_peer`(`stream.cpp:1235`)는 이 잠금을 쥔 채 `peer->set_router_socket_routing_id`(`:1260`)로
  **상대 pipe의 `_out_sync`**를 잡는다. 잠금 방향은 항상 `transport_sync → _out_sync`로 일관되어 역전이 없다.

결론적으로 SAFE지만, 재귀였을 때는 무해했던 "peer 쪽 `transport_sync` 재획득"이 이제는 **즉시 교착**이다.
`transport_lifetime_t::transport_sync` 선언부(`pipe.hpp:88-90`)에 "pair 공유·재진입 금지"를 한 줄 남길 것을 권한다.

**(2) 인라인 자기 디스패치 경로가 존재한다 — 보고서에 언급이 없다.**
`object_t::send_pipe_command(dest, cmd, allow_self_dispatch_)`는 `dest->get_tid() == _tid`이면
`dest->process_command(cmd)`를 **동기 인라인 호출**한다(`object.cpp:682-698`). 이것이 이 job에서
재진입을 만들 수 있었던 유일한 메커니즘이다. 확인 결과:

- `allow_self_dispatch_=true`인 유일한 명령은 `send_activate_write`(`object.cpp:382`).
- 그 유일한 호출처는 `pipe_t::account_inbound_frame`(`pipe.cpp:3945`)이고, 이 함수는 **`_out_sync`를 쥐고 있지 않다**(수신 경로).
- 목적지는 항상 `peer`(다른 객체)이므로 같은 객체 재진입이 아니다.
- `_out_sync` 안에서 나가는 나머지 명령 — `send_pipe_term`(`:3096,3115`)·`send_pipe_term_ack`(`:2904,2923,3071,3105,3191`)·
  `send_peer_weight`/`send_flow_state`(`:2362,2369`)·`send_activate_read`(`:4109`) — 은 전부 `allow_self_dispatch_=false`이며
  `send_flow_state`·`send_peer_weight`에는 그 이유가 주석으로 남아 있다(`object.cpp:396-421`).

이 두 사실이 §1의 `_out_sync`/`transport_sync` SAFE 판정을 실제로 떠받치는 근거다.

## 3. `_out_sync` 임계구역이 밖으로 나가는 호출 전량 (검증 결과)

| 호출 | 판정 |
|---|---|
| write observer `commit` (`pipe.cpp:1720,2693`) | 코어 전체에 구현 1개뿐(`socket_request_reply_submit_api.cpp:73`). commit 단계는 `get_transport_pair_id`(`pipe.cpp:3519`)·`get_transport_pair_generation`(`:3524`)·`request_correlation_lease_t::adopt`(`socket_request_reply_internal.cpp:159-167`)만 부르고 셋 다 잠금이 없다. `_out_sync`를 잡는 `prepare`(`try_reserve_request_correlation`)와 `finish`(`release_request_correlation`)는 블록 **밖**(`:1686`/`:1728`, `:2660`/`:2701`) |
| `send_pipe_term`/`_ack`/`peer_weight`/`flow_state`/`activate_read` | 메일박스 경유(§2-(2)). `ctx_t::send_command`(`ctx.cpp:323-328`)도 인라인 실행 없음 |
| `_physical_queue_registry.*` | `recursive_mutex_t` 유지분 |
| `peer->release_lifetime_ref()` (`:856,2379`) | 최후 참조여도 `~pipe_t`(`pipe.cpp`)는 `_out_sync`를 잡지 않는다 |
| `_out_pipe->write/read/flush`, `_in_pipe->read_if(&consume_if_delimiter)` | ypipe(락프리) + 순수 술어 |
| `pipe_debug_log` | `fprintf`/`fflush`만 |
| `_sink->*` | **0건** — `process_pipe_term`은 `_sink->pipe_peer_terminated`를 블록 밖으로 뺐고(`pipe.cpp:2964-2967`) 그 이유를 주석으로 남겼다. 코드베이스가 이미 `_out_sync` 재진입을 위험으로 취급하고 있었다는 증거 |
| `detach_peer_link` | 양쪽 잠금을 **동시에** 잡지 않는다(`pipe.cpp:410-415`에서 해제 후 `:427-435`에서 peer 획득). 주석도 명시 |

## 4. 프롬프트 개별 점검 항목

- **(a) inproc 양단 동일 스레드**: pair 공유 잠금은 `transport_sync` 하나뿐이며 §2-(1)에서 재획득 부재를 확인.
  `_out_sync`·shard sync는 pipe별/샤드별로 다른 객체다. A가 B를 부르며 B의 잠금을 잡는 경로
  (`pipe.cpp:431,3448`, `stream.cpp:1260`)는 모두 A의 잠금을 먼저 놓거나 다른 mutex여서 자기 교착이 아니다.
- **(b) ERRORCHECK 초기화 컴파일**: `enum kind_t { plain_kind = PTHREAD_MUTEX_ERRORCHECK, … }`
  (`mutex.hpp:138-146`)의 glibc 노출 조건은 기존에 쓰던 `PTHREAD_MUTEX_RECURSIVE`와 동일하므로
  C++11 + 프로젝트 기본 플래그에서 새 문제가 없다. `-Wall -Wextra`(+선택적 `-pedantic`)이고
  `-Werror`는 `LIBZLINK_WERROR` 기본 OFF(`core/CMakeLists.txt:576-599`), `-Wnon-virtual-dtor`는 없다 —
  `recursive_mutex_t : public mutex_t`의 비가상 소멸자는 경고를 만들지 않고, 코드 어디에서도 `mutex_t*`를 delete하지 않는다.
  Windows/VxWorks 분기 가드(`mutex.hpp:180-189`)는 상단 `#if/#elif` 선택과 정확히 대응한다.
  잔여 사항: `ZLINK_MUTEX_ERRORCHECK`가 TU별 `NDEBUG`에 걸려 있어 서로 다른 `NDEBUG`로 컴파일된
  TU가 섞이면 `mutex_t` 생성자 본문이 달라진다(ODR). 단일 CMake 빌드 타입에서는 균일하므로 실무상 무해.
- **(c) 공개 헤더/vers**: `git diff --stat -- core/include core/src/libzlink.vers` → **빈 출력**. 변경 없음 확인.
- **(d) `fast_mutex` 잔존 참조**: 저장소 전체(bindings·tests·docs·scripts 포함, 워크로그 제외)
  `fast_mutex|fast_mutex_t|scoped_fast_lock_t` grep → **0건**.

## 5. 이 변경이 도입하지 않았지만 함께 기록해 둘 사항 (블로킹 아님)

1. `ctx_t::_slot_sync`는 `recursive_mutex_t`로 남았는데 `ctx_socket_registry_t::wait_for_socket_removal`/
   `wait_for_socket_count_at_most`(`ctx_socket_registry.cpp:111,142`)가 이 잠금으로 `pthread_cond_wait`를 부른다.
   보고서 §8이 "결함"이라고 지목한 recursive+CV 조합이 **여기에는 그대로 남아 있다**. 메일박스만 정정됐다.
2. `mailbox_t::send()`가 `_sync`를 쥔 채 `boost::asio::post`를 부른다(`mailbox.cpp:79`). 예외(`bad_alloc`)가
   빠져나가면 뮤텍스가 잠긴 채 유실된다. 기존 결함이며 재귀/비재귀와 무관.
3. `dbuffer_t::write()`의 `_sync.try_lock()`(`dbuffer.hpp:85`) 의미가 바뀐다: 재귀였을 때는 같은 스레드가
   이미 쥐고 있어도 성공했고 지금은 EBUSY다. 그 상황 자체가 원래 오동작이므로 실제 영향은 없다고 판단한다.

## 6. 결론

전환한 12(보고서 집계 15)개 전부 **SAFE**. 차단 사유 없음.
게이트 진행에 동의하되, 병합 전 **코드 주석 2줄**만 추가할 것을 권한다(동작 변경 없음):
`pipe.hpp:88-90`의 `transport_sync`에 "pair 두 endpoint 공유 · 재진입 금지 · 임계구역에서 sink 가상 호출 주의",
`object.cpp:682` `send_pipe_command`에 "allow_self_dispatch_=true는 동기 인라인 실행 — 호출자가 잠금을 쥐고 있으면 안 됨".
