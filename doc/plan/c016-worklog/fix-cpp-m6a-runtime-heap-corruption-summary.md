# M6A runtime heap corruption (`double free or corruption (out)`) — 진단

## 요약

`ctest --test-dir build -L framework-unit` 전체 실행에서 `test_cpp_framework_m6a_runtime`가
간헐적으로 glibc `double free or corruption (out)`로 abort하는 문제. 단독 실행/대부분의
전체 실행에서는 통과하므로 flake처럼 보이지만 **실제 heap 손상(메모리 안전 결함)**이며,
원인은 **Core의 XPUB/`dist_t` pipe lifecycle**에 있다. Framework는 public binding API만
사용한다. 규칙에 따라 **core/\*\* 를 수정하지 않고 STOP**, 공개 C-API 재현과 원인을 보고한다.

- 분류: **B (기존 결함) — 소유 계층 = Core**. Framework/fixture 변경 없음.
- 재현: M6A ×30 (normal build) 통과(fail=0)했으나 gdb 루프 + CPU 부하에서 14/40, 5/60회 재현.
  ASan(Core instrumented) M6A 루프에서 8회째에 확정 stack 확보. **공개 C-API 단독 재현**
  (`pub_churn.cpp`) 200 프로세스 중 55번째에서 **동일 stack** 재현.

## ASan stack (원인 확정)

`AddressSanitizer: heap-buffer-overflow ... WRITE of size 8`
(`doc/plan/c016-worklog/asan-core-iter-8.log`, 단독 재현은 `churn-proc-55.log` — 동일)

```
#0 zlink::array_t<zlink::pipe_t, 2>::erase(unsigned long)      core/src/runtime/utils/array.hpp:89
#1 zlink::array_t<zlink::pipe_t, 2>::erase(zlink::pipe_t*)      core/src/runtime/utils/array.hpp:72
#2 zlink::dist_t::pipe_terminated(zlink::pipe_t*)              core/src/runtime/sockets/internal/dist.cpp:113
#3 zlink::xpub_t::xpipe_terminated(zlink::pipe_t*)             core/src/runtime/sockets/pubsub/xpub.cpp:312
#4 zlink::socket_base_t::pipe_terminated(zlink::pipe_t*)       core/src/runtime/sockets/common/socket_base_api.cpp:1881
#5 zlink::pipe_t::process_pipe_term_ack()                     core/src/runtime/core/pipe.cpp:3028
#6 zlink::object_t::process_command(...)                      core/src/runtime/core/object.cpp:145
   ... socket_base_t::in_event() → reaper_mailbox_handler()  (thread "ZLINKbg/Reaper")
```

할당 지점: `dist_t::attach → array_t::push_back`(`dist.cpp:35`)로 만든 `vector<pipe_t*>`
(capacity 2 = 16 byte). ASan은 **버퍼 8 byte 앞**에 대한 WRITE를 보고한다.

M6A에서 abort하는 프로세스의 non-ASan 코어 덤프에서 `_dist`는
`_pipes = vector length 0 (capacity 2), _active = 1, _eligible = 1, _matching = 0`
— **_items는 비었는데 active/eligible 카운터는 1로 남아 있는 불일치** 상태였다.

## 원인 (file:line)

**한 사실(“pipe가 dist에 붙어 있는가”)을 두 경로가 서로 다르게 판단**한다.

1. `socket_base_t::attach_pipe` — **core/src/runtime/sockets/common/socket_base_api.cpp:273-274**
   ```cpp
   if (pipe_->has_completed_termination ())
       return;                     // ← xattach_pipe()/dist_t::attach() 이전에 조기 return
   ```
   attach 명령을 처리하는 시점에 peer(SUB)가 이미 pipe를 종료시켜 `_lifetime.terminal()`이
   참이면, PUB은 **`dist_t::attach()`를 건너뛴다**. 이 pipe의 `array_item_t::_array_index`는
   기본값 `-1` 그대로 남는다.

2. `socket_base_t::pipe_terminated` — **core/src/runtime/sockets/common/socket_base_api.cpp:1877-1883**
   PUB은 비-paired 소켓(pair_id==0)이라 `application_attached`가 항상 참이므로,
   위와 같은 pipe에 대해서도 term-ack 처리 시 무조건 `xpipe_terminated(pipe_)`를 호출한다.

3. `xpub_t::xpipe_terminated`(**xpub.cpp:312**) → `dist_t::pipe_terminated`(**dist.cpp:113**) →
   `array_t::erase(pipe_)`. 여기서 `_array_index == -1`이므로:
   - `_pipes.index(pipe_)`가 `(size_type)-1`을 돌려주고, 이는 `_matching/_active/_eligible`보다
     크므로 세 카운터의 감소가 모두 skip된다(→ 덤프의 `active=1/eligible=1` 잔존 불일치).
   - `array_t::erase((size_type)-1)`(**array.hpp:89**)가 `_items[(size_type)-1] = _items.back()`을
     실행 → 버퍼 base − 8 byte에 8 byte WRITE = **heap-buffer-overflow / heap 손상**. 이후 임의의
     free에서 `double free or corruption (out)`로 표면화된다.

