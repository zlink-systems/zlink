# MAC-2 — macOS ARM64 잔여 5건 실패 수정 보고서

- 브랜치: `wip/mac-1` (base `wip/0.17.3-all2`), worktree `~/project/zlink-work/mac1`. MAC-1 수정 유지.
- 패치: `/home/hep7hep7/project/zlink-work/all-artifacts/MAC-2.patch` (`wip/0.17.3-all2` 기준, cherry-pick한 main 커밋과 진단 워크플로 제외 = MAC-1 + MAC-2 누적).
- 커밋: `657c9bac60`(진단, 임시) → `8b9a88b2c8`(수정 3건) → `fa7ab9a356`(wake_invariants saturation settle + 임시 진단 제거) → `77a2f81db6`(BUSY 프로브 spin 제거).

## 0. 가장 중요한 사실 정정

MAC-1 보고서의 “5건은 단독 실행 시 모두 PASS, full serial run에서만 재현” 은 **틀렸다**.
좁은 regex로 5개만 `--repeat until-fail:5` 돌린 run **34198394530**에서 **5개 전부 실패**했다(총 332.9 s).
즉 테스트 순서·시스템 자원(TIME_WAIT·포트 고갈) 가설은 모두 기각되고, 5건은 각각 독립적인 **테스트 쪽 Linux 속도 가정** 문제였다.
이 정정 덕분에 재현 비용이 full serial 700 s → 12 s 수준으로 떨어져 반복 검증이 가능해졌다.

## 1. 테스트별 근본 원인과 수정

| 테스트 | 근본 원인 (file:line) | 왜 macOS에서만 | 수정 |
|---|---|---|---|
| `test_single_lane_wire_mandatory_count`, `test_single_lane_wire_old_peer_rejected` | `core/tests/integration/test_dealer_router_single_lane_contract.cpp:321` `wait_for_raw_close()` 가 **1바이트 recv → `msleep(10)`** 루프. 관찰자 처리량이 **100 B/s** 로 고정된다. 라우터가 보내는 HELLO(27 B) + ERROR(42 B) = 69 B 를 다 빼기 전에 자신의 3 s 마감이 끝난다 | Linux는 `usleep(10 ms)`가 거의 10 ms라 69 B를 ~0.7 s에 소진하고 EOF를 본다. macOS 러너에서는 반복당 실측 ~49 ms(3 s에 61 B) → EOF에 도달 불가 | 매 반복마다 수신 큐 전체(256 B 버퍼)를 비우고, **비었을 때만** `msleep(10)`. 진단 hex는 유지(실패 시에만 출력) |
| `test_ctx_options:657` `test_auto_hwm_applied_limit_blocks_and_resumes_after_drain` (및 `run_auto_hwm_public_blocking_send`) | `core/tests/integration/test_ctx_options.cpp` drain 루프가 **레코드당 `blocked_send.wait_for(20 ms)`**. LWM 크레딧 에지에 닿기까지 `queued × 20 ms` 를 쓰는데, 블로킹 send의 기본 `sndtimeo`는 `options.cpp:115` 기준 **1000 ms** | Linux에서는 절반 드레인이 1 s 안에 들어가지만, 3배 느린 러너에서는 초과 → 블로킹 send가 EAGAIN(errno 35)으로 복귀 | 루프 내 20 ms 정지를 제거하고 리더 속도로 드레인. 마지막 `wait_for(1 s)` 재개 단언은 그대로. 실패 시 `queued/drained/drain_ms` 를 출력 |
| `test_xpub_nodrop:243` | `core/tests/integration/test_xpub_nodrop.cpp` 구독자 스레드가 `ZLINK_DONTWAIT` + EAGAIN마다 `msleep(1)`. NODROP 발행자는 구독자가 돌려주는 크레딧으로만 전진하는데 `SNDTIMEO=200 ms` | 리더 지연이 러너 속도만큼 늘어나 한 번의 `zlink_publish`가 200 ms를 넘김 → send 스레드가 `-(1000+EAGAIN)`으로 조기 종료, recv 스레드는 영원히 대기 → 15 s 마감에서 `sent == recv` 로 보인다 | 구독자에 `ZLINK_OPT_RCVTIMEO=50 ms`를 주고 블로킹 `zlink_subscribe`로 대기. 타이머가 아니라 **소켓 이벤트**를 관찰한다. 실패 메시지에 `send_ready/recv_ready/send_result` 추가 |
| `test_wake_invariants:569 → 1299/1327` `test_multi_dealer_dealer_tcp_large_hwm_drain_wakes_all_pollout` | fill 루프가 클라이언트마다 **첫 EAGAIN에서 멈춘다**(`test_wake_invariants.cpp:889~`). 첫 EAGAIN은 “송신자 자기 큐가 SNDHWM(1 MiB=16레코드)에 찼다”만 증명할 뿐, 그 아래 TCP 버퍼와 **서버 인바운드 파이프(RCVHWM 1 MiB)** 에는 여유가 남아 있다. io 스레드가 그 여유로 레코드를 밀어내면 LWM을 지나 **리더 크레딧 없이 WRITABLE 토큰이 발행**된다 | 러너가 느려 fill이 오래 걸리는 동안 io 스레드가 하류를 비울 시간이 생긴다. Linux CI에서는 fill이 io 스레드보다 빨라 그 창이 닫혀 있다 | fill 루프에 **saturation settle 단계** 추가: 전원이 backpressure된 시점에 조기 WRITABLE이 있으면 소비하고 그 클라이언트의 backpressure를 해제해 계속 채운다. 아무도 스스로 크레딧을 되찾지 않을 때만 `saturated`. 기대값·토큰 규칙(`backpressure_attempts[i]==1`, `writable_tokens[i]!=0`)은 그대로 유지된다. 추가로 `wait_until_poller_wait_is_active()`의 `yield()` 스핀을 `msleep(1)`로 바꿔 BUSY 프로브가 waiter 스레드를 굶기지 않게 했다 |

