# RR-1 보고서 — D-BP43 (1,025-part reply의 count-1 completion drain 해제 assertion)

- 대상 worktree: `~/project/zlink-work/rr1` (detached, `wip/0.17.3-all2` = 0.17.4 베이스, HEAD `1a79625d3d`)
- 비교 트리: `~/project/zlink-work/rr1-0173` (`core/v0.17.3` = 결함 보고 기준, 조사용으로만 사용)
- patch: `~/project/zlink-work/all-artifacts/RR-1.patch` (untracked 새 테스트 포함)
- `git diff --stat HEAD -- core/include core/src/libzlink.vers` → **비어 있음**
- 변경 분류: **B(기존 결함) — 단, 0.17.4 베이스에서 이미 수정되어 있었다.** 이 job의 산출물은 원인 확정 + 경계 회귀 테스트.

## 1. 재현 표

| # | 재현 주체 | 트리 | 조건 | 결과 |
|---|---|---|---|---|
| 1 | Rust `ownership_tests::request_future_preserves_more_than_1024_reply_parts` | 0.17.3 | inproc, 1,025-part reply | **SIGABRT** `Assertion failed: released (socket_base_api.cpp:1641)` — 30/30회 결정적 |
| 2 | 같은 Rust 테스트 | **0.17.4 베이스(rr1)** | 동일 | **PASS** (1회 + 반복) |
| 3 | 신규 C 통합 테스트 `test_request_reply_part_count_boundary` (inproc, N=1,023/1,024/1,025/2,048 × pull NONE·DONTWAIT·POLLCOMPLETION poller, 순차/동시 reply) | 0.17.3 | — | PASS (재현 못 함) |
| 4 | 같은 C 테스트 | 0.17.4 베이스 | — | PASS (`--repeat until-fail:20`) |
| 5 | C 테스트 tcp 변형(N 4종 × DONTWAIT·동시 blocking) | 0.17.3 / 0.17.4 | — | 양쪽 PASS |

> Rust binding 빌드 비용은 **크지 않았다**(`~/.rustup/toolchains/stable` cargo 1.97 + `ZLINK_CORE_SOURCE=local`, 약 5 초). 브리프가 허용한 "C 테스트로 대체"를 쓰지 않고 원본 Rust 테스트를 두 트리에서 직접 돌려 판정했다.
>
> 재현 명령(0.17.3 트리 기준):
> ```
> PATH=$HOME/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:$PATH \
> ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR=<tree>/core/include \
> ZLINK_CORE_LIB_DIR=<tree>/core/build-dev/lib LD_LIBRARY_PATH=<tree>/core/build-dev/lib \
> cargo test --test ownership_tests request_future_preserves_more_than_1024_reply_parts -- --test-threads=1
> ```

## 2. 원인 (file:line)

계측(`process_completion_pipe` 반환값·`_count1_completion_ready_state`·ypipe `front/_r/_c`·head 프레임 hex 덤프)으로 확정한 순서:

```
RR1 drain#1
RR1 pcp head=3            (pipe_head_reply)
RR1 deliver parts=1025 seq=1
RR1 head-data delim=1 ...  (다음 head 프레임 = delimiter, type 0x67 = type_delimiter)
RR1 drain pass result=1 state=2   (public_head, state=draining → release 성공, state=idle)
RR1 drain#2 state=0
RR1 pcp head=1            (같은 delimiter를 다시 pipe_head_data 로 분류)
RR1 drain pass result=1 state=0   (public_head, state=idle → release 실패)
Assertion failed: released
```

원인 두 조각:

| 조각 | 위치(0.17.3) | 내용 |
|---|---|---|
| **A. 실제 결함 — claim 없는 2차 drain** | `core/src/runtime/sockets/common/socket_base_api.cpp:1694-1700` (`drain_claimed_completion_pipe`) | `entry.owns_lease()`일 때 `drain(false)`를 **무조건 한 번 더** 돌린다. 1차 drain은 `completion_pipe_public_head`/`terminated`/`budget` 어느 출구에서도 count-1 claim을 **이미 release**하고 나온다. 그래서 2차 drain이 같은 public-head 분기(`:1637-1641`)에 다시 들어가면 상태가 `count1_completion_idle`이라 `release_count1_completion_drain`이 false를 돌려주고 `zlink_assert (released)`(:1641)가 터진다. |
| **B. 그 분기를 두 번 타게 만드는 입력** | `core/src/runtime/core/pipe.cpp:49-73` `probe_normalized_head()` (0.17.4에서는 `core/src/runtime/core/pipe_receive.cpp:34-73`) | reply record 뒤에 남은 **delimiter 프레임**(peer ROUTER close가 count-1 Application FIFO에 쓴다)을 `pipe_head_data`로 분류한다. 이 분류 자체는 설계대로다 — 공개 receive 경로(`check_read()` → `consume_if_delimiter` → `process_delimiter()`)가 delimiter를 소비하도록 `xread_activated`를 걸어야 하기 때문이다. 다만 그 결과 drain이 "public head"로 두 번 연속 빠져나오게 되고, 그때 A의 2차 drain이 assertion을 친다. |

**1,024는 계약 상한이 아니라 타이밍 상수다.** Rust 테스트에서 서버 thread는 `reply.submit()` 직후 `router`를 drop(close)한다. reply part가 1,025개면 requester의 reactor thread가 drain을 시작하는 시점에 reply record와 close delimiter가 **둘 다** 이미 FIFO에 실려 있어 위 순서가 결정적으로 성립한다. 1,024 이하에서는 delimiter가 늦게 도착해 2차 drain이 `pipe_head_empty`를 보고 조용히 끝난다.

## 3. 계약 대조

| 스펙 절 | 확인한 내용 | 판정 |
|---|---|---|
| `core/doc/spec/core/socket/README.ko.md` (completion pull, §REQUEST/REPLY) | reply의 **part 개수 상한 조항이 없다**. 상한은 전부 byte 단위(`ZLINK_OPT_MAXMSGSIZE`, completion reservation 상한 → `ZLINK_SUBMIT_OUT_OF_MEMORY`/`ENOMEM`)다. | 1,024-part 상한은 계약에 없음 |
| `core/doc/spec/core/protocol/01-zmp.ko.md` §7, §537-538 | "Application part별·record 합산 상한이 없는 무제한 값", CONTROL body만 고정 4096 byte 상한. | part 수 상한 없음 |
| `core/doc/spec/core/05-polling.ko.md` (owner 규칙) | completion drain owner는 claim한 pipe에 대해서만 drain을 수행한다. 0.17.3의 무조건 2차 `drain(false)`는 claim을 놓은 뒤 다시 도는 것이라 이 규칙 위반. | A는 스펙 위반, 구현 결함 |

→ 계약상 1,025-part reply는 **정상적으로 한 개의 REQUEST completion으로 전달되어야** 한다. 송신 측 오류 반환·분할 같은 계약 변경은 불필요하다.

## 4. 수정

**0.17.4 베이스(이 worktree의 HEAD)에는 이미 수정이 들어가 있다.** ALL-2가 completion drain / owner gate를 lifecycle turn으로 통합하면서 `drain_claimed_completion_pipe`의 lambda + `owns_lease()` 분기 + 무조건 2차 `drain(false)`를 통째로 없애고, `socket_receive_entry_scope_t entry (receive);` 하나 아래에서 **단일 while 루프**로 만들었다(`core/src/runtime/sockets/common/socket_base_api.cpp:1615-1700`). claim을 놓은 뒤 다시 도는 경로가 사라졌으므로 조각 A가 제거되었고, 재현 표 #2가 이를 확인한다.

따라서 이 job에서 Core 소스를 추가로 고치지 않았다. 설계 비교:

| 안 | 내용 | 판단 |
|---|---|---|
| (1) 채택 — 소스 변경 없음 + 경계 회귀 테스트 추가 | 원인 A는 이미 제거됨. 새 제어점·플래그·상태를 늘리지 않는다(POSDDD). | **채택** |
| (2) `probe_normalized_head()`에 delimiter 전용 head kind 추가 | head kind와 그에 딸린 분기 규칙이 하나 늘어난다. delimiter는 공개 receive 경로가 이미 정상 소비하므로 얻는 것이 없다. | 기각(규칙 수 증가) |

추가 파일:

| 파일 | 내용 |
|---|---|
| `core/tests/integration/test_request_reply_part_count_boundary.cpp` (신규, 317줄) | DEALER↔ROUTER, REQUEST 1-part → REPLY N-part(N = 1,023/1,024/1,025/2,048), inproc + tcp, completion pull 3형태(blocking NONE / DONTWAIT / POLLCOMPLETION poller), reply 순차 제출과 별도 thread 동시 제출. 모든 조합에서 completion 1건·part 수·part 내용이 정확히 일치하는지 검증. |
| `core/tests/CMakeLists.txt` | 위 타깃 등록 + `TIMEOUT 300`. |

이 C 테스트는 결함 자체(0.17.3)를 재현하지는 못한다(재현 표 #3). 재현에는 `owns_lease()` 경로 + close delimiter가 drain 진입 시점에 이미 큐에 있어야 하는데, C 공개 API만으로 그 순서를 결정적으로 만들지 못했다. 대신 이 테스트는 **"reply part 수에 상한이 없다"는 계약**을 1,024 경계에서 고정한다. 결함 자체의 회귀 감시는 Rust `ownership_tests`가 맡는다(0.17.4에서 통과하므로 캠페인에서 제외할 필요가 없어졌다 — 결함 보고서의 "이 테스트만 제외 중" 상태를 해제할 수 있다).

## 5. 검증

| 항목 | 명령 | 결과 |
|---|---|---|
| 신규 테스트 반복 | `ctest --test-dir core/build-dev -R '^test_request_reply_part_count_boundary$' --repeat until-fail:20` | **20/20 PASS** (170 s) |
| 관련 suite | `ctest -R 'request\|reply\|completion\|router\|dealer\|poll' --repeat until-fail:3` | **62 tests × 3회 전부 PASS** (322 s) |
| 원본 Rust 재현 | 위 §1 #2 | **PASS** (0.17.3에서는 30/30 SIGABRT) |
| 공개 인터페이스 | `git diff --stat HEAD -- core/include core/src/libzlink.vers` | **비어 있음** |
| TSan(새 테스트 + `unittest_phase3_request_reply_owners`), ASan(새 테스트) | — | **미실행 — 2시간 상한에 걸렸다.** 원인 규명(계측 빌드 반복)에 시간을 썼다. Core 소스 변경이 0이고 추가분이 테스트 파일뿐이라 sanitizer 위험은 낮지만, 게이트 job에서 새 타깃을 TSan/ASan 트리에 한 번 태워 주기 바란다. |

## 6. 멈춘 지점 / 남은 것

1. TSan·ASan 미실행(위 표).
2. 조사용 worktree `~/project/zlink-work/rr1-0173`을 만들었다가 제거했다. 재현이 필요하면 `git worktree add --detach <경로> core/v0.17.3` 후 §1의 cargo 명령이면 된다.
3. D-BP43은 0.17.4에서 닫아도 된다고 판단한다. 다만 §2-B의 delimiter → `pipe_head_data` 분류는 남아 있으므로, count-1 Application lane에서 peer close 직후 drain이 public-head로 한 번 빠져나오는 동작은 그대로다(계약 위반은 아님, `xread_activated` 후 공개 receive가 delimiter를 소비한다).
