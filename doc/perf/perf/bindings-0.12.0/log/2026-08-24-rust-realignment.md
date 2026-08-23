# Rust binding 0.13.0 계약 재정렬 (send_complete 기반 async terminal)

작업 범위: `bindings/rust`만. commit 없음.
기준 문서: `bindings/doc/spec/async-coroutine-policy.ko.md`(3차 개정),
`doc/plan/core-send-completion-design.ko.md`, `core/include/zlink/socket/api.h`.
참조 구현: `bindings/cpp`(HEAD) —
`bindings/cpp/src/Runtime/Messaging/send_operations.cpp`,
`doc/perf/perf/bindings-0.12.0/log/2026-08-24-cpp-async-readd.md`.

## 구현

### 1. vendored header와 native payload 재동기화
- `bindings/rust/include`를 `core/include`에서 통째로 다시 복사했다.
  `diff -r core/include bindings/rust/include`가 무출력(byte-identical)이다.
- `bindings/rust/native/{linux-x86_64,linux-x64}`의 payload가 0.11.1로 남아
  있어 `build.rs`가 요구하는 `libzlink.so.0.13.0`을 만족하지 못했다. 재정렬
  이전 상태에서는 crate가 **빌드 자체가 불가능**했다. `core/build/lib`의
  0.13.0 런타임과 symlink를 payload에 다시 채웠다.

### 2. binding-owned admission 기계장치 삭제
- `src/internal/routed_admission.rs`(444줄)를 삭제했다. 함께 사라진 것:
  `RoutedWake`/`RoutedWakeOutcome`(park 상태기계), `RoutedTargetKey` pending
  map, `DeadlineToken` + `zlink-rust-deadline` **std::thread + mpsc reactor**,
  `routed_ready_trampoline`.
- 그 자리에 두 개의 좁은 모듈을 두었다.
  - `src/internal/routed_handle.rs` — DEALER/ROUTER native handle 공유 소유자.
    handle 보관, outbound submit gate, `zlink_select_routed_submit_target`,
    Core-owned request timeout 조회, close/detach만 한다. 스레드·큐·타이머
    없음.
  - `src/internal/send_completion.rs` — socket 단위 in-flight operation
    registry. `zlink_send_complete_handler` trampoline은 결과를 슬롯에 저장하고
    waker를 깨우는 일만 한다(Core는 completion 안에서의 submit을 `EDEADLK`로
    거부한다).
- `ffi.rs`: `zlink_send_ready_handler_fn`,
  `zlink_routed_send_ready_handler_fn`, `zlink_routed_send_ready_state_t`,
  `zlink_routed_send_ready_event_t`, 두 extern을 제거하고
  `zlink_send_complete_result_t`, `zlink_send_op_id_t`,
  `zlink_send_complete_event_t`, `zlink_send_async_options_t`,
  `zlink_send_complete_handler_fn`, `zlink_send_async`,
  `zlink_send_complete_handler`, `zlink_send_async_cancel`을 추가했다.
- Core에서 사라진 `send_ready` readiness-hint 표면에 대응하는 public API
  `PairSocket/DealerSocket/RouterSocket/StreamSocket/PubSocket/XPubSocket::on_send_ready`
  6개를 제거했다.

### 3. send / routed send — Core send_complete 기반 Future
- `SocketStorage::create`가 `zlink_send_async`가 지원하는 subject(PAIR,
  DEALER, ROUTER, STREAM)마다 socket 수명 동안 하나의
  `zlink_send_complete_handler`를 등록한다. handler 없이 `zlink_send_async`를
  부르면 Core가 `EINVAL`을 돌려주므로 lazy 등록 대신 eager 등록을 택했다.