`test_wake_invariants`는 run 34198394530에서는 `assert_no_ready_completion`(kind=3 WRITABLE, id=1)으로,
run 34200082673에서는 `waiters_blocked=0`(100개 waiter 전부 arming 후 **72 ms** 안에 깨어남 — drain은 2017 ms 소요)으로 나타났다.
증상은 둘이지만 원인은 하나다: **drain 전에 이미 크레딧이 돌아와 있었다.**

## 2. macOS run id (전/후)

| run | 스코프 | 결과 |
|---|---|---|
| 34196588768 | MAC-1 최종, full | serial **5 실패** / parallel 0 실패, serial 716.0 s |
| **34198394530** | 5개 테스트만, `--repeat until-fail:5` + TCP 상태 샘플러 | **5/5 실패** (332.9 s) — 단독에서도 재현됨을 확정, 진단 데이터 확보 |
| **34200082673** | 5개 테스트만 (수정 3건 적용) | **4/5 통과** (11.99 s). 남은 1건 `test_wake_invariants` (`waiters_blocked=0`) |
| **34201309177** | full serial + parallel (수정 4건 전부) | serial **148/149 통과** (391.6 s, MAC-1 716.0 s에서 단축) — 남은 1건 `test_wake_invariants`(`waiters_blocked=0`). parallel 55/56 — `unittest_flow_state_monitor` Timeout(신규, `-j3` 부하 플레이크로 보임) |
| **34202650122** | full serial + parallel (`77a2f81db6`, BUSY 프로브 수정 포함) | **보고서 작성 시점에 실행 중** — 결과 미확인 |

## 3. Linux 검증

worktree `~/project/zlink-work/mac1`, `core/build-dev`(RelWithDebInfo, LTO OFF, 테스트 ON):

```
ctest --test-dir core/build-dev -j1 --repeat until-fail:3 \
  -R '^(test_ctx_options|test_xpub_nodrop|test_single_lane_wire_mandatory_count|test_single_lane_wire_old_peer_rejected|test_wake_invariants)$'
→ 100% tests passed, 0 tests failed out of 5   (3회 반복 전부)
```

`test_wake_invariants`는 Linux에서 33.3 s → 32.1 s로 사실상 동일하다(settle 단계가 Linux에서는 거의 즉시 0회 재충전으로 끝난다).

## 4. 변경 분류와 설계 비교

전부 **C — 우회가 아니라 B/C 경계의 테스트 결함 수정**이다. Core 런타임 코드는 **한 줄도 바꾸지 않았다**.
공개 인터페이스(`core/include/**`, `libzlink.vers`), 계약 기대값, 옵션·플래그, 계약 동작(completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake의 순서와 조건) 어느 것도 바꾸지 않았다.
네 테스트의 **단언과 기대값은 모두 그대로**이고, 바뀐 것은 관찰 방법(드레인 처리량, 리더 대기 방식)과 전제 조건 도달 방법(포화 확인)뿐이다.

설계 비교(각 항목 두 안 중 선택):

| 문제 | 안 A (택하지 않음) | 안 B (채택) | 이유 |
|---|---|---|---|
| raw close 관측 | 마감 3 s → 10 s로 확대 | 반복당 수신 큐 전체 드레인 | 마감 확대는 원인(100 B/s 관찰자)을 남긴 채 증상만 가린다. 브리프의 “timeout inflation 금지”에도 맞지 않는다 |
| auto-HWM 블로킹 send | 테스트에서 `SNDTIMEO` 상향 | 드레인 루프의 20 ms 정지 제거 | SNDTIMEO 상향은 “기본값 1000 ms 안에 깨어난다”는 검증 자체를 약화시킨다 |
| NODROP publish | `SNDTIMEO` 200 → 1000 ms | 구독자를 `RCVTIMEO` 블로킹 수신으로 | 타이머 대신 이벤트를 관찰한다. 발행자 계약(200 ms 안 재개)은 그대로 검증된다 |
| wake invariants | `backpressure_attempts >= 1` 로 기대 완화 | fill을 실제 포화까지 진행 | 기대 완화는 계약 테스트 기대값 변경이라 금지 항목이다. settle 단계는 기대를 건드리지 않고 전제 조건에만 도달한다 |

