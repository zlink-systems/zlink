# DEALER-DEALER large-message stall 조사·수정 결과

## 결론

Core의 byte-credit wake 유실은 재현되지 않았다. 기존 100-client 회귀 테스트는 waiter의
10초 timeout을 server drain보다 먼저 시작했고, 아직 LWM까지 읽히지 않은 client를 wake
유실로 분류했다. Core source 수정 후보와 임시 계측은 모두 제거했다.

최종 변경은 `core/tests/integration/test_wake_invariants.cpp`의 결정적 회귀 테스트뿐이다.
100개 client의 poller가 drain 전에 실제 blocking wait에 들어갔음을 확인하고, 각 client가
byte-LWM을 넘은 뒤 2초 안에 100/100 POLLOUT이 회복되는지를 검사한다.

## 유실 지점 실증

10초 timeout 계측 run의 단계별 결과는 다음과 같다.

| 단계 | 실측 결과 |
|---|---|
| server reader가 record를 읽음 | 최종적으로 1,597/1,597를 읽음 |
| reader credit / `activate_write` 발행 | timeout 당시 미회복 40개 socket 모두 wake count 1 이상; 미회복군 합계 `activate_woke=40`, `activate_joined=79` |
| client mailbox 등록·signal | 미회복 40/40 모두 pending hint가 켜졌고 primary mailbox FD가 readable |
| DEALER LB 재활성화 | 별도 67/100 run의 미회복 33/33에서 command를 소비하자 `out_active=1`, waiter=0으로 전환 |
| public readiness 관찰 | 10초 시점에는 60/100만 관찰; timeout을 30초로 바꾸면 100/100 관찰 |

따라서 command enqueue와 signal 사이에서 edge가 사라진 것이 아니다. timeout 시점에도
미처 소비하지 않은 command와 readable FD가 그대로 남아 있었다. 실제 30초 run에서 마지막
POLLOUT은 waiter 시작 후 10,795ms에 도착했고, server가 backlog 전체를 읽는 데는
27,891ms가 걸렸다. 실패 지점은 Core transition이 아니라 **LWM 도달 전에 끝난 테스트의
절대 deadline**이었다.

검토했던 `get_events_for_poller()`의 `schedule_if_needed()` 호출은 제거했다. 정상 mailbox
send/reschedule protocol을 중복하고 idle callback 반복 가능성을 만들며, 해당 후보를 넣은
run도 56/100만 10초 안에 회복해 원인 수정이 아니었다.

## 최종 변경

Repository 최종 diff는 한 파일, +366/-0줄이다.

- `core/tests/integration/test_wake_invariants.cpp`
  - TCP DEALER↔DEALER 100-client, 64KiB payload, 1MiB byte-HWM, 4KiB socket buffer 조건을 추가했다.
  - 각 client를 synchronous DONTWAIT send로 실제 EAGAIN까지 채운다.
  - client마다 persistent public poller를 미리 등록하고, 100개 모두 `ZLINK_CONFIG_BUSY`로
    blocking wait에 들어간 다음에만 server가 읽기 시작한다. waiter마다
    `zlink_poller_wait`는 한 번만 호출하므로 재등록이 wake 문제를 가리지 않는다.
  - server가 각 client에서 8개 record를 읽는다. 8×64KiB는 1MiB HWM의 512KiB LWM
    이상이므로 모든 writer에 credit을 발행한 경계를 고정한다.
  - 마지막 client가 LWM을 넘은 시점부터 마지막 POLLOUT까지 2초 미만이며 100/100이
    회복됐는지 검사한다. 60초 값은 실패 시 thread를 회수하기 위한 safety timeout뿐이다.
  - 기존 wake-invariant test 3개의 `RUN_TEST` 등록도 유지한다.

Core source, public API/ABI, framework, bindings, doc, scripts/local-package,
`hotpath_reference.json`은 바꾸지 않았다. mailbox/socket/pipe snapshot, counter, 강제 command
drain과 `WAKE_DIAG` 출력도 최종 diff에 없다.

주의할 계약 경계가 하나 있다. 이 test 파일의 byte-buffer `zlink_send`는 현재 exported
Core symbol이 아니라 `testutil_unity.hpp` compatibility shim이며, valid socket에서는
`send_msg_internal`로 들어간다. 따라서 회귀 테스트가 고정하는 것은 pull-completion과
독립인 synchronous DONTWAIT physical-HWM 경로와 public poller의 POLLOUT 회복이다.

## 검증 결과

모든 명령은 `ulimit -v 16777216` 아래서 실행했다.

| 검증 | 결과 |
|---|---|
| `bash scripts/build-core.sh dev` | PASS |
| dev `test_wake_invariants` 최종 단일 실행 | PASS, 15.37s |
| `bash scripts/build-core.sh release-gate` | PASS, Release + LTO |
| `ctest --test-dir core/build -j2` | PASS, 140/140, 184.30s |
| `ctest --test-dir core/build -L wake-invariant -j1` ×5 | PASS, 20/20; 17.52s, 16.87s, 16.91s, 16.99s, 16.68s |
| 새 test를 포함한 `test_wake_invariants` 위 5회 | PASS; 16.20s, 15.56s, 15.60s, 15.68s, 15.36s |
| standalone `ctest -R '^hotpath_gate$'` | PASS, 5.15s |
| raw header mirror cmp | PASS, 12/12 |
| `git diff --check` | PASS |

Hotpath ratio는 허용 범위 ±5% 안이다.

| cell | reference 대비 ratio |
|---|---:|
| DEALER/DEALER inproc | 1.0022 |
| DEALER/ROUTER req/rep inproc | 1.0068 |
| PAIR inproc | 1.0032 |
| ROUTER/ROUTER tcp | 1.0001 |

참고 multi benchmark는 첫 size가 멈추면 다음 size를 실행하지 못하므로 같은 runner를
4096B와 65536B로 나눠 각각 26초 제한으로 확인했다. 두 경우 모두
`DEALER_DEALER`/tcp 본 실행에 들어간 뒤 RESULT 없이 timeout(exit 124)했고 잔류 프로세스는
없었다. 이는 `bb66e85376` 이후 SEND completion을 drain하지 않는 benchmark 계약 문제와
일치하며, 이 작업 범위에서는 수정하지 않았다.

최종 branch는 `main`, HEAD는 `88de99a0adfd`다. commit은 만들지 않았다.