- `SendOp<Ready>::submit()`과 `RoutedSendOp<Ready>::submit()`은
  runtime 비종속 `Future<Output = Result<(), SubmitError>>`를 반환한다.
  Future는 최초 poll 전까지 아무 것도 하지 않고, 최초 poll에서 record 전체를
  `zlink_send_async` **한 번**으로 넘긴다.
  - **Inline admission**: Core가 즉시 admit하면 completion이 `zlink_send_async`
    안에서 inline 실행된다. slot은 submit **이전에** userdata key로 등록해
    두므로 이 경우에도 completion을 놓치지 않고, 같은 poll이 곧바로 `Ready`가
    된다(테스트: `inline_admission_resolves_the_future_on_its_first_poll`).
  - **Timeout**: builder의 `timeout(Duration)`이
    `zlink_send_async_options_t::timeout_ms`(per-operation deadline)로 그대로
    전달된다. 바인딩 타이머는 없다.
  - **Drop**: 완료 전 drop은 `zlink_send_async_cancel(socket, op_id)`을
    요청한다. Core는 취소된 operation도 정확히 한 번 완료하므로 op state는
    socket-scoped registry가 completion이 도착할 때까지 살려 둔다. key는 slot의
    `Arc` 주소이고 cancel은 Core op id로 하므로 ABA-safe다.
  - **Terminal 매핑**: `ZLINK_SEND_ADMITTED` → `Ok(())`. 그 밖의 completion은
    C++ 참조 구현과 같이 `SubmitResult::NotAdmitted` + Core `terminal_errno`
    (`ETIMEDOUT`/`ECANCELED`/route errno)로 표면화한다.
- `Received::send()`는 ROUTER route면 routed async lane, STREAM route면 STREAM
  lane을 쓰지만 두 경우 모두 같은 `SendOp` 타입을 반환한다(반환 타입 균일성).
- `send()` builder에서 `flags(...)` 단계를 제거했다. `zlink_send_async`는 절대
  blocking하지 않고 flag 인자도 받지 않으므로 `DONT_WAIT`이 의미를 잃는다.

### 4. publish — 동기 전용
- `PublishOp<State>`를 새 public 타입으로 분리했다. `PubSocket::publish` /
  `XPubSocket::publish`가 `SendOp` 대신 이 타입을 반환한다.
- `PublishOp<Ready>::submit()`은 동기 `Result<(), SubmitError>`다. lossy PUB
  의미론상 대기가 없고, `ZLINK_PUB_OPT_NODROP`에서는 가득 찬 subscriber가
  즉시 `Backpressured` 오류가 된다. flag 단계는 유지했다.
- 기존 `Ok(bool)`(backpressure를 `Ok(false)`로 삼키던 형태)을 계약대로
  `Result<(), SubmitError>`로 바꿨다.

### 5. request — admission 기계장치만 제거
- request는 이미 Core reply callback이 완료를 구동하는 형태였다. 여기서
  park/재시도 loop, `RoutedWake` 등록, `DeadlineToken` 스케줄을 걷어냈다.
- 최초 poll에서 exact target을 한 번 고르고 `ZLINK_SEND_FLAGS_NONE`으로 한 번
  제출한다(C++ `request_reply.cpp`와 동일). HWM 대기와 reply deadline은 Core가
  소유하고(`ZLINK_REQUEST_TIMED_OUT`), 바인딩은 재시도 큐도 타이머도 두지
  않는다. `timeout(...)`은 Core의 per-request timeout으로 전달된다.
- acceptance 이후 Future drop은 소비자만 분리한다(Core는 계속 완료한다).

### 6. 테스트·문서
- `tests/routed_async_tests.rs`를 계약에 맞춰 다시 썼다(삭제가 아니라 재작성):
  inline admission, backpressure 후 reader drain으로 완료, per-operation
  timeout, drop→cancel, close→terminal, target 간 독립성, Core-owned request
  timeout — 7개.
- `tests/test_support/mod.rs`에 `poll_once`를 추가했다. Core send operation은
  Future가 poll돼야 실제로 제출되므로, "Core-owned pending 상태"를 만들려면
  await 없이 한 번 poll해야 한다.
- `tests/optimization_guard_tests.rs`: `zlink_routed_send_ready_handler` 대신
  `zlink_send_complete_handler`/`zlink_send_async`/`zlink_send_async_cancel`을
  required substrate로 바꾸고, 가드 3개를 추가했다 —
  `binding_has_no_send_ready_surface`,
  `binding_owns_no_thread_queue_or_deadline_machinery`,
  `publish_terminal_is_synchronous`.
- `tests/send_failure_tests.rs`의 DONT_WAIT 기반 검사 4개를 새 계약으로
  재작성했다(per-operation deadline으로 완료되는지, 모든 record가 정확히 한 번
  완료되는지).
