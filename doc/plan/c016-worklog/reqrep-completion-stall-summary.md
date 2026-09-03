# DEALER/ROUTER request completion stall 조사 요약

## 결과

- 근본 원인을 수정하고 sleep 없는 결정적 회귀 테스트를 추가했다.
- 모든 임시 계측/A-B 변경을 제거했다. 감독관이 추가한 `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp` 진단 출력은 그대로 보존했다.
- 정체의 상류 경계는 확인했다. 실패 connection의 **server session→ROUTER application pipe가 ROUTER에서 한 frame도 소비되지 않은 채 HWM에 도달하고 영구 정지**한다. client completion/timeout 정지는 그 결과 생기는 하류 backpressure다.

## 원인 실증

### 재현 수치

- 원본 candidate: 5회 중 3회 실패.
  - `stall2.log`: 1 socket / 3,606 requests
  - `stall3.log`: 2 sockets / 5,355 requests
  - `stall5.log`: 4 sockets / 13,231 requests
- 대표 저비용 계측 실패 `light-results/stall/hwm-trace-6.log`:
  - replies: 946,395
  - drain timeout: 1/100 socket, 2,569 requests
  - client DEALER socket: `0x576a897fc600`
  - client application writer pipe: `0x576a8981d8c0`
  - 마지막 상태: `written=8,055,936`, `peer_read=7,007,616`, `hwm=1,048,576`

### 실제 정지 경계

같은 실패 로그의 server 프로세스에는 HWM arm이 정확히 하나 있고 대응 credit apply가 없다.

```text
[server] [hwm-credit] arm pipe=0x7af0f41885c0
         sink=0x7af0f8251e28 peer=0x7af0f4188930
         generation=1 written=4095433 peer_read=0 hwm=4096000
```

- `0x7af0f41885c0`은 server session이 쓰는 request application pipe이고, peer `0x7af0f4188930`은 ROUTER/FQ 측 endpoint다.
- `peer_read=0`이므로 ROUTER는 이 connection에서 첫 request조차 소비하지 않았다.
- session writer는 4,095,433 bytes를 채운 뒤 HWM에 걸렸고 이후 `activate_write/apply`가 없다.
- 이 때문에 해당 TCP connection의 server input이 멈추고, client session output도 진행하지 못해 client application pipe가 1 MiB HWM에서 멈춘다. 이후 제출은 pre-admission send-pending으로 쌓인다.

### timeout/completion 가설 배제

- drain timeout 직후 poller를 우회하여 각 DEALER에 `zlink_completion_recv(DONTWAIT)`를 직접 실행했으나 4,102건 중 0건만 drain됐다. `d80ba60c9b`의 completion `has_ready` cache/public poller lost-wake가 아니다.
- 정체 socket의 aggregate timeout task는 admission된 요청을 정상 만료한 뒤 `next_deadline=0`, `has_send_pending=1`이었다. 남은 요청은 물리 전송 큐 admission 전이다.
- DEALER 계약상 request timeout은 local send queue admission 시 시작하므로 pre-admission 요청에 timeout completion이 없는 것은 정상이다. `f3be895b3f`의 earliest-deadline rearm 누락이 아니다.
- rf1 completion queue enqueue/dequeue/close는 동일 mutex 아래 직렬화된다. false-stuck interleaving도 정적 감사에서 발견되지 않았다.
- `8b6c2aa906`의 temporary public command owner는 PAIR 전용이다. DEALER mailbox command 경로와 rf3 signal helper에서도 정체를 만드는 유실 경합을 찾지 못했다.
- `23bb5a968f` pending driver의 enqueue/redrive epoch handoff는 실패 후 epoch 재검사와 mailbox command로 닫혀 있다. 정체 pending 자체가 원인이 아니라 server input 정지의 결과다.

### A/B 결과

