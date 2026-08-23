# C++ PAIR/tcp/64B 탐색 프로파일링 기록 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §5(고정 원칙), §7.2(반복 횟수 — **탐색** 단계: 기본 duration, 1회), §7.3(paired C 규칙)
>
> **단계: 탐색(비공식).** 이 문서의 모든 수치는 **로컬 core 빌드**(`core/build`)
> 기준이며 판정값이 아니다. `core/v0.12.0` release asset은 아직 결함 commit
> 기준으로 빌드돼 있어 무효 상태다(`log/2026-08-23-cpp-single-smoke.md` §7 참조).
> 공식 paired 판정은 release asset 재릴리스 후 §7.3 규칙으로 다시 실행한다.
>
> **목적**: `log/2026-08-23-cpp-single-smoke.md` "로컬 core 재시도" 절에서 관찰된
> C++ / C 처리량 비율 약 76.7%의 원인을 병목 위치별로 분해하고, 계획서 §5 기준
> (allocation·copy·dispatch·callback·poller 비용의 책임 위치, POSDDD 위험 신호,
> public API·ownership·error contract 유지)을 만족하는 개선 후보를 선별한다.
>
> **코드 변경 없음**: shipping code는 전혀 수정하지 않았다. 프로파일링용
> 계측 프로그램은 모두 `/tmp/claude-1000/zlprof` 아래에만 만들었고 작업 종료
> 시점에 삭제했다. commit·push도 하지 않았다.

## 1. 재현 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `12th Gen Intel(R) Core(TM) i7-1260P`, 16 logical cores |
| CPU governor | 읽을 수 없음 (WSL2에 cpufreq 미노출) — CPU pin 없음 |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742` |
| working tree | C++ perf runner 및 public options 미커밋 변경 존재(SPOT 제거, `--sndbuf`/`--rcvbuf`) — 그대로 유지 |
| Core runtime 소스 | **local** (`ZLINK_CORE_SOURCE=local`, 두 러너의 현재 기본값) |
| Core runtime 파일 | `core/build/lib/libzlink.so.0.12.0` (mtime 2026-08-23 12:28 KST) |
| Compiler | `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| perf 대상 build flags | `-O3 -DNDEBUG -std=gnu++20 -Wall -Wextra -Wpedantic`, `ENABLE_LTO=OFF` (러너가 쓰는 값 그대로) |
| session tag | `explore-cpp-gap-20260823` |
| 조합 | `PAIR` / `tcp` / `64` B / duration **5초** / runs **1** (탐색 단계 기본값) |

### 1.1 프로파일러 가용성

이 호스트에는 `perf`(linux-tools), `valgrind`가 **설치돼 있지 않다**
(`/usr/bin/perf*`, `/usr/lib/linux-tools*` 부재). 또한
`/proc/sys/kernel/perf_event_paranoid`가 `2`라 설치돼 있어도 비특권 샘플링이
제한된다. `gprof`는 있으나 `-pg` 재빌드가 필요해 측정 대상 바이너리의 빌드
조건(`-O3`, LTO off)을 바꾸게 된다.

따라서 지시의 3순위 방법인 **throwaway 계측**을 택하되, 트리 안의 어떤 코드도
건드리지 않는 형태로 구성했다(§3). 샘플링 프로파일 대신 **경로 치환
차분(ablation)** 방식을 썼기 때문에, 아래 §4의 "비중"은 함수별 CPU 샘플
백분율이 아니라 **해당 계층을 C API 직접 호출로 치환했을 때 회복되는 처리량의
전체 gap 대비 비율**이다. 이 차이를 표에 명시한다.

## 2. gap 재현 (공식 러너, 탐색 조건)

### 2.1 실행한 명령

```bash
# C reference (local core 기본값)
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 5 --runs 1 \
  --results-tag explore-cpp-gap-20260823

# C++ binding (local core 기본값, 동일 조합)
PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 5 --runs 1 \
  --results-tag explore-cpp-gap-20260823
```

### 2.2 결과