## 5. 남은 실패와 상태

**run 34201309177 (커밋 `fa7ab9a356`, serial 148/149):**

`test_wake_invariants`의 saturation settle 단계는 **의도대로 동작했다**. 실패 상세를 보면

```
saturated=1 all_backpressured=1 backpressured_clients=100 one_wait_token_per_client=1
accepted_total=1796 accepted_min=15 accepted_max=31
waiter_armed=1 waiters_blocked=0
max_wait_ms=2018 drain_elapsed_ms=2018 recovery_after_drain_ms=2
level_ready_clients=100 writable_completions=100 retried_clients=100
delivered_total=1896 expected_delivery_total=1896 no_extra_delivery=1 pollers_closed=1
```

- `accepted_max`가 16 → 31로 늘었다 = 이제 체인 전체(송신 큐 + TCP + 피어 큐)가 실제로 포화된다.
- `max_wait_ms=2018 == drain_elapsed_ms` = **100개 waiter 전부가 drain 내내 블로킹 대기했다**(이전 run은 72 ms). 조기 WRITABLE 문제는 사라졌다.
- 조기 completion 검사, level POLLOUT, WRITABLE 토큰, 재전송, 배달 총계 모두 100/100 통과.

남은 것은 `waiters_blocked=0` 하나뿐이고, 이는 **관찰자 쪽 문제**다.
`test_wake_invariants.cpp:369` `wait_until_poller_wait_is_active()`가 `zlink_poller_size`가 BUSY를 돌려줄 때까지 `std::this_thread::yield()`로 스핀한다. 100개 waiter 스레드가 블로킹 대기에 진입하려는 동안 3코어 호스트에서 프로브가 코어를 점유해 **기다리는 대상 스레드를 굶긴다**. `max_wait_ms=2018`이 “waiter는 실제로 블로킹되어 있었다”를 증명하므로 프로브만 못 본 것이다.

**수정(커밋 `77a2f81db6`)**: 프로브 루프의 `yield()`를 `msleep(1)`로 교체. Linux `--repeat until-fail:3` 통과. macOS 확인은 run **34202650122**에서 진행 중이며 이 보고서 작성 시점에는 결과가 나오지 않았다 — 상한 3.5 h에 도달해 여기서 멈춘다.

**parallel 그룹**: run 34201309177에서 `unittest_flow_state_monitor`가 Timeout 1건. MAC-1 최종(34196588768)에서는 parallel 0 실패였으므로 `-j3` 부하 플레이크로 보인다. MAC-1이 APPLE에서 TIMEOUT을 3배로 스케일했음에도 걸린 것이라 추가 조사가 필요하다(이번 job에서는 손대지 않았다).

**최후 수단 제외 정규식**(적용하지 않았다). run 34202650122에서 `test_wake_invariants`가 여전히 실패할 경우에만:

```
ZLINK_CTEST_EXCLUDE_REGEX='^(test_wake_invariants)$'
```

parallel 쪽까지 막아야 한다면:

```
ZLINK_CTEST_EXCLUDE_REGEX='^(test_wake_invariants|unittest_flow_state_monitor)$'
```

## 6. 부수 사항

- `.github/workflows/core-macos-test.yml`에 시스템 정보 + TCP 상태 샘플러 단계를 추가했다(진단용). TIME_WAIT/포트 고갈 가설은 §0에서 기각되었으므로 이 단계는 최종 병합 대상이 아니다.
- MAC-1의 임시 진단(`core/tests/testutil_unity.cpp`의 per-test 타이밍 출력, `DIAG-MAC1` 문자열)은 모두 제거했다. 실패 시에만 출력되는 세 개의 진단 메시지는 중립적인 문구로 남겼다(원인 재조사 비용을 크게 줄여준다).
- 조사 중 확인했지만 **원인이 아니었던 것**: macOS는 `eventfd`가 없어 `signaler_t`가 socketpair(신호당 1바이트)를 쓰므로 “1회 read로 카운터 전체 소진”이라는 eventfd 의미와 다르다. 그러나 블로킹 send/recv의 대기는 signaler가 아니라 `mailbox_t::wait_for_command_signal`(pthread CV + `_command_wait_epoch`)이 담당하므로 이 차이는 이번 실패와 무관하다. `condition_variable_t`도 양 플랫폼 모두 `stl11`을 고른다. 잘못된 READY 거부 경로도 정상 동작했다 — 드레인한 hex에서 라우터의 ERROR 프레임(`...05 7f 1f "transport pair metadata invalid"`)을 확인했다.