- `8b6c2aa906`의 prefetched-batch-tail 조건을 f3 동작으로 복원: 10회 중 3회 실패. 원인 아님, 원복.
- 모든 endpoint를 수동 1 MiB HWM으로 통일: 5회 중 1회 실패. auto-HWM cache 차이는 단독 원인 아님.
- client writer HWM arm에서 peer reader wake 강제: 10회 중 8회 실패. 원인 아님, 원복.
- 이미 active인 reader까지 강제 `read_activated`: 5회 중 1회 실패. timing만 바뀌고 정체가 남아 원복.
- inactive FQ pipe 중 published request가 있는지 검사: 실패 3회를 포함한 5회에서 recovery hit 0. 단, 최종 server 증거는 pipe가 FQ active partition에 남아 있거나 reply-token admission blocked set에 있을 가능성을 남긴다.

## 근본 원인과 커밋 귀속

`f3be895b3f`가 `router_t::xread_activated()`/`xread_deactivated()`에 추가한 count-1 fast path가 원인이다. 이 조건은 pair/lane/count/ready cache만 검사해, routing identity가 아직 게시되지 않아 `_anonymous_pipes`에만 있고 FQ에는 등록되지 않은 pipe도 이미 채택된 것으로 오판했다.

첫 `activate_read`에서 fast path가 `_fq.activated(pipe)`를 호출하지만 미등록 pipe라 no-op이고 곧바로 return한다. 이 과정에서 anonymous identity 채택 slow path를 영구히 건너뛴다. 이후 ypipe에는 data가 계속 쌓이지만 ROUTER/FQ는 pipe를 모른 채이고, session writer가 `peer_read=0`으로 HWM에 도달한다. 대표 실패 로그의 정확한 상태와 일치한다.

수정은 두 fast path에 이미 존재하는 채택 증표 `router_route_binding_token() != 0`을 추가했다. token이 0인 anonymous/transition pipe는 기존 slow path에서 identity를 채택하고 FQ에 attach한다. 공개 API/ABI나 새 옵션은 없다.

## 결정적 회귀 테스트

`test_count1_router_adopts_anonymous_pipe_on_first_activation`은 기존 synthetic pipe/mailbox harness를 사용한다. count-1 Application pipe를 identity 게시 전에 attach하여 `ready=true`, `route token=0`, `FQ inactive` 상태를 직접 assert한 뒤 identity와 request를 쓰고 단 하나의 queued activation command를 drain한다. 수정 전에는 token이 0인 채 request receive가 EAGAIN이고, 수정 후에는 token/FQ activity가 게시되고 request가 수신된다. sleep은 사용하지 않는다.

## 변경 파일

- `core/src/runtime/sockets/router/router_recv_path.cpp`: count-1 activation/deactivation fast path를 route-binding token으로 fence.
- `core/tests/integration/test_zmp_request_reply_receive_transaction.cpp`: anonymous pipe first-activation 결정적 회귀 테스트 추가.
- `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp`: 기존 감독관 진단만 보존.
- 조사 기록: `reqrep-completion-stall-progress.md`, 이 요약 파일.

## Gate 결과

- 작업 시작 시 `ulimit -v 16777216 && cmake --build core/build -j4`: 성공(`ninja: no work to do`).
- `git diff --check`: 성공.
- 수정 후 동일 executable/direct runner 재현 10회 연속 성공(`fixed-direct.log`, `fixed-direct-2.log`, 각 5 runs, rc=0).
- focused target build 및 `test_zmp_request_reply_receive_transaction`: 성공(10 tests, 0 failures).
- 시간 상한 때문에 전체 ctest 138, wake-invariant ×3, hotpath gate, raw mirror cmp 12는 실행하지 않았다.
- 계측 제거 후 clean-source 재빌드에서 Core object, `libzlink.a`, `libzlink.so.0.15.1` 링크는 성공했다. 변경과 무관한 LTO test executable relink가 이어져 16/87에서 시간 상한 준수를 위해 중단했으므로 전체 build gate로는 계상하지 않는다.

## 추가 테스트

- `test_count1_router_adopts_anonymous_pipe_on_first_activation` 추가 및 green.

## BLOCKERS

- 1.5시간 시간 상한으로 전체 gate는 미실행이다. 수정 후 재현 10회와 focused 결정적 테스트는 green이다.