| 대상 | 결과 파일 | status | throughput (msg/s) | bandwidth (MB/s) | latency mean (ms) | p95 / p99 (ms) |
|------|-----------|--------|-------------------:|-----------------:|------------------:|----------------|
| C reference | `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_123939_explore-cpp-gap-20260823.txt` | **complete** | 2,756,058.200 | 176.388 | 50.330 | 53.760 / 56.021 |
| C++ binding | `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_123954_explore-cpp-gap-20260823.txt` | **complete** | 2,173,000.200 | 139.070 | 63.154 | 67.299 / 68.646 |

- throughput 비율(C++ / C) = 2,173,000.200 / 2,756,058.200 = **0.7885** (78.85%)
- latency 비율(C++ / C, mean) = 63.154 / 50.330 = **1.255**

C 러너 report의 `META,core_source,local` /
`META,core_runtime,/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.12.0`로
local core 사용을 확인했다. duration을 1초에서 5초로 늘려도 smoke 때의 비율
(76.7%)과 같은 수준(78.9%)이 재현됐으므로, gap은 짧은 측정창의 잡음이 아니다.

## 3. 프로파일링 방법 — 경로 치환 차분

### 3.1 설계

`bindings/cpp/perf/single/src/perf_pair.cpp`를 그대로 복사해 `/tmp` 안에서만
사는 throwaway 변형을 만들고, **컴파일 타임 매크로로 특정 계층만 C API 직접
호출로 치환**했다. 하네스(스레드 구성, poller 대기, stamp/decode, latency
누적, deadline 판정, stop token 처리)는 모든 변형에서 문자 그대로 동일하다.
따라서 변형 간 처리량 차이는 **치환한 그 계층의 비용**만을 나타낸다.

- throwaway 소스: `/tmp/claude-1000/zlprof/perf/single/src/perf_pair_variant.cpp`
- 실제 트리의 `bindings/cpp/perf/common`, `bindings/cpp/perf/single/common`을
  symlink로 참조하고, 이미 빌드돼 있는 오브젝트
  (`bindings/cpp/build/perf/CMakeFiles/cpp_perf_pair.dir/single/common/*.o`)와
  `bindings/cpp/build/libzlink_cpp.a`, `core/build/lib/libzlink.so`를 링크했다.
- native handle은 binding 내부 헤더 `Runtime/Sockets/socket_access.hpp`로
  얻었다(치환 변형 전용, shipping code 아님).

```bash
c++ -O3 -DNDEBUG -std=gnu++20 \
  -DVARIANT_C_SEND=<0|1> -DVARIANT_C_RECV=<0|1> \
  -DVARIANT_C_POLL=<0|1> -DVARIANT_NO_NODELAY=<0|1> -DVARIANT_NO_RECALC=<0|1> \
  -I bindings/cpp/include -I bindings/cpp/perf/single/common \
  -I bindings/cpp/perf/multi/common -I bindings/cpp/src -isystem core/include \
  perf_pair_variant.cpp <perf_single_common.o> <perf_single_runner.o> \
  bindings/cpp/build/libzlink_cpp.a core/build/lib/libzlink.so
```

실행: `LD_LIBRARY_PATH=core/build/lib PERF_SINGLE_DURATION_SECONDS=5 ./pair_<변형> current tcp 64`

비교 기준선으로 C reference 바이너리 `bindings/c/build/perf/perf_pair`를 같은
셸 세션에서 같은 조건으로 실행했다.

### 3.2 변형 목록

| 변형 | send 경로 | recv 경로 | poller | 기타 |
|------|-----------|-----------|--------|------|
| `base` | C++ binding (`socket.send().message(msg).flags(...).submit()`) | C++ binding (`socket.recv(message_t&, dontwait)`) | `zlink::poller_t` | shipping과 동일 |
| `csend` | C API (`zlink_msg_init_size`+`memcpy`+`zlink_send_part`) | C++ binding | `zlink::poller_t` | |
| `crecv` | C++ binding | C API (`zlink_msg_init`+`zlink_recv_part`+`zlink_msg_close`) | `zlink::poller_t` | |
| `cboth` | C API | C API | `zlink::poller_t` | |
| `cpoll` | C API | C API | C API (`zlink_poller_new/add/wait`) | |
| `cboth_nodelayoff` | C API | C API | `zlink::poller_t` | `tcp_no_delay(true)` 제거 |
| `cboth_norecalc` | C API | C API | `zlink::poller_t` | `recalculate_auto_hwm()` 제거 |
| `cref` | — | — | — | C reference 바이너리 `bindings/c/build/perf/perf_pair` |

