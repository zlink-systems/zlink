# MAC-3 — macOS ARM64 마지막 실패(`test_wake_invariants`) 종결 보고서

- 브랜치 `wip/mac-1` (base `wip/0.17.3-all2`), worktree `~/project/zlink-work/mac1`. MAC-1·MAC-2 수정 유지.
- 패치: `/home/hep7hep7/project/zlink-work/all-artifacts/MAC-3.patch` (`wip/0.17.3-all2` 기준, cherry-pick한 main 커밋과 진단 워크플로 제외 = MAC-1+2+3 누적).
- **결과: macOS ARM64 ctest 전부 통과. serial 149/149, parallel 57/57. 연속 2회 재현.** 제외 정규식은 쓰지 않았다.
- Core 런타임 코드(`core/src/**`, `core/include/**`)는 **한 줄도 바꾸지 않았다.** Darwin 전용 가드도 추가하지 않았다 — 아래 §2에서 보듯 Core는 계약대로 동작했고, 잘못된 것은 테스트의 관측·전제 도달 방식이었다.

## 1. macOS run 결과

| run | 스코프 | serial | parallel | 비고 |
|---|---|---|---|---|
| 34202650122 | full (MAC-2 마지막) | 148/149 — `test_wake_invariants` FAIL | 56/57 — `unittest_flow_state_monitor` Timeout | MAC-3 시작 시점 |
| 34204737552 | `^test_wake_invariants$`, until-fail:3 | 1회차 PASS, 2회차 FAIL(**증상 전환**) | (매칭 0) | settle 이벤트화 적용 |
| 34205855167 | `^test_wake_invariants$`, until-fail:3 | **3/3 PASS** (3.47/3.71/3.65 s) | (매칭 0) | 얕은 수신 큐 적용 |
| 34206451147 | full | 148/149 — `test_stream_packet_progress` FAIL | **57/57 PASS** | wake 통과, 신규 실패 노출 |
| 34207768327 | full | 148/149 — 같은 실패 | 56/57 — `unittest_asio_transport_writev_lifetime` FAIL | msleep 수정으로 해결 안 됨 |
| 34209046724 | 위 두 테스트, until-fail:3 | 3/3 PASS | 3/3 PASS | 단독 재현 실패 → 부하 의존 확인 |
| **34209700208** | full (TCP 샘플러 제거) | **149/149 PASS** (388.5 s) | **57/57 PASS** (21.5 s) | 워크플로 Summary 스텝 버그로 run만 red |
| **34210988899** | full (최종) | **149/149 PASS** (394.7 s) | **57/57 PASS** (20.0 s) | **run conclusion success** |

## 2. `test_wake_invariants` — 실제 원인과 수정

MAC-2가 남긴 진단(`waiters_blocked=0`, `max_wait_ms == drain_elapsed_ms`)은 "프로브가 굶었다"로 읽혔지만,
BUSY 프로브를 `msleep(1)`로 바꾼 run 34202650122의 수치는 다른 그림을 보여줬다:

```
saturated=1 accepted_max=26 waiter_armed=1 waiters_blocked=0
max_wait_ms=44 drain_elapsed_ms=2023
```

`max_wait_ms=44` — **waiter가 실제로 44 ms에 깨어났다.** 프로브가 못 본 게 아니라 관측 대상이 이미 끝나 있었다.
즉 남은 문제는 그대로 "drain 전에 크레딧이 돌아온다"였다.

### 2.1 조기 크레딧 — settle 판정을 1회 샘플에서 이벤트로

MAC-2의 settle은 전원이 backpressure된 시점에 100개 클라이언트의 completion 큐를 `DONTWAIT`로 **한 번씩** 훑어
아무것도 없으면 `saturated`로 확정했다. 그러나 그 순간에도 io 스레드는 하류로 바이트를 밀고 있어서,
방금 훑고 지나간 클라이언트가 1 ms 뒤 크레딧을 되찾는다. 샘플링으로는 "체인이 멈췄다"를 판정할 수 없다.

수정: 전원 backpressure 상태에서 `zlink_poll(fill_items, ZLINK_POLLOUT, 1000 ms)`로 **기다린다**.
누군가 크레딧을 회복하면 poll이 즉시 깨어나고(그 클라이언트만 WRITABLE 소비 후 계속 채움),
1 s 동안 아무도 깨우지 않으면 그것이 "체인이 멈췄다"의 정의다. (`3178bfde63`)

### 2.2 저수지 — drain과 그 drain이 유발할 크레딧의 분리

2.1 적용 후 증상이 바뀌었다(run 34204737552 2회차):

