# core-rf S-12: `test_close_completion_poller_release` 간헐 실패 수정

- worktree: `~/project/zlink-work/s12` (detached `bc1519e105`), 커밋 없음.
- 분류: **B — Core 기존 결함**. 계약 변경 없음.

## 1. 결과 (수치)

| 항목 | before(수정 전, 같은 worktree) | after |
|---|---:|---:|
| stress 변형(단일 테스트 ×50 + 동시 `ctest -j4 -R 'stream|pipe'`) | **5/50 실패** (계측 빌드에서 6/50, 3/50 재현) | **0/50** |
| `ctest -R close_completion_poller_release --repeat until-fail:20` (동시 부하) | — | 통과 |
| 실패 iteration 소요 | 정확히 1020~1030 ms | 없음 |

## 2. 진단: 브리프의 전제가 틀렸다 — lost wake가 아니다

진단서(diag-close-completion-poller-release.md)는 "close edge를 제3자가 소비하고 re-arm하지
않는다"로 추정했으나, 임시 계측(ring buffer trace: `CLOSEIN`/`BCLOSE`/`BCLOSEBUSY`/`CLOSEBIT`/
`SIGNAL`/`SEND`/`ACT`/`RCV0`/`RCVW`/`DRAIN`/`REARM`/`POLLENT`/`POLLRET`/`ADMOK`/`ADMFAIL`)로
실패 3건을 모두 한 줄로 고정한 결과는 다르다.

실패 순간의 trace(iter 8·15·20 동일):

```
CLOSEIN     tid=closer                  <- zlink_close() 진입
BCLOSE      tid=closer                  <- begin_close_or_fail_busy() 진입
BCLOSEBUSY  tid=closer  inflight=1      <- EBUSY 로 즉시 실패
...
POLLENT     tid=main    timeout=1000
POLLRET     tid=main    rc=0            <- 1001 ms 뒤 timeout
ADMOK       tid=main                    <- closing bit 이 아직도 없다
```

- `signal()`이 보낸 edge를 누가 먹는 문제가 **아니다**. 애초에 `public_api_closing_bit`가
  세워지지 않았고 `CLOSEBIT`/`SIGNAL`이 한 번도 나오지 않는다. `DRAIN`은 이 테스트에서 0회다
  (`async_mailbox_owns_commands()`가 false라 `drain_primary_signaler()`가 호출되지 않는다).
- 실제 원인: **poller의 readiness 샘플이 잡는 public-API in-flight 토큰과 close admission의 경합**.
  `socket_base_api.cpp:894`의 `socket_public_api_scope_t admission (lifecycle_coordinator ())`가
  `enter_public_api()`로 in-flight 카운트를 +1 한다. 그 수 µs 창에 closer 스레드의
  `begin_close_or_fail_busy()`(`socket_lifecycle_runtime.cpp:288`)가 `inflight != 0`을 보고
  **EBUSY로 즉시 실패**한다 → `zlink_close()`가 -1을 반환하고 closing bit도 POLLERR도 영영 없다
  → poller는 1000 ms를 다 자고 0을 반환한다("Expected 1 Was 0").
- 진단서 3.1의 "closer 스레드가 이미 존재하지 않는다"는 관측은 "이미 끝났다"가 아니라
  "EBUSY로 이미 반환하고 사라졌다"였다. `N × 1000 ms`도 poller timeout 그 자체다.

**소비자 확정: 없음(edge 미발행). 확정된 지점은 `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:288`
의 EBUSY 조기 반환이고, 상대 in-flight 토큰의 주인은 `core/src/runtime/sockets/common/socket_base_api.cpp:894`다.**

## 3. 깨지고 있던 계약

`core/doc/spec/core/05-polling.en.md:146-148`:
> It is therefore safe for the application to close a registered socket before removing it from
> the poller. A closed socket source reports `POLLERR` once ...

poller registration이 살아 있는 동안 poller는 주기적으로 `get_events_for_poller()`를 호출하며
매번 in-flight 토큰을 잡는다. 즉 등록된 소켓의 close는 구조적으로 EBUSY를 맞을 수 있었다.
`core/doc/reference/03-socket-lifecycle.en.md:49-51`의 EBUSY는 "another thread의 in-flight
**operation**"이지 라이브러리 내부 readiness 샘플이 아니다.

## 4. 설계 비교

