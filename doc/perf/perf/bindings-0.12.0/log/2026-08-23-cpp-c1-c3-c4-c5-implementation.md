# C++ binding 개선 후보 C1·C3·C4·C5 구현 기록 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> 후보 출처: `log/2026-08-23-cpp-pair-tcp-profile.md` §5, `progress.ko.md` §5
>
> **단계: 탐색(비공식).** 이 문서의 모든 수치는 **로컬 core 빌드**
> (`core/build/lib/libzlink.so.0.12.0`) 기준이며 판정값이 아니다.
> `core/v0.12.0` release asset이 재릴리스된 뒤 계획서 §7.3 규칙으로 공식 paired
> 측정을 다시 수행해야 최종 판정이 가능하다. 따라서 이 문서의 모든 판정은
> **예비 판정**이다.
>
> **commit 하지 않았다.** 작업 트리 변경만 남긴다.
>
> **C2는 구현하지 않았다** — 종료 경합 시 `ETERM` 관측 시점 동등성이 증명되지
> 않아 조건부 no-go 상태 그대로 둔다.

## 1. 재현 환경

| 항목 | 값 |
|------|-----|
| host / OS | `ulalax-gram` / `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `12th Gen Intel(R) Core(TM) i7-1260P`, 16 logical cores (pin 없음) |
| 브랜치 / base commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742` |
| Core runtime | **local** — `core/build/lib/libzlink.so.0.12.0` |
| 기존 미커밋 변경 | C++ perf SPOT 제거, `--sndbuf`/`--rcvbuf` 배선, 각 언어 runner 스크립트 규약 변경 — **그대로 보존하고 그 위에 얹었다** |
| session tag | `c1345-before*`, `c134-after`, `c1345-after*`, `c1345-cref-*` |
| 측정 조합 | `PAIR` / `tcp` / 64 B / duration 5 / `--runs 3` (배치) × 3 배치 |

## 2. 후보별 변경 요약

### C1. raw send 경로의 per-call `weak_ptr` 왕복 제거 — **구현함(설계 조정)**

변경 파일과 위치:

| 파일 | 위치 | 내용 |
|------|------|------|
| `bindings/cpp/src/Runtime/Messaging/operation_state.hpp` | 77–101 (`raw_command_t`) | `std::weak_ptr<socket_callback_state_t> callbacks`를 **`socket_callback_state_t *callbacks` (비소유 view) + `std::weak_ptr callbacks_anchor` (수명 토큰)** 두 필드로 분리. `reset()`은 두 필드를 더 이상 지우지 않는다(소켓 주소를 키로 하는 캐시로 승격). |
| 같은 파일 | 127–158 | `bind_callback_state()` / `live_callback_state()` / `share_callback_state()` 3개 helper 추가 |
| `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp` | 36–41 | 동기 terminal이 `lock()` 대신 `live_callback_state()` 사용 |
| `bindings/cpp/src/Runtime/Messaging/send_operations.cpp` | 116, 189 | async/admission 시작 경로는 `share_callback_state()`로 **shared_ptr 승격 유지** |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp` | 250 | 동일 |
| `bindings/cpp/src/Runtime/Messaging/reply_operations.cpp` | 21 | 동일(핫 경로 아님, 강한 참조 유지) |
| `bindings/cpp/src/Runtime/Sockets/{pair,dealer,router,pubsub,stream}.cpp` | 각 socket entry factory 9곳 | `state_ptr->raw.callbacks = callback_state ().weak_from_this ();` → `detail::bind_callback_state (state_ptr->raw, callback_state ());` |

핵심 메커니즘:

```cpp
inline void bind_callback_state (operation_state_t::raw_command_t &raw_,
                                 socket_callback_state_t &state_)
{
    if (raw_.callbacks == &state_ && !raw_.callbacks_anchor.expired ())
        return;                                    // 재바인딩 불필요
    raw_.callbacks = &state_;
    raw_.callbacks_anchor = state_.weak_from_this ();
}
```

thread_local pool의 state는 같은 소켓에서 반복 사용되므로, **정상 hot loop에서는
anchor를 다시 만들지 않는다.** 결과적으로 send 1건당 사라지는 lock 접두 RMW는
4회 전부다(weak inc / strong CAS / strong dec / weak dec). 남는 것은
`expired()`와 `socket_closed` 두 번의 **atomic load**뿐이다(RMW 아님).

주소 재사용 안전성: 소켓이 파괴된 뒤 새 `socket_callback_state_t`가 같은 주소에
할당되면 `raw_.callbacks == &state_`가 참이 될 수 있다. 그러나 그 경우 이전
anchor는 반드시 `expired()`이므로(강한 참조가 0이 되어야 메모리가 해제된다)
재바인딩 조건이 성립한다. 이 경로는 §4의 전용 검증 프로그램 1번 항목으로 확인했다.

**설계 조정(중요)**: 후보 문서의 "weak_ptr 제거" 문구를 문자 그대로 적용하면
`raw.callbacks`를 순수 raw pointer로 바꾸게 되는데, 그러면
`bindings/cpp/tests/contract/test_cpp_contract_request_reply.cpp:618`
`test_routed_async_builder_does_not_outlive_socket_anchor()`가 요구하는
"소켓이 파괴된 뒤 builder terminal을 호출하면 `invalid_state`/`EINVAL`"
계약이 dangling 역참조가 된다. 따라서 **수명 토큰(weak_ptr) 자체는 유지하고,
그 토큰의 비용을 호출당(per-call)에서 소켓당(per-socket)으로 옮기는 형태**로
구현했다. 후보의 목표(호출당 수명 추적 비용 제거)는 달성하되 계약은 유지한다.

**contract 보존 논거**:
- public signature·type·enum 값 변경 없음. 변경은 전부 `src/Runtime/**` 내부
  구조체와 내부 helper다.
- 동기 terminal의 오류 동작 불변: 소켓이 죽었으면 `live_callback_state()`가
  `nullptr`을 돌려주고 기존과 같은 `invalid_state`/`EINVAL`을 던진다. 소켓이
  살아 있고 닫혔으면 기존과 같이 `socket_closed` 검사에서 걸린다.
- `outbound_record_attempt_mutex` gate는 **그대로 유지**했다(C2 미구현).
  따라서 동시 `close()` 대 send 경합 처리는 이전과 동일하다 —
  `socket_t::close()`가 mutex 안에서 `socket_closed`를 세우고 native close를
  하므로, mutex를 잡은 send는 여전히 native close와 직렬화된다.
- ownership 불변: callback state의 소유는 계속 `socket_t::_callbacks`
  (`shared_ptr`)다. async/admission record는 예전처럼 강한 참조를 승격해 잡는다.

**POSDDD**: callback state의 lifetime 소유자는 `socket_t`다. 호출문을 넘지 않는
operation state가 매 호출 소유권 추적을 재지불하던 중복 책임을 없애고, state에는
"주인이 아직 살아 있는가"를 묻는 토큰 하나만 남겼다.

**잔여 위험(감독자 확인 요청)**: 다른 스레드가 send 진행 중에 `socket_t`를
**파괴**하면(=`close()`가 아니라 소멸자) 동기 경로가 raw pointer를 통해
해제된 mutex를 만질 수 있다. 이전 코드의 `lock()`은 그 창을 막아줬다. 다만 그
시나리오는 이미 native handle(`raw.socket`)이 dangling이 되므로 지원 가능한
계약이 아니라고 판단했다(임의 C++ 객체를 사용 중에 파괴하는 것과 동치).
동시 `close()`(파괴 아님)는 위에 적은 대로 여전히 안전하다.

### C3. 동기 raw send terminal의 pooled state 기계장치 축소 — **부분 구현 / 일부 no-go**

구현한 부분:

| 파일 | 위치 | 내용 |
|------|------|------|
| `bindings/cpp/src/Runtime/Messaging/operation_state.hpp` | 261–274 (`reset_for_reuse`) | **`RAW_SEND_HOT_PATH` 특수 분기 삭제.** 이제 모든 operation kind가 같은 reset을 탄다. |
| 같은 파일 | 53–60 (`routing_target_t::reset`) | `first_rid_native_cache = zlink_routing_id_t{}` (**256 B memset**) 제거. 캐시는 `has_first_rid_native_cache`가 참일 때만 읽히므로 플래그만 내리면 충분하다. |
| 같은 파일 | 92–100 (`raw_command_t::reset`) | `callbacks`/`callbacks_anchor`를 지우지 않는다(C1의 소켓 키 캐시) |

특수 분기가 존재했던 이유는 일반 reset이 (a) 256 B memset과 (b) weak count
감소를 포함했기 때문이다. C1이 (b)를, 위 memset 제거가 (a)를 없앴으므로 분기는
설계상 불필요해졌다 — hot path의 불필요한 특수 경우가 실제로 사라졌다.

**no-go로 남긴 부분과 이유**: "동기 terminal용 축소 state를 스택에 두기"는
구현하지 않았다. 공개 builder 타입
(`send_operation_t`, `send_submit_operation_t`, `routed_send_*`,
`request_*`, `reply_*`)이 모두
`detail::operation_builder_base_t<operation_state_t, pooled_operation_state_policy_t>`를
private 상속하고, 그 base가 `std::unique_ptr<operation_state_t>`를 **멤버로**
보유한다(`include/zlink/Contracts/Messaging/operation_builder_base.hpp:60`).
state를 스택/inline으로 옮기려면 이 공개 클래스들의 멤버 구성을 바꿔야 하는데,
계획서 §5의 "공개 type 변경 금지"에 저촉된다. 또한 builder 체인은
`send()` → `.message()`에서 **타입이 바뀌므로**(`send_operation_t` →
`send_submit_operation_t`) state를 값으로 옮기면 `optional<message_t>` 등을
포함한 대형 구조체를 단계마다 move 하게 되어 오히려 손해다.
공개 타입 변경 없이 이 부분을 하려면 builder 체인 자체의 재설계가 필요하고,
그것은 public interface 변경이므로 이번 범위 밖이다.

**contract 보존 논거**: `reset_for_reuse`는 pool 반납 시점의 내부 정리이고
public 관측 대상이 아니다. 캐시 필드를 남기는 것은 다음 사용자가
`bind_callback_state()`로 반드시 재검증하므로 stale 값이 관측될 수 없다.
`routing_target_t`의 native 캐시도 presence 플래그가 거짓이면 읽히지 않는다
(`target_first_rid_native()` 참조).

### C4. `recv_single_part_message`의 guard/재질의 부기 축소 — **구현함(재질의 제거는 부분)**

| 파일 | 위치 | 내용 |
|------|------|------|
| `bindings/cpp/src/Runtime/Sockets/detail.hpp` | 74–83 | `assign_recv_source_rid()` 추출(두 경로 공용, 중복 제거) |
| 같은 파일 | 85–117 | **valid·empty 출력 message용 inline fast path 추가** — `recv_part_out_guard_t` 객체를 만들지 않고, `prepare()`의 `init()` 재확인, `_has_saved`/`_committed` 분기, 소멸자 분기를 모두 건너뛴다. 실패 시 동작은 guard 소멸자와 동일하게 `part_out_.close()`다. |
| 같은 파일 | 105–107 | 성공 경로에서 `refresh_payload_presence()`(=`valid() && zlink_msg_size(...)>0`) 대신 **`message_access_t::has_payload` 직접 대입**. 이 지점에서 frame의 valid 여부는 이미 확정이므로 재검증 분기를 없앴다. |

**`zlink_msg_size` 호출 자체는 남겼다(부분 no-go)**: Core의
`zlink_recv_part()`는 수신 크기를 out param으로 돌려주지 않는다
(`core/include/zlink/socket/api.h:465`). `message_t::_has_payload`가 실제
frame 상태를 반영해야 하므로, 크기 질의를 없애려면 (a) Core API 변경(범위 밖,
`core/` 금지) 또는 (b) `_has_payload`를 보수적으로 항상 `true`로 두는 방법뿐이다.
(b)는 message_t를 재사용하는 정상 사용자에게 매 수신마다 guard의
save/restore를 강제하므로 순손해다. 따라서 "중복 재질의"는 **재검증 분기 제거
수준까지만** 달성했고, native 크기 질의 1회는 유지한다.

**contract 보존 논거**: 비어 있지 않은 출력 message에 대한 실패 시 복원 계약은
기존 guard 경로가 그대로 담당한다(fast path 진입 조건이
`part_out_.valid () && !has_payload (part_out_)`). 실패 시 반환값·errno·message
상태는 두 경로가 동일하다. fast path는 `errno = EMSGSIZE`를 `close()` **뒤에**
설정하므로 errno가 덮일 위험은 오히려 줄었다.

**POSDDD**: payload 유무는 `message_t`가 스스로 아는 상태이고, 그 상태를 근거로
"복원할 것이 있는가"를 판정하는 책임을 socket 헬퍼에서 message 상태 질의
한 번으로 정리했다.

### C5. C++ perf 하네스 설정을 C reference와 정렬 — **구현함(구조개선/측정 의미 정렬)**

| 파일 | 위치 | 내용 |
|------|------|------|
| `bindings/cpp/perf/single/src/perf_pair.cpp` | 84–90 → 84–88 | `bind_socket.options ().tcp_no_delay (true)`, `conn_socket.options ().tcp_no_delay (true)`, `perf::single::recalculate_single_auto_hwm (ctx)` 블록 삭제 후 근거 주석으로 대체 |

`bindings/c/perf/single/src/perf_pair.cpp`에는 두 호출이 **모두 없다**
(`grep -n "no_delay\|recalculate\|auto_hwm" bindings/c/perf/single/src/*.cpp` 무결과).
계획서 §5 "perf는 측정 의미가 C와 다른 경우에만 수정한다"에 해당한다.
binding library public API와 무관하며, 다른 pattern 파일은 건드리지 않았다
(변경을 분리 가능하게 유지).

## 3. 테스트 결과

명령: `bash bindings/cpp/tests/run_tests.sh` (configure + build + `ctest -L contract` + samples)

| 테스트 | before(변경 전 작업 트리) | after(C1+C3+C4+C5) |
|--------|---------------------------|--------------------|
| 14개 contract 테스트 | 12 pass / **2 fail** | 12 pass / **2 fail (동일)** |
| `test_cpp_contract_socket` | abort — `submit_error_t: No such file or directory (errno=113)` | **동일** |
| `test_cpp_contract_request_reply` | abort — `test_cpp_contract_request_reply.cpp:714` `test_routed_send_async_isolates_a_backpressure_from_b()` assertion | **동일** |
| samples smoke (7개) | 6 pass / **1 fail** (`sample_cpp_dealer_router_recv_sample`) | **동일** |

**두 contract 실패와 sample 실패는 이번 변경 이전에 이미 존재하던 것**이며,
변경 전 트리를 stash로 되돌려 같은 명령으로 재확인했다. 새로 깨진 테스트는 없다.

close/파괴 경합 관련 테스트는 다음이 **통과**한다(=회귀 없음):
- `test_routed_async_builder_does_not_outlive_socket_anchor()`
  (`test_cpp_contract_request_reply.cpp:618`) — 파괴된 소켓의 builder,
  닫힌 소켓의 builder 모두 `invalid_state`/`EINVAL`. C1의 anchor 설계를 직접
  검증한다. 실패 지점(714행)보다 **앞서** 실행되므로 실제로 통과했다.
- `test_send_throws_on_general_error()` / `test_publish_throws_on_general_error()`
  / `test_router_send_throws_for_closed_socket()`
  (`test_cpp_contract_behavior.cpp:223/234/210`) — 닫힌 소켓 send/publish.
  `test_cpp_contract_behavior`는 전체 통과.

### 3.1 추가 전용 검증(throwaway)

트리 밖(`scratchpad/lifetime_check.cpp`)에 공개 API만 쓰는 검증 프로그램을 만들어
실행했다. 결과 전부 통과:

1. `reuse-ok` — pair 소켓 200회 생성·파괴·send 반복. pooled state의
   소켓 키 캐시가 **주소 재사용**을 만나도 오동작하지 않음.
2. `orphan-sync-ok` — 소켓이 파괴된 뒤 **동기** `submit()` 호출 →
   `invalid_state`/`EINVAL` 던지고 payload는 caller에게 남음(crash 없음).
3. `orphan-sync-mixed-ok` — 같은 thread_local pooled state가 먼저 **살아 있는**
   다른 소켓에 바인딩된 뒤 위 시나리오 → 동일하게 정상 예외.
4. `recv-guard-ok` — payload를 가진 message_t로 실패하는 recv → 이전 payload
   복원됨(guard 경로 유지). 빈 message_t로 실패하는 recv → invalid로 남음
   (fast path, 기존 guard 소멸자와 동일).
5. `recv-roundtrip-ok` — fast path 수신 후 payload와 presence 플래그가 정상,
   같은 message_t 재사용 시 guard 경로로 전환되어 복원 계약 유지.

## 4. 측정 (탐색, 로컬 core)

조건: `PAIR` / `tcp` / 64 B / duration 5 / `--runs 3`을 한 배치로 보고 **3 배치**
실행, 배치 median들의 median을 대표값으로 삼았다.

### 4.1 C++ binding

| 배치 | before (msg/s) | after C1+C3+C4+C5 (msg/s) |
|------|---------------:|--------------------------:|
| 1 | 2,151,410.400 | 2,132,297.000 |
| 2 | 2,060,151.400 | 2,171,471.600 |
| 3 | 2,045,469.600 | 2,140,546.800 |
| **median** | **2,060,151.400** | **2,140,546.800** |
| latency mean median (ms) | 64.775 | 56.903 |

중간 지점(C1+C3+C4만, C5 미적용) 1배치: **2,169,972.000** msg/s, latency 62.238 ms
(`perf_cpp_single_linux_20260823_130722_c134-after.txt`).

### 4.2 C reference (동일 조건, 동일 세션)

| 배치 | throughput (msg/s) | latency mean (ms) |
|------|-------------------:|------------------:|
| 1 | 2,696,726.200 | 48.291 |
| 2 | 2,550,299.200 | 54.485 |
| 3 | 2,556,849.200 | 54.587 |
| **median** | **2,556,849.200** | 54.485 |

### 4.3 비율

| 항목 | before | after | 변화 |
|------|-------:|------:|------|
| C++ throughput | 2,060,151.400 | 2,140,546.800 | **+3.90%** |
| C++/C 비율 | **0.8057** | **0.8372** | **+3.15%p** |
| C++ latency mean | 64.775 ms | 56.903 ms | **−12.2%** |
| C++/C latency 비율 | 1.189 | 1.044 | 개선 |

배치 간 산포가 ±3% 수준이라 +3.9%는 **잡음보다는 크지만 여유가 크지 않다.**
공식 판정은 release core 재릴리스 후 §7.3 규칙으로 다시 해야 한다.

### 4.4 부수 회귀 확인 (다른 pattern, tcp / 64 B / duration 5 / runs 3)

| pattern | before | after | 판정 |
|---------|-------:|------:|------|
| `DEALER_DEALER` | 9,791.600 msg/s (lat 0.117 ms) | 9,735.200 msg/s (lat 0.117 ms) | −0.58%, **잡음 범위 — 회귀 없음** |
| `PUBSUB` | 1,273,783.000 msg/s | 1,373,134.200 msg/s | **+7.80% — 개선** |
| `ROUTER_ROUTER` | `status: partial` (`non_zero_exit_1`) | `status: partial` (`non_zero_exit_1`) | **변경 전에도 실패 — 기존 문제, 이번 변경과 무관** |

`ROUTER_ROUTER` single/tcp가 변경 전 트리에서도 동일하게 실패함을 되돌려 확인했다
(`perf_cpp_single_linux_20260823_131402_c1345-before-rr.txt`). 별도 조사 항목이다.

### 4.5 결과 파일

C++ (`bindings/cpp/perf/results/single/report/`):
`..._130135_c1345-before.txt`, `..._131115_c1345-before-r2.txt`,
`..._131150_c1345-before-r3.txt`, `..._130722_c134-after.txt`,
`..._130829_c1345-after.txt`, `..._130857_c1345-after-r2.txt`,
`..._130913_c1345-after-r3.txt`, `..._130222_c1345-before-dd.txt`,
`..._131305_c1345-after-dd.txt`, `..._131504_c1345-before-ps.txt`,
`..._131423_c1345-after-ps.txt`, `..._131402_c1345-before-rr.txt`,
`..._131335_c1345-after-rr.txt`, `..._131350_c1345-after-rr2.txt`

C (`bindings/c/perf/results/single/report/`):
`..._130200_c1345-before.txt`, `..._131223_c1345-cref-r2.txt`,
`..._131238_c1345-cref-r3.txt`

## 5. 예비 판정 요약

| 후보 | 결과 | 예비 판정 | 근거 |
|------|------|-----------|------|
| C1 | 구현함(설계 조정: 수명 토큰 유지, 비용을 소켓당으로 이동) | **채택 후보(예비)** | hot loop에서 send당 lock 접두 RMW 4회 소멸, contract·테스트 유지 |
| C2 | **미구현** | **보류(조건부 no-go 유지)** | `ETERM` 관측 시점 동등성 미증명 |
| C3 | **부분 구현** — `RAW_SEND_HOT_PATH` 분기 및 256 B memset 제거. 스택 축소 state는 no-go | **구조개선(성능 중립~소폭 개선), 예비** | 스택 state는 공개 builder 타입 멤버 변경을 요구 → 계획서 §5 저촉 |
| C4 | 구현함(guard 회피 fast path). `zlink_msg_size` 1회는 유지 | **채택 후보(예비)** | Core recv API가 크기를 돌려주지 않아 완전 제거는 불가 |
| C5 | 구현함(`perf_pair.cpp`만) | **구조개선(측정 의미 정렬), 예비** | C reference와 소켓·context 설정 정렬. 단독 성능 효과는 잡음 수준 |

## 6. 다음 조치

1. `core/v0.12.0` release asset 재릴리스 후 계획서 §7.3 규칙으로 공식 paired
   측정을 다시 수행하고 이 문서의 예비 판정을 확정한다.
2. `ROUTER_ROUTER` single/tcp `non_zero_exit_1`(변경 전부터 존재)을 별도로
   조사한다.
3. `test_cpp_contract_socket`(errno=113)과
   `test_cpp_contract_request_reply:714`, `sample_cpp_dealer_router_recv_sample`
   의 기존 실패도 별도 조사 대상이다 — 이번 변경과 무관하지만
   contract gate가 열려 있는 상태다.
4. C2는 `socket_closed` 경합 시 `ETERM` 관측 시점 동등성을 먼저 증명한 뒤에만
   착수한다.