- samples 3개와 perf single/multi 소스를 새 terminal에 맞췄다.
- 문서: `bindings/doc/spec/rust/README.{ko,en}.md`의 routed-async 절
  (readiness handler / park / DONTWAIT 재시도 / "PAIR·PUB·STREAM 즉시 submit")을
  send-completion 계약으로 교체했다. `bindings/doc/reference/rust/03-sockets.*`
  에서 `on_send_ready` 행 4개씩을 제거하고 `publish -> PublishOp`,
  `send -> Future` terminal을 반영했다.
  `bindings/doc/reference/rust/02-messaging.*`의 builder 표와 completion 문단도
  갱신했다.

## grep 증거

```
$ grep -rn "send_ready" bindings/rust/{src,include,samples,perf}
(무출력)

$ grep -rn "RoutedAdmission|routed_admission|RoutedWake|DeadlineToken|\
   routed_ready_trampoline|RoutedTargetKey|with_attempt_gate|attempt_gate" bindings/rust/src
(무출력)

$ diff -r core/include bindings/rust/include
(무출력 — byte-identical)

$ grep -rn "thread::spawn|thread::Builder" bindings/rust/src
bindings/rust/src/internal/deferred_cleanup.rs:43:        thread::Builder::new()
bindings/rust/src/internal/callback_lifecycle.rs:199:        let join = std::thread::spawn(...)
```

`src` 아래 남은 스레드 2개는 둘 다 send/admission 기계장치가 아니다. 지시대로
삭제하지 않고 여기에 flag한다.

- **`callback_lifecycle.rs:199`** — `#[cfg(test)] mod tests` 안이다. C callback이
  외부 스레드에서 도착하는 상황을 흉내내는 테스트 보조 스레드이며 라이브러리
  코드가 아니다.
- **`deferred_cleanup.rs:43`** — `zlink-rust-cleanup` worker. 이번 작업 이전부터
  있던 것이고, **C callback lifecycle 계약**을 위한 것이다: Rust `Drop`은
  실패를 반환할 수 없는데 `zlink_close`가 `EBUSY`로 거절할 수 있고, callback
  userdata는 Core가 마지막으로 참조할 때까지 살아 있어야 한다. 그래서 실패한
  native close와 그 callback box를 이 worker가 이어받는다. send admission,
  retry, deadline과는 무관하다. 가드 테스트에서도 이 파일만 사유를 적어
  예외 처리했다.

## 테스트 결과

- `cargo build` / `cargo build --tests --examples`: 경고 0, 오류 0.
- `cargo test -- --test-threads=1`: **158 passed, 0 failed** (16개 test target).
- 샘플 7개(`cargo run --example ...`) 전부 정상 종료.
- **NEW failure 없음.** 다만 "기존 실패인지 stash로 대조" 지시는 이번 경우
  구조적으로 불가능했다: HEAD의 Rust 바인딩은 `zlink_send_ready_handler` /
  `zlink_routed_send_ready_handler`를 참조하는데 Core 0.13.0에는 그 심볼이
  없어 **링크가 되지 않는다**(`git archive HEAD bindings/rust`로 뽑아
  0.13.0 payload로 빌드 → `undefined reference to zlink_routed_send_ready_handler`).
  즉 재정렬 전 상태에는 비교할 수 있는 green baseline 자체가 없다.
  아래 pre-existing 결함 두 건은 Core 0.12.0 + HEAD 바인딩 조합으로 별도
  재현해 pre-existing임을 확인했다.
- `cargo fmt` / `cargo clippy`는 이 toolchain에 component가 설치돼 있지 않아
  실행하지 못했다.

## PERF SMOKE

로컬 workspace Core(`core/build/lib/libzlink.so.0.13.0`, sha256
`d9658327f42c1ff97bf7f0b91511da4a51a4f79e34543948e79c4818a50451c2`) 사용.
판정은 report 파일의 `status:` 줄 기준이다. 이 머신은 RAM 11 GB이고 다른
바인딩 작업 에이전트 2개가 동시에 돌고 있어 load average가 15~47 사이를
오갔다. 절대 수치는 그 영향을 크게 받는다(같은 설정 재실행에서 PAIR가
785k → 105k → 550k msg/s로 흔들렸다). 아래 값은 부하가 상대적으로 낮았던
실행이다.

### single (tcp, 64B, duration 1, runs 1)

