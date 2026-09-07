# G-2 — `msg_t` 생명주기 비용 축소 + R10 묶음 A(join/leave 사멸 개념 제거)

worktree `~/project/zlink-work/g2` (detached `f67625990a`). 커밋하지 않았다.

## 1. 결과 (수치)

### 1.1 hotpath 5셀 — 결정적(instruction count) 비교, 같은 트리에서 before/after

`ctest -R hotpath_gate`(dev 트리)는 타이밍이 아니라 명령어 수를 세므로 노이즈가 없다.
같은 워크트리에서 pristine `f67625990a`를 빌드해 잰 값이 before다.

| 셀 | before (Ir/msg) | after (Ir/msg) | 변화 |
|---|---:|---:|---:|
| `dealer_dealer_inproc` | 4,341.50 | 4,241.17 | **−100.3 (−2.31 %)** |
| `dealer_router_reqrep_inproc` | 23,676.75 | 23,312.47 | **−364.3 (−1.54 %)** |
| `pair_inproc` | 3,289.89 | 3,082.43 | **−207.5 (−6.31 %)** |
| `router_router_tcp` | 3,714.62 | 3,651.95 | **−62.7 (−1.69 %)** |
| `stream_tcp` | 16,410.97 | 16,074.63 | **−336.3 (−2.05 %)** |

5셀 모두 감소. G-A 예상(300–700 Ir/msg)의 하단~중간.

> `hotpath_gate` 자체는 before·after **양쪽 모두 FAIL**한다(ratio 1.10–1.30). 기준값이 dev(LTO OFF)
> 트리 기준이 아니어서 생기는 **기존 실패**이며 이 job이 만든 것이 아니다. 실제로 이 job은 5셀 전부의
> ratio를 낮췄다(예: `pair_inproc` 1.3015 → 1.2194).

### 1.2 축소 callgrind 셀 — `msg_t` 호출 횟수/self-Ir 표 (single ROUTER_ROUTER tcp 1024, 차분법 12 s−4 s)

G-A와 같은 방법(`--tool=callgrind --cache-sim=no`, `PERF_IO_THREADS=1`, 12 s/4 s 두 판의 차분).
**before는 G-A 원본이 아니라 같은 머신·같은 셸 환경에서 pristine `f67625990a`를 다시 잰 값**을 쓴다
(G-A 판은 환경변수 크기가 달라 하네스 `getenv`가 1,116 vs 1,262 Ir/msg로 어긋난다).

| 심볼 | before 호출/msg | before Ir/msg | before Ir/call | after 호출/msg | after Ir/msg | after Ir/call |
|---|---:|---:|---:|---:|---:|---:|
| `size()` | 20.98 | 359.9 | 17.2 | **0 (인라인됨)** | — | — |
| `data()` | 7.00 | 121.8 | 17.4 | **0 (인라인됨)** | — | — |
| `check()` | 6.02 | 54.2 | 9.0 | **0 (인라인됨)** | — | — |
| `init()` | 33.04 | 264.3 | 8.0 | 8.07 | 64.6 | 8.0 |
| `init(void*,size,ffn,hint,content)` | (호출자에 인라인) | — | — | 1.88 | 75.0 | 40.0 |
| `init_size()` | 2.14 | 62.7 | 29.4 | 2.14 | 58.6 | 27.4 |
| `init_external_storage()` | 0.87 | 22.7 | 26.0 | (인라인됨) | — | — |
| `close()` | 28.05 | 732.8 | 26.1 | 28.12 | 734.6 | 26.1 |
| `move()` | 11.00 | 417.9 | 38.0 | 11.02 | 407.8 | 37.0 |
| `copy()` | 2.00 | 78.0 | 39.0 | 2.00 | 78.1 | 39.0 |
| **`msg_t` self Ir/msg 합** | | **2,114** | | | **1,419** | |

셀 전체(하네스 `getenv` 제외): before 15,155 → after 14,832 / 15,002 (2판) = **−153 ~ −323 Ir/msg**.
같은 구성 2판의 편차가 170 Ir/msg이므로 이 셀만으로는 유의성이 약하다 — **판정 근거는 §1.1의 hotpath 5셀**이다.

### 1.3 "각 호출이 필요한가" — 브리프 질문에 대한 답

- **`size()`가 1.68 %인 이유 / 인라인 안 되던 이유**: 본문은 4-way switch로 짧지만 `zlink_assert (check ())`가
  매크로라 `fprintf` + `fflush` + `zlink_abort` 3문장이 **호출부마다 통째로 전개**되어 인라인 예산을 넘긴다.
  Release+LTO에서도 아웃라인 사본이 남아 20.98회/msg × 17.2 Ir을 냈다. 냉각 경로를 아웃라인
  `report_invalid()`(noreturn)로 빼고 `always_inline`을 붙이자 셋 다 심볼에서 사라졌다.