## 4. 측정 증거

### 4.1 계층별 차분 (5초, 3회, msg/s)

| 변형 | run1 | run2 | run3 | median |
|------|-----:|-----:|-----:|-------:|
| `base` (shipping C++ 경로) | 2,055,709 | 1,821,741 | 2,044,625 | **2,044,625** |
| `crecv` (recv만 C) | 2,131,239 | 2,132,150 | 2,087,278 | **2,131,240** |
| `csend` (send만 C) | 2,337,944 | 2,315,506 | 2,276,200 | **2,315,506** |
| `cboth` (send+recv 모두 C) | 2,427,526 | 2,320,581 | 2,284,471 | **2,320,581** |
| `cref` (C reference 바이너리) | 2,809,390 | 2,590,896 | 2,551,095 | **2,590,896** |

두 번째 배치(같은 조건 3회)에서도 순서와 값이 재현됐다:
`base` median 2,019,614 / `cboth` median 2,322,491 / `cref` median 2,590,619.

### 4.2 gap 분해

전체 gap = `cref` − `base` = 2,590,896 − 2,044,625 = **546,271 msg/s**

| 계층 | 회복량 (msg/s) | 전체 gap 대비 | 근거 |
|------|---------------:|--------------:|------|
| **binding send 계층** | 270,881 | **49.6%** | `csend` − `base` |
| **binding recv 계층** | 86,615 | **15.9%** | `crecv` − `base` |
| binding send+recv 합계 | 275,956 | 50.5% | `cboth` − `base` (send/recv가 완전 가산적이지는 않음 — send 제거 후 recv 비용이 임계경로에서 벗어남) |
| **binding 밖 잔여** | 270,315 | **49.5%** | `cref` − `cboth` |

즉 **gap의 약 절반이 binding 호출 계층**에 있고, 그 안에서 **send 계층이 recv
계층의 약 3.1배**다. 나머지 절반은 binding 호출 계층 밖이다.

### 4.3 잔여 49.5%에 대해 배제한 요인

| 가설 | 검증 변형 | 결과 (msg/s) | 판정 |
|------|-----------|-------------:|------|
| `zlink::poller_t` wrapper가 `zlink_poller_wait` 대신 `zlink_poll` fast path를 쓰는 비용 | `cpoll` vs `cboth` | 2,265,419 vs 2,342,130 (동일 run) | **배제** — C poller로 바꾸면 오히려 느려진다 |
| C++ 하네스만 `tcp_no_delay(true)`를 설정(C `perf_pair`는 미설정) | `cboth_nodelayoff` vs `cboth` | 2,320,043 vs 2,342,130 (동일 run) | **배제** — 차이 없음(잡음 범위) |
| C++ 하네스만 `recalculate_auto_hwm()`를 호출(C `perf_pair`는 미호출) | `cboth_norecalc` vs `cboth` | median 2,353,499 vs 2,322,491 | **부분 설명** — 약 31,008 msg/s, 전체 gap의 **5.7%** |
| context option 차이(io_threads / max_sockets / blocky / auto_hwm profile) | 소스 대조 | `apply_ctx_options` 양쪽 동등 | **배제** |
| latency sampler 비용 차이 | 소스 대조 | `latency_stats_builder_t` 양쪽 구현 동일(cap 4,000,000 동일) | **배제** |
| binding socket 생성이 native callback을 추가 등록 | 소스 대조 (`socket_t::socket_t(context_t&, socket_type)`) | `zlink_socket` + `make_shared<socket_callback_state_t>` 뿐, native handler 등록 없음 | **배제** |