```
waiters_blocked=1  accepted_min=30 accepted_max=39
drain_elapsed_ms=5  max_wait_ms=5978  recovery_after_drain_ms=5973
```

`waiters_blocked=1` — 원래 단언은 해결됐다. 대신 마지막 단언 `recovery_after_drain_ms < wake_timeout_ms(2000)`가 깨졌다.

Linux 로컬 계측(임시 `DIAG-MAC3` 출력)과 비교하면 구조가 드러난다:

| | accepted_min/max | drain_elapsed_ms | max_wait_ms | recovery_after_drain_ms |
|---|---|---|---|---|
| Linux (dev, 3코어) | 16 / 16 | 15910 | 12358 | **0** |
| macOS (settle 이벤트화만) | 30 / 39 | **5** | 5978 | **5973** |

- Linux에서는 서버 인바운드 큐가 채워지지 않아(정확히 SNDHWM 16레코드에서 멈춤) drain이 **전송로에서 끌어오는** 작업이 되고,
  15.9 s 걸리는 그 drain 도중에 크레딧이 도착한다 → `recovery_after_drain_ms=0`.
- macOS에서는 settle이 서버 인바운드 큐(RCVHWM 1 MiB × 100)까지 가득 채운다. 그러면 drain은 **메모리에서 5 ms 만에** 끝나고,
  그 뒤에야 51 MB(= LWM 8레코드 × 64 KiB × 100)가 전송로를 건너야 송신자 크레딧이 돌아온다 → 6 s.

즉 **서버가 미리 버퍼링한 양(저수지)이 drain을, 그 drain이 유발해야 할 크레딧에서 분리한다.**
저수지가 깊을수록 "리더가 비웠다"와 "송신자가 깨어난다" 사이가 벌어진다. macOS/Linux 차이는
러너 속도가 아니라 fill 단계에서 저수지가 얼마나 채워지느냐였다.

수정: 서버의 `RCVHWM`을 **2레코드(128 KiB)** 로 낮춘다(`configure_shallow_receive`). drain이 wire에서 끌어오게 되어
모든 플랫폼에서 "wake는 그것을 유발한 drain을 따라온다"가 성립한다. (`c98290dd19`)

이것은 **픽스처 설정**이지 계약 기대값이 아니다. 단언·기대값·토큰 규칙(`backpressure_attempts[i]==1`,
`writable_tokens[i]!=0`, level POLLOUT, WRITABLE 1건, 재전송, 배달 총계)은 **한 줄도 바꾸지 않았다.**
송신자 쪽 `SNDHWM`(large hwm)은 그대로다 — 이 테스트가 검증하는 계약은 송신자의 HWM/EAGAIN/WRITABLE이다.

## 3. 그 과정에서 드러난 나머지 2건

wake가 통과하자 그 뒤에 가려져 있던 실패 2건이 드러났다(둘 다 full run에서만, 단독 3/3 통과).

| 테스트 | 원인 (file:line) | 수정 |
|---|---|---|
| `unittest_asio_transport_writev_lifetime:165` `test_tcp_pending_writev_releases_completion_after_close` (`Expected 0 Was 1`) | `unittest_asio_transport_writev_lifetime.cpp:69 fill_send_buffer()`가 **첫 EAGAIN에서 멈춘다**. EAGAIN은 그 순간 버퍼가 찼다는 뜻일 뿐, 스택은 계속 피어로 바이트를 넘기고 있어 곧 2바이트가 들어갈 공간이 생긴다 → 픽스처가 pending으로 남기려던 2바이트 writev가 인라인 완료 | EAGAIN 뒤 `poll(POLLOUT, 200 ms)`가 **timeout될 때까지** 계속 채운다. 소켓이 계속 unwritable일 때만 "찼다" |
| `test_stream_packet_progress:84` `test_shutdown_during_drain` (`transport did not queue all fragments`) | `await_input()`이 `zlink_monitor_status`를 `yield()` 스핀으로 폴링. 매 폴이 모니터 상태 락을 잡아 262k개 1바이트 WS 프래그먼트를 흡수해야 하는 io 스레드와 경합 | 폴 간격을 `sleep_for(1 ms)`로. 실패 시 `queued/expected/last_progress_ms`를 출력하도록 메시지 보강 |

`test_stream_packet_progress`는 `msleep` 수정만으로는 run 34207768327에서 다시 실패했고,
워크플로의 **백그라운드 TCP 샘플러(5 s마다 `netstat -an -p tcp` 전체 스캔)** 를 제거한 뒤 두 번의 full run에서 모두 통과했다.
3코어 러너에서 이 진단 루프 자체가 부하였다. 샘플러는 MAC-2가 넣은 진단이고 병합 대상이 아니었으므로 제거했다.