- **같은 msg를 두 번 init/close 하는가**: 아니다. `init()` 33.04회/msg는 중복이 아니라 **서로 다른 메시지
  슬롯 33개**(part 배열, staged RID 사본, 인코더/디코더 프레임)를 각각 1회씩 초기화한 것이다.
  중복은 `init()` 안에 있었다 — `invalidate()`가 signature 4 B와 type을 0으로 쓴 직후
  `mark_valid()`가 같은 자리에 다시 쓴다. 실패 경로가 없는 init 계열에서는 순수 낭비다.
- **빈 msg 초기화 후 즉시 덮어쓰기**: `move()`의 `src_.init ()`(11회/msg)는 스펙이 요구하는 관측 가능한
  동작이라 유지했다(§3 인용).

## 2. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/core/msg.hpp` | join/leave 선언·enum 제거; `init()`를 헤더 인라인으로 이동(+ 중복 `invalidate()` 제거); `check()/size()/data()`에 `ZLINK_MSG_INLINE`; `report_invalid()` 선언 |
| `core/src/runtime/core/msg.cpp` | `init_join/init_leave/is_join/is_leave` 정의 삭제; 아웃라인 `init()` 삭제; `init_size/init_data/init_delimiter/init_external_storage`의 선두 `invalidate()`를 **실패 경로로만** 이동; `report_invalid()` 정의 |
| `core/src/runtime/core/pipe.cpp` | **3줄만** — `is_join()/is_leave()` 항상-false 검사 제거 (2524, 3591, 3608행). G-1이 만지는 락 스코프에는 손대지 않았다 |
| `core/tests/unittest/unittest_pipe_byte_charge.cpp` | `test_delimiter_join_and_leave_charge_metadata_only` → `test_delimiter_charges_metadata_only` |

## 3. join/leave — 독자 0건 확인

전 저장소(`core/`, `bindings/`, `framework/`, `bench/`, 테스트 포함) grep 결과:

- `init_join`/`init_leave`/`type_join`/`type_leave`: 생산자·소비자가 **`msg.cpp` 자신과
  `unittest_pipe_byte_charge.cpp` 뿐**. 후자는 공개 계약 테스트가 아니라 unit 테스트이고,
  "join/leave 프레임은 metadata만 과금한다"는 **도달 불가능한 분기**를 검증하고 있었다.
- `is_join`/`is_leave`: `pipe.cpp` 3곳. 값을 만드는 생산자가 0이므로 **항상 false**.
- 공개 헤더: `ZLINK_JOIN`/`ZLINK_LEAVE`/`zlink_join`/`zlink_leave` **0건**. `libzlink.vers` 무변경.
- `02-message` 스펙 전문에 join/leave/radio/dish/group 개념 **없음**.
- `type_max`가 107 → 105로 좁아진다. 106/107 타입 메시지를 만들 수 있는 유일한 생산자가
  사라졌으므로 `check()`의 판정 결과는 모든 도달 가능한 상태에서 동일하다.

## 4. 설계 비교와 선택 이유

**`size()/data()`를 싸게 만드는 두 방법.**

- (A) `zlink_assert (check ())`를 **제거**한다. 가장 싸다(호출당 ~6 Ir 추가 절감). 그러나 이는 내부
  불변식 감시망을 없애는 것이고, 공개 경로는 이미 `checked_message()`가 EFAULT를 내므로 남은 것은
  **Core 내부 버그를 즉시 abort로 드러내는 안전망**이다. 그 안전망을 성능 때문에 지우는 것은
  "규칙을 줄인다"가 아니라 "보증을 줄인다"다.
- (B) **검사는 그대로 두고 냉각 경로만 아웃라인**한다(채택). `report_invalid()`(noreturn)가 같은 문구를
  같은 `zlink_abort`로 넘기므로 abort 동작·메시지가 동일하고, 뜨거운 본문이 인라인 가능한 크기로
  줄어든다. 새 옵션·플래그·상태를 하나도 추가하지 않는다(매크로 `ZLINK_MSG_INLINE` 1개는
  제어점이 아니라 컴파일러 힌트다).

(B)를 골랐다. 측정으로도 (B)만으로 `size/data/check` 심볼이 셋 다 사라졌다.

**`init()`의 `invalidate()`.** "선(先)무효화 후 재유효화"는 실패할 수 있는 init에만 의미가 있다.
`init()/init_delimiter()/init_external_storage()`와 `init_size()/init_data()`의 vsm·cmsg 분기는
실패 경로가 없어 signature를 지웠다 그대로 다시 쓴다. malloc이 실패할 수 있는 두 분기에만
`invalidate()`를 남겨 "실패하면 메시지는 무효"라는 성질을 그대로 지켰다.
**규칙을 하나 없앤 것이지 추가한 것이 아니다**(POSDDD).

## 5. 실행한 테스트와 남은 실패

- `ctest --test-dir core/build-dev -R 'msg|message|part|stream|router|dealer|pubsub|pipe'` — **5회 전부 50/50 통과**.
- `hotpath_gate` — FAIL이지만 **pristine `f67625990a`에서도 같은 FAIL**(§1.1). 이 job의 회귀가 아니며,
  5셀 ratio는 모두 개선됐다.
