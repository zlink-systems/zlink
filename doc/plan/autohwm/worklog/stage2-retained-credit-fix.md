# Stage 2: retained-credit lease NULL 결함 수정 (2026-08-22)

`core-byte-hwm-flow-control-plan.ko.md` §3.2 "retained receive의 public 계약을
삭제하거나 변경하지 않는다"에 대한 실행 증거다. 계약을 바꾸지 않고 구현이 계약을
지키도록 고쳤다.

- Branch: `codex/bindings-0.11.1-performance` (전환 없음, push 없음)
- 보호 경로(`core/doc/spec/`, `bindings/doc/spec/`, `core/doc/internals/`) 변경 없음
- Public header / ABI 변경 없음, test 변경 없음

## 1. 증상

`core/build-tests`에서 `test_retained_hwm_credit`가 20/20 재현 실패했다.

```text
core/tests/integration/test_retained_hwm_credit.cpp:280:
  test_router_synthetic_frame_is_unleased_and_typed_payloads_are_leased:
  FAIL: Expected Non-NULL
```

Line 280은 ROUTER multipart 수신의 `TEST_ASSERT_NOT_NULL (lease)`다.
`zlink_router_recv_part_v2_with_hwm_budget_lease()`가 첫 payload frame(`i == 0`)에
대해 NULL lease를 돌려줬다. 원본 사용자 worktree(`447f41a9f2`)에서도 같은 실패가
나므로 최근 작업 이전부터 존재한 결함이다.

## 2. 근본 원인

`core/src/runtime/sockets/router/router_recv_path.cpp:409`
(`zlink::router_t::xrecv_routed_with_credit()`, prefetch가 없는 경로)

```c++
    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (msg_, &pipe);          // <-- retained 아님

    // Routing-id frames are Core-owned metadata ...
    while (rc == 0 && msg_->is_routing_id ())
        rc = token_out_ ? _fq.recvpipe_retained (msg_, &pipe, token_out_)
                        : _fq.recvpipe (msg_, &pipe);
```

Retained 여부가 뒤집혀 있었다. 첫 fetch가 항상 non-retained
`fq_t::recvpipe()`이고, retained fetch는 **routing-id frame을 건너뛰는 loop
안에서만** 일어난다. `fq_t::recvpipe()`는 message를 꺼내면서 byte credit을 즉시
queue에 돌려주므로 caller에게 넘길 retained origin이 만들어지지 않는다. 실제
wire stream에는 message당 routing-id frame이 없어서(peer identity는 handshake
때 pipe에 붙는다) loop body는 보통 한 번도 실행되지 않고, `token_out_`은 빈 채로
반환된다. 그 빈 token이 `recv_router_message_direct()`의 `credits_out_[0]` →
`stage_recv_sequence()`의 `buffered_credits[0]` →
`retained_token_to_public_lease()`까지 전달되고, 거기서 `token_->empty()`가 참이라
`*lease_out_ = NULL`로 정상 종료(rc 0)한다. 즉 실패가 error가 아니라 조용히
"lease 없음"으로 나타났다. 이 경로는 첫 frame이 routing-id frame인 경우에만
우연히 동작했다.

같은 socket family의 다른 retained 경로는 모두 첫 fetch부터 retained 형태를
쓴다: `router_t::xrecv_with_credit()`
(`router_recv_path.cpp:250`), `stream_t::xrecv_with_credit()`
(`core/src/runtime/sockets/stream/stream.cpp:627`), `xsub_t` 경로
(`core/src/runtime/sockets/pubsub/xsub.cpp:349,371`). `xrecv_routed_with_credit()`
하나만 예외였고, 이것이 ROUTER 계열 typed recv API만 lease를 잃던 이유다.

증상 분포도 이 원인과 일치한다. 같은 test 안에서
`zlink_recv_with_hwm_budget_lease(router, ...)`(→ `xrecv_with_credit`)는 lease를
정상 반환했고, multipart 두 번째 frame(→ `recv_retained` follow-up)도 정상이었다.
오직 `recv_routed_retained()`를 타는 첫 frame만 NULL이었다.

## 3. 수정

`core/src/runtime/sockets/router/router_recv_path.cpp` 한 곳만 바꿨다.

```c++
    pipe_t *pipe = NULL;

    // Every fetch must use the retained form when a public HWM lease was
    // requested, otherwise the credit of the caller-visible frame is returned
    // to the queue on receive and the caller ends up with an empty token.
    // Routing-id frames are Core-owned metadata and are never exposed to the
    // caller: recvpipe_retained() resets the token before each fetch, so the
    // credit of a skipped routing-id frame is released as soon as the next
    // frame is pulled, leaving only the caller-visible frame retained.
    int rc = token_out_ ? _fq.recvpipe_retained (msg_, &pipe, token_out_)
                        : _fq.recvpipe (msg_, &pipe);
    while (rc == 0 && msg_->is_routing_id ())
        rc = token_out_ ? _fq.recvpipe_retained (msg_, &pipe, token_out_)
                        : _fq.recvpipe (msg_, &pipe);
```

근거와 안전성:

- `fq_t::recvpipe_retained()`(`core/src/runtime/sockets/internal/fq.cpp:166`)는
  fetch 전에 `token_out_->reset ()`을 호출한다. 따라서 routing-id frame을
  건너뛰며 반복해도 앞 frame의 credit은 다음 fetch 시점에 반환되고, loop 종료
  시 token은 caller-visible frame 하나의 credit만 보유한다. 주석이 약속하던
  동작이 이제 실제로 성립한다.