결론적으로 잔여의 약 5.7%p는 auto-HWM 재계산으로 설명되고, **나머지 약
43.8%p는 이번 탐색에서 위치를 특정하지 못했다.** 남은 차이는 C 하네스와 C++
하네스의 남은 구조 차이(수신 drain 루프 형태, 단일 TU 인라이닝 대 static
library 경계 등)에 있을 것으로 보이며, 샘플링 프로파일러 없이 더 좁히지
못했다. 이 부분은 **binding library의 개선 후보로 올리지 않는다** — 근거가
충분하지 않기 때문이다.

### 4.4 send 계층 안의 비용 구조 (소스 분석 + 마이크로 계측)

`bindings/cpp/perf` PAIR sender가 호출하는 public 경로:
`conn_socket.send().message(msg).flags(none).submit()`

C 경로 대비 **메시지 1건당 추가로 수행하는 일**:

1. `pair_socket_t::send()` (`src/Runtime/Sockets/pair.cpp:18`)
   - `detail::acquire_state()` — thread_local `vector<unique_ptr<operation_state_t>>` pool에서 pop
   - `state_ptr->raw.callbacks = callback_state().weak_from_this()` — **weak count atomic 증가**
2. `.message(msg)` (`send_operations.cpp:280`) — borrow fast path(포인터 저장), 복사 없음
3. `.flags(int)` (`send_operations.cpp:259`) — 무시할 수준
4. `.submit()` → `detail::submit_raw_send_state()` (`src/Runtime/Messaging/operation_submit.hpp:28`)
   - `state_.raw.callbacks.lock()` — **strong count atomic CAS**, scope 종료 시 **atomic 감소**
   - `callbacks->socket_closed.load(acquire)` × 2 — atomic load 2회
   - `std::lock_guard<std::mutex> (callbacks->outbound_record_attempt_mutex)` — **비경합 mutex lock/unlock**
   - `zlink_send_part(...)` ← **C 경로가 하는 유일한 일**
   - `zlink::detail::mark_sent(part)`
5. builder 소멸 → `detail::release_state()` → `reset_for_reuse()`
   (`operation_state.hpp:213`) — `callbacks.reset()`으로 **weak count atomic 감소**,
   pool로 push_back

즉 메시지 1건당 lock 접두 RMW가 **최소 4회**(weak inc / strong CAS / strong dec
/ weak dec) + mutex lock/unlock + atomic load 2회 추가된다.

이 동기화 원시연산만 따로 계측했다
(`/tmp/claude-1000/zlprof/atomics_price.cpp`, `-O2`, 5,000만 회):

```
weak_from_this+lock+reset : 14.13 ns/op
mutex gate + 2 acq loads  : 3.36 ns/op
total binding sync per send: 17.50 ns
```

파이프라인 관점의 환산: `base` 2,044,625 msg/s → 489.1 ns/msg,
`csend` 2,315,506 msg/s → 431.9 ns/msg. send 계층 치환으로 회수되는 시간은
**메시지당 약 57.2 ns**이고, 그중 **약 17.5 ns(≈31%)가 위 동기화 원시연산**,
나머지 약 39.7 ns가 builder/pooled-state 기계장치, static library 경계의
비인라인 호출, 그리고 그로 인한 cache/branch 효과다.
(주의: 이 환산은 sender가 처리량 제한 요인이라는 1차 근사이며, sender CPU
시간을 직접 잰 값이 아니다.)

### 4.5 recv 계층 안의 비용 구조 (소스 분석)

`bind_socket.recv(message_t&, dontwait)` → `pair_socket_t::recv`
(`src/Runtime/Sockets/pair.cpp:36`) → `detail::recv_single_part_message`
(`src/Runtime/Sockets/detail.hpp:71`).

C 경로 대비 추가되는 일:

- `recv_part_out_guard_t` 생성/소멸 (`detail.hpp:28`) — 하네스가 매 반복
  빈 `message_t`를 새로 만들기 때문에 save/restore 자체는 건너뛰지만,
  guard 객체와 `_committed`/`_has_saved` 분기, 소멸자 분기는 남는다