또한 워크플로 Summary 스텝이 `for f in ...LastTestsFailed*.log; do [ -f "$f" ] && ...; done` 형태라
**실패 로그가 없을 때(=전부 통과) 종료코드 1**을 내 run을 red로 만들었다. `if`문 + `exit 0`으로 고쳤다.

`unittest_flow_state_monitor` Timeout은 main의 `550f0e3f6e`를 cherry-pick(`507f5f8d42`)해 해결됐다(parallel 57/57).

## 4. 설계 비교

| 문제 | 안 A (택하지 않음) | 안 B (채택) | 이유 |
|---|---|---|---|
| settle 판정 | 샘플 패스를 N회 반복해 연속 0회면 확정 | `zlink_poll(POLLOUT, quiet)` 대기 | N회 반복도 결국 샘플링이다. poll은 "누가 크레딧을 회복했다"는 **이벤트 자체**를 기다리고, timeout이 곧 "아무 일도 없었다"의 증거다. 규칙(반복 횟수)을 하나 늘리는 대신 기존 관측 수단을 쓴다 |
| drain 후 wake 지연 | `wake_timeout_ms`를 2 s → 8 s로 확대 | 서버 `RCVHWM`을 2레코드로 축소 | 마감 확대는 계약 단언을 약화시키고 원인(저수지)을 남긴다. 저수지를 없애면 Linux와 같은 인과(“drain이 크레딧을 만든다”)가 복원되고 단언은 원래 세기 그대로다 |
| `fill_send_buffer` | 2바이트 대신 큰 페이로드를 pending으로 | EAGAIN이 **유지될 때까지** 채우기 | 페이로드를 키우면 "작은 write도 pending으로 남는다"는 검증 자체가 약해진다. 전제(버퍼가 찼다) 도달 방법만 고친다 |
| `test_stream_packet_progress` | `timeout_ms` 3 s → 10 s | 폴 간격 `msleep(1)` + 진단 루프 제거 | 마감 확대 금지. 관측자가 관측 대상 스레드를 굶기지 않게 하는 것이 원인 수정이다 |

## 5. Linux 검증

worktree `~/project/zlink-work/mac1`, `core/build-dev` (RelWithDebInfo, LTO OFF, 테스트 ON):

```
ctest -j1 --repeat until-fail:5 -R 'wake'                      → 5/5 통과 (2회 실행, 총 10반복)
ctest -j1 --repeat until-fail:3 -R '^(test_wake_invariants|test_stream_packet_progress|
  unittest_asio_transport_writev_lifetime|unittest_flow_state_monitor|test_ctx_options|
  test_xpub_nodrop|test_single_lane_wire_mandatory_count|test_single_lane_wire_old_peer_rejected)$'
                                                                → 8/8 통과
```

`test_wake_invariants`는 Linux에서 32.1 s → 32.9 s(settle quiet window 1 s 1회분). 계측값도 변화 없다
(`accepted_min=max=16`, `drain_elapsed_ms≈15 s`, `recovery_after_drain_ms=0`) — **Linux 동작은 그대로다.**

## 6. 스펙 재확인

건드린 것은 `core/tests/**` 뿐이다. `core/include/**`·`core/src/libzlink.vers`·`core/src/**` 무변경.
계약 동작(completion 발행 순서, READY/DISCONNECTED, POLLIN/POLLOUT level, WRITABLE wake의 순서와 조건)은
어느 문장도 다른 동작이 되지 않았다. 네 개의 wake 단언, writev lifetime 단언, packet-progress 단언은
**표현과 기대값 모두 그대로**이고 바뀐 것은 (a) 전제 조건(포화·버퍼 full)에 도달하는 방법과 (b) 관측 간격뿐이다.

## 7. 변경 분류

**C — 테스트의 관측/전제 도달 방식 수정** (Core 결함 아님, 계약 변경 아님, 우회 아님).
`core/builds/macos/build.sh`의 ctest 게이트화(MAC-1, `1d797c091e`)만 CI 성격이며 패치에 포함했다.

## 8. 남은 것

없다. 제외 정규식 불필요. 다만 진단용 `.github/workflows/core-macos-test.yml`(및 `core-macos-hang-sampler.yml`)은
병합 대상이 아니므로 패치에서 제외했다 — macOS를 상시 게이트로 만들려면 `core/builds/macos/build.sh`의
serial/parallel 분리 실행을 정식 CI 워크플로로 옮기는 별도 작업이 필요하다(권고).