`perf_rust_single_linux_20260824_024744_rust-realignment-20260824-nopubsub.txt`
— **status: complete** (20/20 result line)

| pattern | throughput (msg/s) | bandwidth (MB/s) | lat mean (ms) | p95 | p99 |
|---|---|---|---|---|---|
| PAIR | 549,692 | 35.180 | 13.281 | 44.749 | 53.405 |
| DEALER_DEALER | 423,658 | 27.114 | 133.741 | 161.488 | 165.168 |
| DEALER_ROUTER | 451,353 | 28.887 | 18.283 | 57.127 | 65.064 |
| ROUTER_ROUTER | 396,157 | 25.354 | 19.298 | 56.052 | 69.186 |
| PUBSUB | — | — | — | — | — |

`--pattern ALL` 실행
(`perf_rust_single_linux_20260824_024625_rust-realignment-20260824-r2.txt`)은
**status: partial** (20/25)이고 유일한 미달 사유가 `PUBSUB current tcp 64B:
binary_exit`다. 아래 "미해결 — Core 결함 1"을 참고한다. 나머지 4개 패턴
수치는 위 표와 같은 범위였다(PAIR 552,174 / DD 453,241 / DR 425,303 /
RR 251,697).

### multi (tcp, default patterns, duration 1)

`perf_rust_multi_linux_20260824_024432_rust-realignment-20260824-r2.txt`
— **status: complete** (120/120 result line, success 24 / skip 4 / fail 0)

| pattern | 64B throughput | 64B lat mean | 65536B throughput | 65536B lat mean |
|---|---|---|---|---|
| MULTI_DEALER_DEALER | 443,873 | 0.066 ms | 64,156 | 43.300 ms |
| MULTI_DEALER_ROUTER | 59,994 | 0.832 ms | 19,302 | 2.577 ms |
| MULTI_ROUTER_ROUTER | 54,272 | 0.920 ms | 16,065 | 3.097 ms |
| MULTI_PUBSUB | 1,067,960 | 226.985 ms | 32,376 | 504.049 ms |
| MULTI_STREAM | memory_guard skip | | memory_guard skip | |

MULTI_STREAM 4개 case는 전부 harness의 memory guard가 건너뛰었다
(`clients=6095;max_clients=6089;mem_available_kb=9655000;budget_pct=70`).
11 GB 머신의 환경 제약이지 바인딩 회귀가 아니며, `--pattern MULTI_STREAM
--clients 200 --msg-sizes 256`으로 따로 돌리면 **status: complete**로
15,690 msg/s / lat 12.737 ms가 나온다
(`perf_rust_multi_linux_20260824_024238_rust-stream-probe.txt`).

### perf harness 수정 (bindings/rust 범위 안)

`bindings/rust/perf/run_benchmarks_multi.sh`가 `nofile_guard` /
`memory_guard` skip 사유를 CSV case 파일에 **쉼표를 그대로 둔 채** 기록해서
(`clients=6095,max_clients=6089,...`) report 렌더러가
`ValueError: too many values to unpack (expected 5)`로 죽었다. 다른
case_reason writer들과 동일하게 `${VAR//,/;}`로 치환하도록 고쳤다. 이 버그
때문에 memory guard가 한 번이라도 걸리면 multi report가 아예 생성되지
않았다(pre-existing).

## 미해결 / 소유자 확인 필요

### Core 결함 1 — PUB/XPUB monitor full event mask + socket close 시 reaper SIGSEGV

single PUBSUB perf가 `binary_exit`으로 죽는 원인이다. **순수 C로 재현했다.**

- 재현: PUB(NODROP) + SUB를 tcp로 잇고 두 소켓에
  `zlink_socket_monitor_open(s, &opts)`를 **`opts.events = 0x7FFFF`(full mask)**
  로 연 뒤, 1초 publish 후 sender 스레드에서 `zlink_close(pub)`을 부르면
  `ZLINKbg/Reaper` 스레드가 SIGSEGV로 죽는다(`pthread_mutex_lock`에서 RDI가
  0x80인 쓰레기 포인터).
- `opts`를 `NULL`(Core 기본 mask)로 주면 재현되지 않는다. monitor 핸들러를
  아예 설치하지 않아도 재현되므로 콜백 경로가 아니라 monitor 자체다.