- `part_guard.prepare()` → `message_t::init()` 재확인 호출
- `refresh_payload_presence(part_out_)` (`Native/message_access.hpp`) —
  **`zlink_msg_size` 추가 호출** 1회
- `libzlink_cpp.a` 경계를 넘는 비인라인 호출(C 하네스는 헤더 inline이라 단일
  TU 안에서 인라인됨)

message 생성/소멸 자체는 양쪽 동일하다(C도 `zlink_msg_init` /
`zlink_msg_close` 1쌍). 64 B는 `init_owned_message_storage`의 pooled 구간
(128 KiB~1 MiB) 밖이라 binding의 large-message pool은 이 셀에서 전혀 관여하지
않는다 — 이 셀의 recv 비용은 **allocation이 아니라 wrapper 부기(簿記)**다.

## 5. 개선 후보 (순위)

순위 기준: 계획서 §5 및 이번 라운드 정책 — **성능 이득이 있는 후보를 먼저
두고, 성능 중립이더라도 POSDDD 상 책임·상태 경계를 명확히 하는 후보는
"구조개선(성능 중립)"으로 표시해 유지한다.** 모든 후보는 public API signature,
public type·enum 값, ownership, error 동작을 바꾸지 않는 범위로 한정했다.

### C1. raw send 경로의 per-call `weak_ptr` 왕복 제거 — 성능 개선

- **메커니즘**: `pair_socket_t::send()`가 `weak_from_this()`를 pooled state에
  저장(weak count atomic 증가)하고, `submit_raw_send_state()`가 `lock()`
  (strong CAS + scope 종료 시 감소)하며, `reset_for_reuse()`가 `reset()`
  (weak count 감소)한다. 동기 raw send는 호출문 하나 안에서 시작하고 끝나므로
  소켓이 살아 있음이 **구성상 보장**되는데도 lifetime 추적 비용을 매 건 낸다.
- **gap 기여 추정**: send 계층 49.6% 중 동기화 원시연산 몫(메시지당 14.1 ns의
  대부분) → **전체 gap의 약 12–15%**.
- **변경 스케치**: 동기 terminal(`submit()`)이 쓰는 pooled state에는
  `socket_callback_state_t*` raw pointer와 기존 `socket_closed` atomic 플래그만
  두고, `shared_ptr` 승격은 실제로 호출을 넘겨 사는 경로(async terminal,
  routed/publish admission record)에서만 수행한다. 또는 `socket_t`가
  `shared_ptr`를 이미 멤버로 보유하므로 operation state에 `socket_callback_state_t&`를
  전달한다.
- **contract**: public API signature·ownership·error 동작 모두 불변. `ETERM`,
  `EINVAL` 던지는 조건과 backpressure 반환 의미 그대로.
- **POSDDD 배치**: callback state의 **lifetime 소유는 `socket_t`의 책임**이다.
  호출문 수명을 넘지 않는 operation state가 소유권 추적 비용을 재차 지불하는
  것은 책임 중복이다. 비용을 `socket_t`로 되돌린다.
- **no-go 여부**: no-go 아님.

### C2. 단일 part raw send fast path의 `outbound_record_attempt_mutex` 범위 축소 — 성능 개선

- **메커니즘**: `submit_raw_send_state()`가 모든 send에서 소켓 단위 mutex를
  잡고 `socket_closed`를 두 번 acquire-load 한다. 단일 part PAIR/DEALER send는
  native `zlink_send_part` 한 번으로 끝나고 core가 이미 소켓 단위 직렬화를
  보장한다.
- **gap 기여 추정**: 메시지당 약 3.4 ns → **전체 gap의 약 3%**.
- **변경 스케치**: 이 gate를 **여러 native 호출에 걸친 원자성이 실제로 필요한
  경로**(다중 part 제출, routed/publish admission record, async 재개)로 한정하고,
  단일 part 직접 제출에는 `socket_closed` 검사 1회만 남긴다.