- TSan 트리 미실행: 이 워크트리에 `core/build-tsan`이 없고, `pipe.cpp` 변경이 **항상 false인 술어 3개 제거**로
  락·순서·상태를 전혀 건드리지 않아 새 경합 표면이 없다. 감독관 게이트에서 돌리기를 권한다.
- 전체 ctest는 공통 규칙대로 돌리지 않았다.

## 6. 성능 표

§1.1(hotpath 5셀, 결정적)과 §1.2(callgrind RR 셀) 참조. with_stream 처리량은 이 job의 검증 항목이
아니어서 재지 않았다(브리프 검증 항목 = `ctest`, 축소셀 Ir/msg, hotpath 5셀).

## 7. 재확인한 스펙 절 — 어느 문장도 다른 동작이 되지 않았다

`core/doc/spec/core/02-message.en.md`:

- §2 "A message proceeds through the **initialize → use → close** lifecycle. Every message must be
  ... closed exactly once with `zlink_msg_close`. After it is closed, the ..." — `close()`는 손대지 않았다.
  `init()`은 헤더로 옮겼을 뿐 쓰는 필드·최종 상태가 바이트 단위로 동일하다.
- §2 "For a zero-copy message, the callback is the ownership boundary." / "the library ... invokes a
  callback so the caller can release the buffer" — `init_data`/`init_external_storage`의 ffn·hint 저장과
  `close()`의 ffn 호출 경로 무변경.
- §3 표: "`zlink_msg_copy` | Lightweight copy | The two messages share the buffer for large/zero-copy
  storage, while a small inline message is copied by value." — `copy()` 무변경(Ir/call 39.0 동일).
- §3 "`zlink_msg_copy()` atomically increments the count, and `zlink_msg_close()` atomically decrements
  it. It is therefore safe to copy or close ..." — refcnt 경로 무변경.
- §6 "if a handle is `NULL` or a message is invalid (uninitialized or already closed), the function sets
  `errno == EFAULT`" + 표(`zlink_msg_data` → `NULL`, `zlink_msg_size` → `0`) — 이 판정은
  `message_api.cpp`의 `checked_message()`가 하며 손대지 않았다. `size()/data()` 안의 검사는
  **그 뒤에 오는 내부 불변식 assert**이고, 조건식·abort 동작 모두 그대로다.
- `zlink_msg_move` "After a successful move, `src_` becomes an empty message equivalent to a freshly
  initialized message" — `move()`의 `src_.init ()` 유지, `init()`의 결과 상태 동일.
- `zlink_msg_adopt` "`src_` becomes an empty initialized message" — 동일.

ABI: `sizeof (msg_t) == 64`, `max_vsm_size == 29`, `trivially_copyable`, `message_auxiliary_t == 16 B`,
`request_reply.sequence` offset 8 — 모든 `static_assert` 그대로 통과. `_u` 유니언 레이아웃 무변경.
`core/include/**`·`libzlink.vers` 무변경.

## 8. 변경 분류

**B 기존 결함**(join/leave: 생산자가 사라진 뒤 남은 죽은 개념과 항상-false 분기) + **C 우회 없음의
순수 내부 구현 개선**(인라인·중복 store 제거). 계약 적응(A)도 spec gap(D)도 없다.

## 9. 사고 보고 — 공유 stash 충돌 (감독관 확인 요망)

`git stash`는 워크트리 사이에 **공유되는 `refs/stash` 한 스택**이다. hotpath before를 재기 위해
10:24에 stash 했는데, 그 사이 job **G-1**이 같은 스택에 `stash push`/`pop`을 했다:

- 내 stash(msg.hpp/msg.cpp/pipe.cpp/unittest 4파일)를 **G-1이 pop 해 자기 워크트리로 가져갔을
  가능성이 매우 높다**. G-1 워크트리에 `core/src/runtime/core/msg.*` 변경이 섞여 있으면 이 job 것이다.
- 나는 G-1의 stash(`c8f0bd51`, "g1": `ctx_physical_queue_registry.cpp`,
  `socket_base_dispatch.cpp`, `socket_runtime.hpp`)를 실수로 pop 했다. **즉시 `git stash store`로
  같은 커밋을 스택에 되돌려 놓았고**(현재 `stash@{0}: On (no branch): g1`), 내 워크트리의 그 3파일은
  `git checkout --`으로 되돌렸다.
- 내 변경은 stash 커밋 `f5eb99f305`에서 패치로 복구했고, 복구 후 dev 재빌드 + ctest 50/50 통과를 확인했다.

**권고: 이 저장소의 job 브리프에 "워크트리에서 `git stash` 금지"를 추가**한다.

## 10. 멈춘 지점

없다. 다만 §1.2의 callgrind 셀 전체 수치는 같은 구성 2판 편차가 ±170 Ir/msg라 이 크기의 변경을
판정하기에 분해능이 부족했다. `hotpath_gate`의 명령어 카운트가 이 급의 변경에는 훨씬 나은 계측기이며,
후속 G-1/G-3/G-4도 이쪽을 before/after 판정 기준으로 쓰기를 권한다.