- Rust 바인딩은 `SocketMonitorOpenOptions::default()`가
  `SocketMonitorEventMask::ALL`(0x7FFFF)이라 항상 이 경로를 탄다. perf single
  harness는 모든 패턴에서 monitor로 connection-ready를 기다리는데, 실제로
  터지는 것은 PUB 소켓뿐이다.
- **pre-existing 확인**: HEAD의 Rust 바인딩 + Core 0.12.0 조합에서도 동일하게
  SIGSEGV가 난다. 이번 재정렬과 무관하다.
- 재현 코드: `pubmon.c`(C), `pubrepro`(Rust). 요청하면 첨부한다. core는 이번
  작업 범위 밖이라 수정하지 않았다.

### Core 결함 2 — multipart routed record (작업 중 제3자가 수정함)

작업 초반, `zlink_send_async`에 2-part routed record를 넘기면
- DEALER: completion이 `ZLINK_SEND_TERMINAL` + `EFSM`
  (`lb_t::sendpipe_to`가 `_more`가 서 있는 동안의 호출을 전부 거절)
- ROUTER: `zlink_assert (!_more_out)` (`router_t::xsend_routed`)로 프로세스
  abort

가 났다. 순수 C로 재현했다. 이 때문에 multipart routed record만 exact-target
per-part API로 우회하는 fallback을 넣었었는데, 작업 도중 병렬로 진행된 Core
작업(`doc/perf/perf/bindings-0.12.0/log/2026-08-24-core-send-async-multipart-fix.md`)이
`core/src/runtime/sockets/common/socket_send_complete.cpp`에서
`const bool routed_start = record_->has_target && i == 0;`로 첫 part만
`xsend_routed`를 타도록 고쳤다. 같은 재현 프로그램으로 DEALER/ROUTER 두 경우
모두 `completion result=0`을 확인한 뒤 **fallback을 다시 제거**해서 C++ 참조
구현과 같은 순수 `zlink_send_async` 경로만 남겼다.

주의할 의존 관계 두 가지:
- 이 Core 수정은 아직 commit되지 않은 working-tree 변경이다. 되돌아가면 Rust
  바인딩의 multipart routed send는 다시 abort한다
  (`tests/routed_async_tests.rs::inline_admission_resolves_the_future_on_its_first_poll`이
  2-part record를 보내므로 즉시 드러난다).
- 이번 작업이 `bindings/rust/native/{linux-x86_64,linux-x64}`에 채운 payload는
  `core/build/lib/libzlink.so.0.13.0`(2026-08-24 02:21 빌드, 위 수정 포함)을
  복사한 것이다. Core를 다시 빌드하면 payload도 같이 갱신해야 한다.

### 계약 질문

- send completion의 non-admitted terminal을 Rust에서
  `SubmitResult::NotAdmitted` + Core `terminal_errno`로 매핑했다. C++이
  `submit_result_t::not_admitted`로 매핑한 것과 맞췄지만, 이 매핑을 규정한
  normative 문구는 문서에서 찾지 못했다(C++ 로그의 미해결 질문과 동일).
  `SubmitResult::NotAdmitted`의 기존 doc comment("Rejected by an admission
  policy before sending")는 이 의미와 어긋나므로 소유자 확정이 필요하다.
- STREAM `send(target)` / STREAM `Received::send()`를 async 분류에 넣었다.
  `async-coroutine-policy.ko.md`의 HWM-managed send 정의는 "PAIR send,
  DEALER/ROUTER routed send"만 열거하지만, `Received::send()`가 ROUTER route와
  STREAM route에서 같은 타입을 돌려줘야 하고 `zlink_send_async`가 STREAM을
  지원하므로 STREAM도 같은 lane으로 통일했다. 원하면 STREAM만 동기로 되돌릴
  수 있으나 그 경우 `Received::send()`의 반환 타입이 갈라진다.
- request의 native 제출은 `Future`의 최초 poll에서 일어나고, 그 제출은
  `ZLINK_SEND_FLAGS_NONE`이라 HWM이 차 있으면 Core 안에서 대기한다(C++
  `submit()`과 동일). Rust README의 옛 문구 "poll 중 runtime worker를 점유하지
  않는다"는 이 지점에서 더 이상 성립하지 않는다. Core에 async request 표면이
  없어 대안이 없으므로 문서를 계약에 맞춰 고쳤다.