- **contract**: public API 불변. **단, 종료 경합 시 `ETERM` 관측 시점이 달라질
  수 있는지 반드시 검증해야 한다.** 검증되지 않으면 gate를 유지한다(그 경우
  이 후보는 no-go).
- **POSDDD 배치**: 레코드 간 순서 보장은 **admission state 객체의 책임**이지,
  모든 send가 무조건 지불할 비용이 아니다.
- **no-go 여부**: 조건부 — error contract 동등성 증명 실패 시 no-go.

### C3. 동기 raw send terminal의 pooled `operation_state_t` 기계장치 축소 — 성능 개선 + 구조개선

- **메커니즘**: 매 send가 `acquire_state()`/`release_state()`로 thread_local
  pool을 오가고, `unique_ptr` 간접참조를 거치며, builder 객체 2개
  (`send_operation_t` → `send_submit_operation_t`)의 move-ctor/dtor를 통과한다.
  `operation_state_t`는 `optional<message_t>`, `vector<message_t>`,
  `std::string topic`, routing target을 담은 큰 구조체이고 매 건
  `reset_for_reuse()`된다. 현재 코드에 이미 `RAW_SEND_HOT_PATH` 특수 분기가
  주석과 함께 박혀 있다는 사실 자체가 POSDDD 위험 신호다.
- **gap 기여 추정**: send 계층 49.6% 중 동기화 몫을 뺀 나머지의 상당 부분
  → **전체 gap의 약 15–25%**(정확한 분리는 이번 탐색 범위 밖).
- **변경 스케치**: 동기 terminal은 호출문을 벗어나지 않으므로 heap 수명
  기계장치가 필요 없다. raw send용 축소 state를 스택에 두고, pooled/heap state는
  async terminal 전용으로 남긴다. `reset_for_reuse()`의 `raw_send` 특수 분기는
  그 결과로 사라진다.
- **contract**: public builder 체인 signature·ownership·error 동작 불변.
- **POSDDD 배치**: **호출문보다 오래 살 수 없는 operation은 heap 수명 기계장치를
  지불하지 않아야 한다.** 지금은 동기·비동기 두 수명 모델을 한 state 타입이
  겸하면서 hot path에 특수 분기를 만들었다 — 이것이 제거 대상인 "불필요한 특수
  경우"다.
- **no-go 여부**: no-go 아님.

### C4. `recv_single_part_message`의 guard/재질의 부기 축소 — 성능 개선 + 구조개선

- **메커니즘**: `recv_part_out_guard_t` 객체와 `_committed`/`_has_saved` 분기,
  `prepare()`의 `init()` 재확인, 그리고 성공 경로마다 `refresh_payload_presence()`가
  `zlink_msg_size`를 **다시** 부른다(수신 직후 크기는 이미 확정돼 있다).
- **gap 기여 추정**: **전체 gap의 약 16%**(`crecv` − `base` = 86,615 msg/s) 중
  wrapper 부기 몫.
- **변경 스케치**: "호출자가 넘긴 valid·empty message" 케이스에 guard 객체를
  아예 만들지 않는 inline fast path를 두고, payload 유무는 수신 직후 이미 읽는
  크기 값에서 직접 갱신한다.
- **contract**: public API·ownership·error 동작 불변. 비어 있지 않은 출력
  message에 대한 실패 시 복원 계약은 기존 경로로 그대로 유지한다.
- **POSDDD 배치**: **payload 유무는 `message_t`가 스스로 아는 상태**여야 하고,
  socket이 native 계층에 재질의해서 message의 상태를 대신 갱신하는 것은
  정보 은닉 위반이다.
- **no-go 여부**: no-go 아님.

### C5. C++ perf 하네스의 소켓·context 설정을 C reference와 정렬 — 구조개선(성능 중립)

- **메커니즘**: `bindings/cpp/perf/single/src/perf_pair.cpp`가 C
  `perf_pair.cpp`에 없는 두 가지를 수행한다 — 양 소켓에 `tcp_no_delay(true)`
  설정, 그리고 `recalculate_single_auto_hwm(ctx)` 호출.
