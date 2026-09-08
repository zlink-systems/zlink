# MAC-4 — `wip/0.17.4` macOS ARM64 `test_stream_packet_progress` 실패 보고서

- 기준 `wip/0.17.4` (`b27f25dea1`), 브랜치 `wip/mac-4`, worktree `~/project/zlink-work/mac4`.
- 패치: `/home/hep7hep7/project/zlink-work/all-artifacts/MAC-4.patch` (`b27f25dea1` 기준, 진단 workflow 제외). 5개 파일 +49/−1.
- **결과: 원인은 Core 결함이었고 Core에서 고쳤다.** macOS `test_stream_packet_progress` **20/20 통과**, full serial **150/150** + parallel **57/57** 통과. macOS 제외/skip 없음.
- public 헤더(`core/include/**`)·`libzlink.vers`·계약 기대값 **무변경**.

## 1. 실패 문구 확보와 재현율

`build.sh`에 `--output-on-failure`가 없어 문구가 없었으므로 MAC-3의 진단 workflow를 `wip/mac-4`에 올렸다.

| run | 스코프 | 결과 |
|---|---|---|
| 34221148788 | `^test_stream_packet_progress$` until-fail:5 | 1회차 PASS, **2회차 FAIL** |
| 34221976261 | 동일 (계측 추가) | 5/5 PASS — 실패 시에만 출력되므로 데이터 못 얻음 |
| **34222529635** | 동일, until-fail:20 | **1회차 FAIL** — 계측 확보 |
| **34223549078** | 동일, until-fail:20 (수정 적용) | **20/20 PASS** |
| **34224529148** | full (serial + parallel) | **serial 150/150, parallel 57/57, run success** |

실패 문구:

```
test_stream_packet_progress.cpp:360:test_shutdown_during_drain:
  FAIL: receive ignored shutdown until all partial input was drained
DIAG-MAC4 shutdown: 262152 at request, 0 at recv return (rc=0), 0 after join, recv_us=13449
```

ALL-3가 넣은 ping/pong fence와 그 뒤의 `TEST_ASSERT_EQUAL_UINT64(bytes.size()-1, f.pending())`는 **통과했다**.
깨진 것은 마지막 단언 `remaining > 0`이다. fence는 원인이 아니다.

## 2. 원인 (Core 결함)

같은 지점의 Linux 계측과 비교:

| | at request | at recv return | recv 소요 | 소비한 청크 |
|---|---:|---:|---:|---:|
| Linux (dev) | 262152 | 255561 | 402 us | 6,591 |
| macOS (실패) | 262152 | **0** | 13,449 us | **262,152 (전부)** |

청크당 처리 속도는 두 플랫폼이 거의 같다(≈16~19 청크/us). 다른 것은 **shutdown이 수신 측에 보이기까지의 시간**이다.

두 개의 결함이 겹쳐 있었다.

1. **`core/src/runtime/core/ctx_termination.cpp:begin_shutdown_locked()`** 가
   모든 소켓의 `stop_monitor(false)`를 **먼저** 전부 돌리고, 그 다음에야 `stop()`(= `_ctx_terminated` 게시)을 돈다.
   바로 위 주석이 말하듯 "Monitor teardown may wait for its worker or process a peer's mailbox" — 이 테스트는
   모니터를 열어 두므로 그 대기가 macOS에서 ~13 ms다(Linux ~0.4 ms). 그동안 컨텍스트는 아직 "살아 있는" 상태로 보인다.