즉, **attach는 건너뛰고 terminate는 실행하는 비대칭** 때문에 dist에 등록된 적 없는 pipe가
`dist_t::pipe_terminated`에 도달한다. `dist_t::has_pipe()`(dist.cpp:43)라는 멤버십 확인이
이미 존재하지만 이 경로에서 사용되지 않는다.

## 완료 보고 4줄

- **소유 계층**: Core — XPUB/`dist_t` pipe lifecycle
  (`socket_base_api.cpp` attach/terminate 대칭, `dist.cpp`, `utils/array.hpp`). Framework 아님.
- **spec 조항**: connection 선택·교체·reconnect·teardown(pipe 종료)은 Core 소유
  (framework/AGENTS.md “소유 계층 확인이 먼저다”, `core/doc/spec/core/socket/` PUB/XPUB·README RID 정책).
  Framework는 logical handshake·descriptor·liveness만 소유하고 물리 pipe 상태를 갖지 않는다.
- **교차언어 대조**: 네이티브 Core 결함이므로 언어 무관 — 동일 Core를 쓰는 C++/.NET/Java/
  Kotlin/Node 모든 binding의 PUB(XPUB) 소켓이 같은 조건(연결 즉시 종료가 attach와 race)에서 영향.
  Framework 계층엔 언어별 차이가 없다.
- **변경 분류**: **B (기존 결함, Core)**. Framework·fixture·spec 변경 없음. 규칙 위반(C/D) 아님.

## 규칙 수 (수정 전/후 — Core 제안 기준)

- 수정 전: **2**. “pipe가 dist에 붙는가”라는 한 사실을 attach(조기 return 분기)와 terminate(무조건
  호출)가 서로 다른 규칙으로 판단 → 비대칭.
- 수정 후: **1**. “dist에 attach된 pipe만 dist에서 terminate한다”는 단일 불변식으로 통일.
  (제안 방향 — Core 담당자 판단: `xpub_t::xpipe_terminated`/`dist_t::pipe_terminated` 진입 전
  `_dist.has_pipe(pipe_)`로 게이트하거나, `attach_pipe`의 조기 return 이후에도 attach/terminate
  회계를 대칭으로 맞춘다. 특수 분기·잔존 카운터가 사라져 규칙이 줄어든다.)
  **Framework 변경 규칙 수 = 0 (변경 없음).**

## 공개 C-API 재현 (core를 고치지 않고 원인 증명)

`doc/plan/c016-worklog/pub_churn.cpp` — 오직 `zlink_socket/bind/connect/close/set_option/
get_option/publish_part/ctx_term` 만 사용.

- 하나의 PUB(tcp bind)를 유지한 채 별도 thread가 `zlink_publish_part(..., DONTWAIT)` 로 계속 발행.
- 다수 SUB를 반복 connect 후 **즉시 close** → SUB pipe 종료가 PUB의 attach 처리와 race.
- 빌드/실행(ASan Core):
  ```
  g++ -std=c++17 -g -O1 -fsanitize=address -fno-omit-frame-pointer -pthread \
    -I<zlink-core>/include pub_churn.cpp -o pub_churn \
    -L core/build-asan/lib -lzlink -Wl,-rpath,core/build-asan/lib
  ASAN_OPTIONS=detect_leaks=0 ./pub_churn 300 4    # 부하 하 200 프로세스 중 재현
  ```
- 결과: `array.hpp:89 dist_t::pipe_terminated` heap-buffer-overflow, **M6A와 동일 stack**
  (`churn-proc-55.log`).

## 실행/검증 결과

- M6A ×30 (normal `build/`, CPU 16-loop 부하): **fail=0** (통과) — 간헐성 확인.
- M6A gdb 루프(normal build): 40회 중 14회째 SIGABRT, 60회 중 5회째 SIGABRT — 재현 확정.
- M6A ASan 루프(Core instrumented `core/build-asan`, framework `build-asan`, CPU 부하):
  8회째에 heap-buffer-overflow 확정 stack.
- 공개 C-API 단독 재현(`pub_churn`): 200 프로세스 중 55회째에 동일 stack.
- ASan Core 빌드: `core/build-asan`(`ENABLE_ASAN=ON`, Debug, shared)로 별도 생성.
  ASan framework 빌드: `framework/languages/cpp/build-asan`(Debug + `-fsanitize=address`).
  **`build/`는 손대지 않음.**

## BLOCKERS / 후속

- **원인이 Core(core/\*\*)에 있어 이번 지시 범위에서 수정하지 않음(STOP).** 규칙에 따른 완료 조건
  중 “M6A ASan 0 errors / full framework-unit ASan 0 errors / owner 수정 후 회귀” 항목은
  **Core 담당자의 수정 이후**에만 검증 가능하다. 제안 수정 방향은 위 “규칙 수” 참고
  (`dist_t::has_pipe` 게이트 또는 attach/terminate 회계 대칭화).
- 재현은 부하·타이밍 의존적이지만 `pub_churn`으로 안정적으로 표면화된다. Core 수정 후
  `pub_churn`을 회귀 재현(부하 하 다프로세스 무-hit)으로 사용할 수 있다.
- 정리: `core/build-asan`, `framework/languages/cpp/build-asan`는 진단용 임시 빌드로 남아 있다
  (원본 `build/`와 무관). 필요 시 삭제 가능.