- **측정 결과**: `tcp_no_delay` 제거는 **차이 없음**. `recalculate_auto_hwm`
  제거는 median +31,008 msg/s(전체 gap의 5.7%)로 **작지만 방향성 있음**.
  단일 셀 기준이라 성능 후보로 올릴 만큼 견고하지 않다.
- **변경 스케치**: 두 호출을 제거해 C reference의 측정 의미와 정렬한다.
  계획서 §5는 "perf는 측정 의미가 C와 다르거나, 실제 버그가 있거나, `doc/perf`
  정책을 위반한 경우에만 수정한다"고 규정하며, 이는 **측정 의미 차이**에
  해당한다.
- **contract**: perf 하네스만 변경, binding library public API 무관.
- **POSDDD 배치**: 소켓 옵션 정책은 **C reference가 정의하는 측정 계약**에
  속하고, 언어별 하네스가 임의로 추가 설정을 얹는 것은 계약의 이중 소유다.
- **no-go 여부**: no-go 아님. 단, **다른 pattern/transport 셀에서 회귀가 없는지
  확인한 뒤**에만 적용한다(특히 `tcp_no_delay`는 큰 메시지 셀에서 효과가
  다를 수 있다).

### 후보 요약표

| 순위 | 후보 | 성격 | gap 기여 추정 | contract 영향 | POSDDD 소유자 | 판정 |
|------|------|------|--------------:|---------------|---------------|------|
| C1 | raw send의 per-call `weak_ptr` 왕복 제거 | 성능 개선 | 12–15% | 없음 | `socket_t` | 미착수 |
| C2 | 단일 part send의 `outbound_record_attempt_mutex` 범위 축소 | 성능 개선 | ~3% | **검증 필요**(ETERM 관측 시점) | admission state | 미착수 (조건부 no-go) |
| C3 | 동기 send terminal의 pooled state·builder 기계장치 축소 | 성능 개선 + 구조개선 | 15–25% | 없음 | operation terminal | 미착수 |
| C4 | `recv_single_part_message` guard/재질의 축소 | 성능 개선 + 구조개선 | ~16% 중 일부 | 없음 | `message_t` | 미착수 |
| C5 | C++ perf 하네스 소켓·context 설정을 C와 정렬 | **구조개선(성능 중립)** | ~5.7%(약함) | 하네스만 | C reference 측정 계약 | 미착수 |

### 후보로 올리지 않은 항목 (배제 근거는 §4.3)

- `zlink::poller_t` wrapper 비용 — C poller로 치환 시 오히려 느려짐
- binding large-message pool — 64 B는 pooled 구간(128 KiB~1 MiB) 밖, 미관여
- context option / latency sampler / binding socket 생성 — C와 동등함을 소스로 확인
- 잔여 약 43.8%p — **위치를 특정하지 못했다.** 근거가 부족하므로 후보로
  올리지 않는다. 샘플링 프로파일러(`perf`) 사용이 가능한 환경에서 다시 좁혀야
  한다.

## 6. 정리와 다음 조치

- **정리 완료**: `/tmp/claude-1000/zlprof` 아래의 throwaway 소스·바이너리·symlink를
  모두 삭제했다. repo 트리에는 계측 코드가 남아 있지 않다(`git status`로
  이번 작업 전후 변경 파일 목록이 동일함을 확인).
- **다음 조치(제안)**:
  1. C1 → C3 → C4 순으로 후보를 하나씩 구현하고, 계획서 §7.2 "후보 판정"
     조건(기본 duration, 3회)으로 before/after를 측정한다. C5는 다른 셀 회귀
     확인 후 적용한다.
  2. C2는 구현 전에 `socket_closed` 경합 시 `ETERM` 관측 시점이 동등한지 먼저
     증명한다. 증명 실패 시 no-go로 확정한다.
  3. `perf`를 쓸 수 있는 호스트에서 §4.3의 잔여 43.8%p를 다시 좁힌다.
  4. `core/v0.12.0` release asset 재릴리스 후 §7.3 규칙으로 공식 paired 측정을
     다시 수행하고, 이 문서의 수치는 탐색 기록으로만 남긴다.