**채택**: `begin_close_or_fail_busy()`가 in-flight를 보면 즉시 EBUSY 하지 말고, 같은 파일에 이미
있는 sync-bit backoff 스케줄(`public_api_sync_backoff`, 한도 `public_api_sync_yield_limit`
= spin 64 + yield 960, sleep 구간 미진입)로 **유한하게 배수(drain)를 기다린 뒤** EBUSY.
- 새 상태·플래그·옵션 0개. 기존 backoff 규칙을 close gate로 확장(중복 없음).
- µs 단위의 poller 샘플은 통과하고, 호출 전체 구간 동안 토큰을 쥐는 호출(blocking recv, endpoint
  teardown — 같은 파일 33-36행 주석 참조)은 창을 넘겨 **여전히 `ZLINK_CLOSE_BUSY`**를 받는다.
  즉 close BUSY 계약이 사라지지 않는다(`test_stream_socket.cpp:2271-2277`의 재시도 루프, 
  `test_public_inproc_multipart_send.cpp:918-921`의 BUSY 허용 분기 모두 유효).

**기각 1 (진단서 4-1안, close edge re-arm)**: 이 실패에는 적용되지 않는다. edge가 소비되는 게
아니라 발행조차 되지 않는다. 넣어도 5/50이 그대로 남는다.

**기각 2 (poller 샘플에 close gate가 세지 않는 별도 admission 카운터)**: `public_api_state`
64비트가 이미 꽉 차 있어(closing 63 / sync 62 / multipart 61 / complete 32-60 / inflight 0-31)
새 atomic 워드가 필요하고, 그러고도 close는 teardown 전에 그 카운터가 비기를 **기다려야** 한다 —
결국 같은 대기에 제어점만 하나 더 늘어난다(POSDDD 위배).

## 5. 변경 파일

- `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp` (+16 −2) — 유일한 변경.
- `core/include/**`·`libzlink.vers`·계약 테스트·문서 변경 없음.

## 6. 실행한 검증

| 검증 | 결과 |
|---|---|
| stress 변형 50회(부하 동시) | 0/50 |
| `--repeat until-fail:20`(부하 동시) | 통과 |
| `ctest -R 'wake|poll|close|release|stream|pipe|mailbox' -j4` ×5 (+확인 1) | 38/38 통과 ×6 |
| lost-wake 세트 `ctest -R wake` ×10 | 5/5 통과 ×10 |
| `ctest -R 'socket_runtime|lifecycle'` ×5 (close gate 단위 테스트 포함) | 3/3 통과 ×5 |
| CLOSE_BUSY 계약 테스트 `stream_socket|public_inproc_multipart_send|timer_poller|flow_state_c_api` ×3 | 9/9 통과 ×3 |
| TSan(`setarch -R`, 새 build-tsan: clang 아님/gcc, LTO OFF, EVENTFD OFF) | `close_completion_poller_release`·`socket_runtime` 2/2 통과, ThreadSanitizer 경고 0건 |
| TSan 전체 `close|wake|socket_runtime` | `test_stream_send_blocking_wakeup`·`test_wake_invariants`·`test_two_poller_wake` 실패 — **수정을 stash 하고 같은 트리에서 재실행해도 동일하게 실패(pre-existing)**. 경고는 `ypipe.hpp:104 check_read()` ×10, `pipe.cpp:1196`, `socket_base_msg.cpp:68` — 모두 이번 변경과 무관한 기존 race. |

성능: 변경 지점은 소켓당 close 1회에만 실행되는 경로이고 send/recv/poll 핫 경로를 건드리지 않는다.
경합이 없으면 추가 원자 연산 0회(첫 로드에서 inflight==0이면 기존과 동일 경로). 별도 벤치 미실행.

## 7. 재확인한 스펙 절

- `core/doc/spec/core/05-polling.en.md` §3·§5: POLLERR one-shot, level 규칙, "closed registered
  socket reports `ZLINK_POLLERR` once", "registration and lifetime pin remain in place until
  removal" — **어느 문장도 다른 동작이 되지 않았다**. 오히려 146-148행이 약속한 동작이 이제
  실제로 지켜진다.
- `core/doc/reference/03-socket-lifecycle.en.md` `zlink_close`: EBUSY는 "another thread의
  in-flight operation"에 대해 그대로 유지된다(유한 대기 후 반환).
- `core/doc/spec/core/03-errors.*` `ZLINK_CLOSE_BUSY` 행: 변경 없음.

## 8. 멈춘 지점

없음. 다만 진단서 `diag-close-completion-poller-release.md` 3.2/4절(“edge를 제3자가 먹는다”,
“close edge re-arm”)은 **오진**이므로 후속 job이 그 가설을 다시 쓰지 않도록 정정이 필요하다.