- `token_out_ == NULL`(lease를 요구하지 않는 기존 recv 경로)일 때는 이전과
  완전히 동일하게 `_fq.recvpipe()`를 호출한다. Hot path 동작·비용 불변.
- 실패 경로도 안전하다. `recvpipe_retained()`는 진입 시 reset하므로 `rc != 0`로
  빠져나갈 때 token은 비어 있다.
- Public API/ABI, header, retained receive의 public 계약(§3.2) 모두 불변.
  Test는 수정하지 않았다.

Diff 요약: 1 file changed, 파일 하나에서 첫 `_fq.recvpipe()` 호출을
`token_out_` 분기 형태로 바꾸고 주석을 실제 동작에 맞게 다시 썼다.

## 4. Test 결과

모두 `core/build-tests`에서 `cmake --build core/build-tests --parallel 8` 후 실행했다.

Focused 8개 + `unittest_poller` + `test_timer_poller`:

```text
ctest --test-dir core/build-tests --output-on-failure \
  -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options|test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm|unittest_poller|test_timer_poller)$'

100% tests passed, 0 tests failed out of 10   (57.02 s)
```

`test_router_mandatory_hwm` standalone 5회: **5/5 pass** (기존 batch-load flake
재현 없음).

`test_retained_hwm_credit`: 대상 case
`test_router_synthetic_frame_is_unleased_and_typed_payloads_are_leased`는 수정
후 **20/20 pass**(수정 전 0/20). Test binary 전체는 standalone 10회 중 6회
pass다. 남은 4회 실패는 **다른 case의 기존 flake**이며 이번 수정과 무관하다
(§5).

## 5. 남은 별개 결함 (이번 범위 밖, 수정하지 않음)

```text
core/tests/integration/test_retained_hwm_credit.cpp:137:
  test_retained_pair_preserves_total_and_releases_credit_cross_thread:
  FAIL: Expected 0 Was 1088
```

- 수정 **전** baseline(같은 파일을 `git stash`로 되돌려 재build) standalone 10회:
  line 137 실패 4/10.
- 수정 **후** standalone 10회: line 137 실패 4/10.

빈도가 동일하므로 이번 변경과 인과가 없다. 성격도 다르다. Line 137은
`released.current_accounted_bytes == 0`을 lease release 직후에 요구한다.
`ctx_physical_queue_registry_t::release_retained_origin()`
(`core/src/runtime/core/ctx_physical_queue_registry.cpp:989`)에서 application
lease는 `publish_credit_inline`이 거짓이라 `pipe_->schedule_retained_credit()`
(비동기 command)로 credit을 돌려준다. 반면 `current_accounted_bytes`는
`ctx_auto_hwm_recalc.cpp:231`에서 socket별 pipe 회계를 sampling한 값이라, io
thread가 그 command를 처리하기 전에 snapshot을 읽으면 아직 1088(payload 1024 +
회계 overhead)이 남아 보인다. 같은 test의 line 133~136(registry atomic 기반
`application_accounted_bytes`, lease count, deferred bytes)은 동기적으로 갱신되어
항상 통과한다.

즉 registry atomic과 pipe-local sampling 사이의 publish 지연을 test가 동기라고
가정한 race다. 동기화하려면 cross-thread pipe 회계 갱신 방식을 바꿔야 하므로
이번 수정 범위를 넘는다. 별도 항목으로 남긴다.

## 6. Perf 확인

`cmake --build core/build --parallel 8`로 perf runtime을 먼저 재build했다.
Paired 1회, `ROUTER_ROUTER_SENDSEND / tcp / 256 B / --runs 1`.

Runtime provenance(report META):

- local: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.11.1`
  (core_source=local, core_version=0.11.1)
- release: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/lib/libzlink.so.0.10.1`
  (core_source=release, core_version=0.10.1)

두 report 모두 clients=100, duration_seconds=5, ctx_auto_hwm_profile=balanced로
동일하다.

| Tag | Throughput (ops/s) | 비고 |
| --- | --- | --- |
| `autohwm-retained-fix-local` | 163,431.6 | 기준 median 158.5 K |
| `autohwm-retained-fix-release-0101` | 191,076.2 | 기준 median 177.0 K |

Local/release 비 0.855 (기준 158.5/177.0 = 0.896). Local 절대값은 기준 median
위이고 비율 차이는 1회 측정 noise 범위다. **측정 가능한 비용 없음.** 수정이
`token_out_ != NULL`인 retained API 경로에만 적용되고 benchmark가 쓰는 일반 recv
경로(`token_out_ == NULL`)의 명령 흐름은 변하지 않으므로 구조적으로도 기대에
부합한다.

첫 시도 pair(11:16)는 직전 test 실행으로 load average가 10을 넘은 상태였고
local 105.2 K / release 19.4 K로 양쪽 모두 기준에서 크게 벗어났다. Load가 1
미만으로 내려간 뒤(11:20) 다시 측정한 위 값을 채택했다. 두 결과 모두 report에
남아 있다.

## 7. 정리

- 진단용 임시 logging은 추가하지 않았다(코드 읽기와 기존 test 출력만으로 특정).
- `gmon.out`은 손대지 않았다(untracked 상태 그대로).