2. **`core/src/runtime/sockets/common/socket_base_msg.cpp`** 의 블로킹 receive 루프 3곳(`recv_common`, `recv_routed`, 그리고
   그 앞의 동일한 루프)은 `_ctx_terminated`를 **진입 시 한 번만** 확인하고, 루프 안에서는 관리 명령이 도착하기만 기다린다.
   반면 블로킹 send(`socket_message_send_api.cpp:652,750`)는 **매 턴** `is_ctx_terminated()`를 확인한다.
   `socket_base_t::stop()`의 주석("Publish termination before queueing the administrative command … makes both
   blocking paths observe ETERM")이 의도한 대칭이 receive 쪽에만 빠져 있었다.

두 결함의 합: 이미 루프에 들어간 blocking receive는 게시된 종료를 못 보고, 게시 자체도 모니터 teardown 뒤로 밀린다.
그 창이 macOS에서 13 ms이고, STREAM 패킷 펌프는 그 안에 262k개 조각을 전부 소비해 버린다.
Linux는 창이 0.4 ms라 결함이 가려져 있었을 뿐, 플랫폼 조건부 결함이 아니다.

## 3. 수정

| 파일 | 변경 |
|---|---|
| `core/src/runtime/sockets/common/socket_base.hpp/.cpp` | `publish_ctx_terminated()` 추가 — teardown 없이 `_ctx_terminated`만 게시 (private 런타임 헤더) |
| `core/src/runtime/core/ctx_termination.cpp` | `begin_shutdown_locked()`에서 **모든 소켓에 먼저 게시**한 뒤 `stop_monitor()` → `stop()` 순서로 진행 |
| `core/src/runtime/sockets/common/socket_base_msg.cpp` | 블로킹 receive 루프 3곳에서 매 턴 `_ctx_terminated` 확인 → `errno=ETERM; return -1` (블로킹 send와 동일) |
| `core/tests/integration/test_stream_packet_progress.cpp` | `remaining`을 `control.join()` **전에** 읽는다. `zlink_ctx_shutdown()`은 teardown 완료를 기다리지 않으므로, join 뒤에는 조기 반환한 receive에서도 카운터가 0으로 보일 수 있다 |

효과(Linux 계측): `at recv return 262089, recv_us=19` — 정확히 펌프 1라운드(63청크) 뒤 반환. 402 us/6,591청크 → **19 us/63청크**.

### 설계 비교

| 안 | 내용 | 판단 |
|---|---|---|
| A | 테스트의 페이로드를 키워 drain 시간을 늘린다 | 기각. 1바이트 WS 프레임을 수백만 번 쓰는 비용이 비현실적이고, 원인(종료 게시 지연)을 남긴 채 마진만 늘린다 |
| B | 펌프의 64청크 경계를 더 줄인다 | 기각. 처리량을 깎으면서도 "게시가 늦다"는 원인은 그대로다. 새 튜닝 상수를 하나 더 만든다 |
| **C (채택)** | 종료를 teardown보다 먼저 게시하고, blocking receive가 그것을 매 턴 본다 | 이미 존재하는 상태(`_ctx_terminated`)와 이미 존재하는 규칙(블로킹 send의 확인)을 receive에 **대칭으로 적용**할 뿐, 새 제어점·플래그·상수를 만들지 않는다 |

## 4. 계약 확인

- `zlink_ctx_shutdown()` 이후 blocking receive는 ETERM을 반환한다 — 이는 이미 `recv_common`/`recv_routed` **진입부**가 하던 동작이다
  (`if (unlikely (_ctx_terminated)) { errno = ETERM; return -1; }`). 루프 안에서 같은 확인을 하는 것은
  "한 명령 뒤에 시작한 receive"와 "이미 루프에 있는 receive"가 같은 답을 받게 하는 것이고, 새 동작을 만들지 않는다.
- 블로킹 send는 이미 동일하게 동작한다. 이번 변경으로 send/receive의 종료 관측이 일치한다.
- completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake의 순서와 조건은 **어느 문장도 다른 동작이 되지 않았다**.

## 5. 검증

**macOS ARM64** (진단 workflow, `core-macos-test.yml`):

- `test_stream_packet_progress` **20/20 통과** (run 34223549078). 수정 전에는 1~2회 안에 실패.
- full: serial **150/150** (401.2 s), parallel **57/57** (19.3 s) (run 34224529148, conclusion success).

**Linux** (worktree `~/project/zlink-work/mac4`, `core/build-dev` RelWithDebInfo/LTO OFF):

```
ctest --repeat until-fail:5 -R '^test_stream'   → 18/18 통과
ctest -j2 (전체)                                 → 211/212, 실패는 hotpath_gate 뿐
```

`hotpath_gate`는 **이번 변경과 무관**하다. 변경이 없는 `~/project/zlink-work/mac1`의 dev 트리에서 동일하게 실패하며
수치도 사실상 같다(dev·LTO OFF 빌드 대 release 기준값):

| cell | mac4(수정 적용) | mac1(수정 없음) |
|---|---:|---:|
| dealer_dealer_inproc | 1.2784 | 1.2778 |
| pair_inproc | 1.3565 | 1.3534 |
| router_router_tcp | 1.2672 | 1.2670 |
| stream_tcp | 1.1054 | 1.1091 |

즉 dev 빌드에서 gate가 항상 실패하는 기존 성질이며, 성능 판정은 release 빌드 게이트 job의 몫이다.

## 6. 변경 분류

**B — 기존 결함 수정** (Core). 테스트 1줄 변경(측정 시점)은 그 결함을 정확히 관측하기 위한 것이다.
ALL-3의 ping/pong fence와 MAC-3의 `await_input` msleep은 **원인이 아니었다** — 둘 다 실패 시점에 이미 통과한 단계였다.

## 7. 남은 것

없다. 진단 workflow(`.github/workflows/core-macos-test.yml`)는 패치에서 제외했다.
MAC-3 보고서의 권고는 유효하다: macOS를 상시 게이트로 두려면 `core/builds/macos/build.sh`에
`--output-on-failure`를 넣어야 한다(이번에 실패 문구가 없어 재현 workflow를 따로 올려야 했다).
