# C++ Framework 성능 개선 재취합 — Core I/O thread 2.2배의 원인 후보와 적용 계획

읽기 전용 분석이다. 코드·스펙·빌드·측정·git 변경은 없다. µs 값은 측정값(출처 표기)과 **가설**(계산식 표기)을 구분한다.

## 0. 기준과 입력

| 항목 | 값·출처 |
|---|---|
| 저장소 | `D:/worktree/zlink-fable-design`, `39bbfe470d`(읽기 전용) |
| 측정 | `D:/t/jobs/fx/cx/jfable-syn/xio-report.md`(이하 X), main `39bbfe470d`, Core·binding 1.23.0, Release `-O3 -DNDEBUG`, `-g` 없음(X:7) |
| 동일 처리율 셀 | request-backpressure 1 KiB, binding 21,928.4 / Framework 21,966.3 msg/s, 3회 중앙값(X:10,16-19) |
| 이전 문서 | `rev1639cpath/report.md`(R), `jfable-08/design.md`(D8), `jfable-net/compare.md`(CN), `jfable-08/count-report.md`(CR) |

경로 접두어. `K`=`core/src/`, `KD`=`core/doc/spec/core/`, `B`=`bindings/cpp/src/Runtime/`, `C`=`framework/languages/cpp/framework/src/runtime/`, `H`=`framework/languages/cpp/framework/include/zlink/framework/`, `P`=`framework/bench/grpc/cpp/`, `S01`·`S08`=`framework/doc/framework/common/spec/server/01-execution/{01-submit-and-completion,08-messaging-hot-path}.ko.md`, `T01`=`.../02-channel-transport/01-channel-topology.ko.md`, `RB`=`KD/08-runtime-boundary.ko.md`.

### 0.1 측정값(X)

| 구분 | target 가장 바쁜 I/O | target 활성 I/O 합 | source 가장 바쁜 I/O | source 활성 I/O 합 | 출처 |
|---|---:|---:|---:|---:|---|
| binding 22k | 7.42 | 14.33 | 7.35 | 12.57 | X:18 |
| Framework 22k | 17.05 | 29.27 | 17.95 | 30.55 | X:19 |
| thread별 | binding target IO/0 6.91·IO/1 7.42, source IO/0 7.35·IO/2 5.22; Framework target IO/0 12.03·IO/1 17.05, source IO/0 17.95·IO/2 12.60 | | | | X:25-32 |

두 프로세스 모두 활성 I/O thread가 **둘**이고 둘 다 Framework에서 1.7–2.4배다(target IO/0 12.03/6.91=1.74, IO/1 17.05/7.42=2.30; source IO/0 2.44, IO/2 2.41). 연결은 하나이므로(§1 행 "연결 수") 두 번째 활성 thread는 transport engine이 아니라 Core가 socket의 command 처리를 맡기는 **async command owner**(`K/runtime/sockets/common/socket_lifecycle_runtime.cpp:593-611`, socket mailbox를 I/O thread의 io_context에 연결)다. 어느 TID가 engine이고 어느 것이 owner인지는 X에 없다(§6 측정 항목).

| 조건 | binding | Framework | 출처 |
|---|---:|---:|---|
| request-backpressure 1 KiB, 무제한 | 3.48/2.63 (253.20k msg/s) | 15.37/15.16 (53.28k) | X:9 |
| request-backpressure 4 KiB, 무제한 | 6.12/6.00 (161k) | 17.61/18.35 (47k) | X:617-618 |
| request-backpressure 4 KiB, 10k paced | 14.16/16.16 | 19.53/20.69 | X:619-620 |
| send 1 KiB, 무제한 | 1.48/1.57 (609k) | 3.31/4.15 (183k) | X:621-622 |
| send 1 KiB, 10k paced | 12.27/12.77 | 11.89/13.56 | X:623-624 |
| frame/msg, byte/msg | 2, 1,228 (201+1,027) | 2, 1,401.65 (13+1,388.65), reply 1,391.65 | X:603-608 |

kairos가 before/after.json의 voluntary context switch로 계산한 값(보고서 밖): target IO/1 깨어남당 CPU 31.7 대 14.2 µs(2.2배), 깨어남/msg 0.52–0.77 대 0.52. 이 둘은 kairos 산출값이며 X의 표에는 없다.

### 0.2 측정값이 말하는 것 — 깨어남당 고정 비용과 깨어남당 메시지 수

같은 thread의 메시지당 CPU는 `W / M`(W=깨어남 한 번의 고정 CPU, M=깨어남당 처리 메시지 수)로 읽는다. **가설** 계산:

- binding: 10k paced(4 KiB) 14.16, 22k 7.42, 253k 3.48 → 10k에서 M≈1이면 W_b≈12–14; 22k에서 M_b≈13/7.42≈1.75; 253k에서 M_b≈3.7. rate가 오르면 M이 커져 메시지당 비용이 준다.
- Framework: 10k paced(4 KiB) 19.53, 22k 17.05, 53k 15.37 → 10k에서 M≈1이면 W_f≈18–20; 22k에서 M_f≈19.5/17.05≈1.14; 53k에서 M_f≈1.27. rate가 올라도 거의 줄지 않는다.
- send 1 KiB 10k paced는 양쪽이 같다(12.27 대 11.89, X:623-624). 즉 **한 메시지만 든 깨어남의 비용은 양쪽이 같고(≈12 µs), 차이는 request·reply 경로에서만 생긴다**(브리프 §"측정").

22k에서의 차이 9.63 µs(17.05−7.42)를 두 항으로 나눈 **가설**: (H1) 깨어남 고정 비용 차이 — 4 KiB 10k paced의 19.53−14.16=5.4 µs가 근거, M_b=1.75로 나누면 ≈3.1 µs; (H2) 깨어남당 메시지 수 차이 — 나머지 ≈6.5 µs(= 19.5/1.14 − 19.5/1.75). kairos 산출값(깨어남당 31.7 대 14.2, 깨어남/msg 0.52–0.77 대 0.52)은 H1 쪽을 더 크게 본다. 두 읽기가 어긋나므로 §6의 측정은 context switch가 아니라 **epoll 반환 수·read 완료 수**로 M을 직접 센다.

## 1. 경로 대조 — Framework와 binding 벤치가 Core를 쓰는 방식

Core 공통 기구(양쪽 동일): application thread의 submit은 pipe write 뒤 `flush`에서 reader가 자고 있으면 `activate_read` command를 보낸다(`K/runtime/core/pipe_write.cpp:1426-1448`, `K/runtime/core/pipe_transport.cpp:51-69` "every message that finds the inbound reader asleep runs this command"). command 전달은 mailbox `send`(`K/runtime/core/mailbox.cpp:70-101`): cpipe write·flush → 등록된 poller signaler에 eventfd write(`:87`, `:482-487`) → `_primary_signaler_required`이거나 executor가 없으면 primary eventfd write(`:94-97`) → executor가 있으면 `boost::asio::post`(`:338-362`). I/O thread는 asio `poll()`로 준비된 이벤트를 한 번에 처리하고 없으면 `run_for(≤100 ms)`로 잔다(`KD/systems/03-io-thread.ko.md:68-78`). engine은 read 완료마다 decode 루프 뒤 `session->flush()` 한 번(`K/runtime/engine/asio/asio_engine.cpp:1273-1311`), write는 `async_write_some`/`async_writev`(`:685-760,838-853`). socket의 command는 async command owner(I/O thread)가 `process_async_mailbox`로 처리한다(`K/runtime/sockets/common/socket_base_lifecycle.cpp:1705-1726`; hot path 표 `KD/systems/10-hot-path.ko.md:43`). owner는 command batch 뒤 `signal_pollers`로 poller를 깨운다(`socket_base_lifecycle.cpp:559-570`).

| 항목 | C++ Framework (request-backpressure) | C++ binding 벤치 | 차이 |
|---|---|---|---|
| socket·옵션 | ROUTER, `handover(true)`, `mandatory(true)`, `linger 0`, 선택적 `recv_timeout`, RID 설정, bind(`C/mesh/raw_mesh_node_owner.cpp:513-532`) | client ROUTER RID 설정·connect(`P/client/bench_cpp_client.cpp:440-449`); server ROUTER 2개 bind(`P/zlink/bench_zlink_cpp_server.cpp:133-139`), 옵션 없음(CR:7) | Core 기본이 이미 `_mandatory(true)`(`K/runtime/sockets/router/router.cpp:37`), handover는 RID 중복 정책(`:39`)으로 메시지 경로에 없음. **메시지당 차이 없음** |
| 연결 수 | source가 manual connect 1개(`P/client/bench_cpp_client.cpp:808-815`), target은 역방향을 만들지 않음(T01:447-456 "connect initiator가 하나") | client 1개 | 같음(1) |
| poller 등록 | ingress poller에 `pollin\|pollroute\|pollconnect\|pollout\|pollcompletion` + wake timer(`C/backend/raw_route_port.cpp:116-133`, `C/mesh/raw_mesh_node_owner.cpp:540-549`) | client `pollcompletion`만(`P/client/bench_cpp_client.cpp:449`); server **poller 없음**, blocking `recv`(`P/zlink/bench_zlink_cpp_server.cpp:37-41`) | target: Framework만 poller. Core는 poller 등록 socket의 mailbox에 primary eventfd 통지를 켠다(`K/runtime/core/socket_poller.cpp:297-325`, `K/runtime/core/mailbox.cpp:420-437`; `waits_on`=`events!=0` `socket_poller.hpp:96-99`) |
| poll 1회의 Core 작업 | `zlink_poller_wait` → `get_events_internal`: `process_commands` + POLLCOMPLETION이면 completion drain(`K/runtime/sockets/common/socket_base_api.cpp:880-910`); ROUTER `xhas_out`은 `mandatory`면 pipe 순회(`K/runtime/sockets/router/router_send_path.cpp:561-567`); `has_route_change`는 atomic 2회(`router.cpp:89-96`); permit이 없으면 `modify` 2회 → 다음 wait에서 `rebuild`(`C/backend/raw_route_port.cpp:431-444`, `K/runtime/core/socket_poller.cpp:184-196,667-675`) | client 같은 wait, POLLCOMPLETION만 | application thread 비용. I/O thread에는 직접 영향 없음 |
| request 제출 thread·API | caller thread(lane turn) → `_socket_mutex` 아래 `request(rid).message(...).timeout(남은 deadline).async()`(`C/backend/raw_route_port.cpp:363-400`) → binding `zlink_request(..., DONTWAIT, timeout_ms, completion_id)`(`B/Messaging/operation_submit.hpp:130-200`, timeout `:164-167`) | submit thread에서 같은 builder, timeout 30,000 ms 고정(`P/client/bench_cpp_client.cpp:641,688`) | API 같음. timeout 값만 다름(요청마다 감소). timer thread `zlink-req-time`은 양쪽 0 CPU(X:69,338) |
| Core request 경로 | `submit_pull_dontwait_request`: pending 관찰자 등록·`request_admission_submit`·timeout arm(`K/api/socket/socket_request_reply_submit_api.cpp:172-211`); pipe별 correlation 예약 `_out_sync`(`K/runtime/core/pipe_receive.cpp:624-654`) | 같음 | 같음(app thread) |
| reply thread·API | **worker thread**(handler turn) → `_socket_mutex` 아래 `reply(rid, token).message(...).submit()` blocking(`C/backend/raw_route_port.cpp:516-537`) → `zlink_reply`(`B/Messaging/reply_operations.cpp:16-50`) | **recv와 같은 thread**에서 즉시 `received.reply().submit()`(`P/zlink/bench_zlink_cpp_server.cpp:55-66`) | API 같음(`public_router_reply_submit` `K/api/socket/socket_request_reply_submit_api.cpp:472-515`). **thread와 지연이 다름**: Framework는 recv→mailbox→worker 인계 뒤 reply |
| recv | ingress poller thread, `recv(DONTWAIT)` record 1건(`C/backend/raw_route_port.cpp:475-503`) | blocking `recv`(flags none) 루프 | Core 수신 hot path 같음(`KD/systems/10-hot-path.ko.md:39`). 비어 있으면 reader가 잠드는 것도 같음 |
| completion drain | ingress poller thread가 POLLCOMPLETION으로 drain(binding poller `B/Eventing/poller.cpp:156-182`), 관찰 coroutine 재개(`C/backend/raw_binding_adapter.hpp:146-199`) | submit thread의 `_poller.wait`에서 drain(CR:5) | 같음. `_completion_poller_refs>0`이면 async owner는 drain을 생략(`socket_base_lifecycle.cpp:1718-1724`). server 쪽은 Framework만 poller → binding server는 owner가 completion lane을 직접 drain |
| HWM·admission | HWM 명시 없음, auto-HWM 기본(CR:7). DONTWAIT BACKPRESSURED 비율 p_r 0.34 %(CR:18) | 같음(CR:7) | 같음 |
| **수신 flow 제어** | application job queue 압력(pause 80 %·resume 60 %, `H/contracts/configuration/detail/framework_options_state.hpp:67-68`)이 바뀌면 `set_receive_flow_state(paused/running)`(`C/dispatch/application_job_receive_flow.hpp:14-30`, `C/mesh/raw_mesh_node_owner.cpp:524-529`) → Core가 pair마다 **wire control frame**을 쓴다(`K/runtime/sockets/common/socket_base_flow_state.cpp:24-64,149-171`); 상대는 completion lane에서 소비(`K/api/socket/socket_request_reply_dispatch.cpp:262-275`) | 없음 | **Framework만**. 네 언어 공통(.NET `ZLinkManagedMeshNode.cs:416`, Java `ZLinkChannelSocketRegistry.java:1978`) |
| 추가 thread | `zlink-timer`(poller wake timer, `K/api/monitoring/timer_scheduler_backend.cpp:133`; X:332) 0 CPU; Framework stats host의 둘째 context | stats host의 둘째 context(X:14) | I/O 비용 아님 |
| 메시지 모양 | 2 frame, 13 + 1,388.65 B(X:606-607) | 2 frame, 201 + 1,027 B(X:604-605) | +14 % byte. 13 B part는 Core VSM(inline) 범위라 할당이 오히려 적다. **원인 아님**(브리프) |
| 다른 언어와의 동등성 | .NET도 `PollOut\|PollCompletion\|PollRoute\|PollConnect`(`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:39-44`)와 receive flow state를 쓴다 | — | C++ Framework의 Core 사용은 네 언어 공통 형태다 |

## 2. 깨어남당 2.2배의 원인 후보

각 후보에 근거, 깨어남당 추가 CPU **가설**, 다음 perf에서 보일 판별 조건을 적는다. 비용 단가 가정: eventfd write로 잠든 thread를 깨우는 호출 1.5–4 µs(syscall 0.5 + 스케줄러 wake 1–3), futex broadcast 같은 값, `epoll_wait` 반환 1회 2–4 µs(모두 가설).

| 번호 | 후보 | 근거 file:line | 어느 쪽에 있는가 | 깨어남당 추가 CPU 가설 | 판별 조건(perf) |
|---|---|---|---|---|---|
| **N1** | **깨어남당 메시지 수(M)와 깨어남 수.** Framework는 recv와 reply가 다른 thread에 있어(§1 "reply thread") reply가 요청 read보다 수십 µs 뒤에 pipe에 들어온다. engine 루프는 `poll()` 뒤 바로 `run_for`로 자므로(`KD/systems/03-io-thread.ko.md:73-78`) 늦게 온 reply는 **새 깨어남**이 된다. binding server는 같은 thread에서 수 µs 안에 reply를 써 같은 깨어남 안에 든다. source도 completion→dispatcher→caller 인계(S01 §11 `:490-513`, D8 §1.4 C5) 뒤에야 다음 request를 제출해 제출 밀도가 낮다 | `P/zlink/bench_zlink_cpp_server.cpp:41-66`(같은 thread); `C/mesh/mesh_node_host_service.cpp:3354-3362,2125-2141`(owner 알림→worker, R §4.2); `C/foundation/operation_registry.cpp:269-290`(완료마다 dispatcher item) | 양쪽(target·source) | §0.2 H2: 22k에서 ≈6.5 µs/msg(=19.5/1.14−19.5/1.75). 깨어남당 CPU 자체는 늘지 않고 **깨어남이 는다** | I/O thread의 `epoll_wait`(또는 asio `run_for`) 반환 수/msg와 `asio_engine_t::on_read_complete`·write 완료 수/msg. Framework가 binding의 1.3–2배면 N1 확정. kairos의 context switch 계수(0.52–0.77 대 0.52)와 어긋나면 context switch 계수가 asio `poll()` 경로를 놓친 것 |
| **N2** | **poller 통지 syscall.** Framework target socket은 poller에 등록되어 `_primary_signaler_required=true`(`K/runtime/core/mailbox.cpp:419-424`). 그러면 engine thread의 `mailbox::send`가 command마다 primary eventfd write를 하고(`mailbox.cpp:94-97`), async owner가 batch 뒤 `signal_pollers`로 한 번 더 쓴다(`socket_base_lifecycle.cpp:562`, `mailbox.cpp:472-480`). binding server는 poller가 없어 두 write가 없고, parked recv thread가 있을 때만 `publish_receive_progress`의 cv broadcast 1회(`K/runtime/sockets/common/socket_runtime.hpp:377-384`) | 위 | **target만**(binding client도 poller라 source는 대칭) | target: +2 eventfd write −1 futex ≈ +1.5–4 µs/msg (9.63의 16–42 %). source: 0 | I/O thread TID의 `syscalls:sys_enter_write` 수/msg와 `zlink::signaler_t::send`(`K/runtime/core/signaler.cpp:200-227`) self time. Framework target ≈2/msg, binding target ≈0이면 N2 확정. source에서 양쪽이 같아야 한다 |
| N3 | **receive flow state control frame.** job queue 압력 전환마다 pair별 wire command frame + flush(`socket_base_flow_state.cpp:149-171`) → engine write, 상대 completion lane drain(`socket_request_reply_dispatch.cpp:262-275`), 상대 송신의 `remote_flow_blocked` 거절(`pipe_receive.cpp:635-637`) → WRITABLE record·재제출 | 위; 전환 수는 `paused_transition_count`(`C/dispatch/application_job_queue.hpp:603-608`) | Framework만, 양쪽 node | 전환 f회/s일 때 ≈ f×(5–15 µs)/22k. p_r 0.34 %(CR:18)가 전부 이 전환이라도 74회/s → **<0.1 µs/msg**. 가설상 작다 | uprobe `zlink::socket_base_t::set_local_receive_flow_state`·`consume_receive_flow_state_frame` 호출 수/s. 수백 회/s 이상이면 재계산 |
| N4 | **poll event mask·modify.** `pollout`은 `xhas_out`→pipe 순회, `pollroute`·`pollconnect`는 atomic 비교; permit 없는 회전은 `modify` 2회→`rebuild`(signaler 재등록, mailbox mutex) | `router_send_path.cpp:561-567`; `router.cpp:89-96`; `raw_route_port.cpp:431-444`; `socket_poller.cpp:184-196,297-325` | Framework만 | **application(poller) thread** 비용 0.1–0.5 µs/poll. I/O thread 추가 0(rebuild가 `_primary_signaler_required`를 다시 켜는 것 외) | `socket_poller_t::rebuild` 호출 수/msg(app thread). I/O 판별과 무관 |
| N5 | **두 번째 application thread로 인한 Core 진행 통지.** worker의 blocking `submit()`은 HWM 대기일 때만 `wait_submit_progress`·owner lease를 쓴다(`socket_request_reply_submit_api.cpp:393-470`, `socket_base.hpp:1547-1568`); 그때 async owner가 `submit_progress.cv.broadcast`(`socket_base_lifecycle.cpp:564-570`)·`publish_receive_progress` | 위 | Framework만 | 정상 경로(p_r 0.34 %)에서는 거의 0. 가설 <0.2 µs/msg | I/O thread TID의 `futex` syscall 수/msg. 0.1/msg 이하면 제외 |
| N6 | **async owner의 completion lane drain 분담.** binding server는 poller가 없어 owner(I/O thread)가 completion pipe를 drain하고(`socket_base_lifecycle.cpp:1718-1724`), Framework는 poller thread가 한다 | 위 | binding server 쪽이 더 함 | 방향이 반대(Framework가 I/O에서 덜 함). 원인 아님 | `process_ready_completion_pipes`·`process_completion_records` self time이 binding target I/O에서만 보여야 한다 |
| N7 | request timeout 값 변동·correlation 예약·`zlink-req-time` | `operation_submit.hpp:164-167`; `pipe_receive.cpp:624-654`; X:69,338 | 값만 다름 | timer thread 0 CPU, 예약은 app thread. **제외** | — |
| N8 | 옵션(handover·mandatory·linger)·frame 수·byte 수 | §1 "socket·옵션", "메시지 모양"; X:603-608 | — | 메시지 경로 밖이거나 +14 % byte. **제외**(브리프 §"측정") | — |

종합 가설. 깨어남당 2.2배의 주된 원인은 Framework가 Core에 더 많은 일을 시키는 것이 아니라 **(N1) 요청·응답이 서로 다른 깨어남에 떨어져 I/O thread가 메시지마다 고정 비용(≈12–20 µs)을 다시 내는 것**이고, target에서는 **(N2) poller 통지 syscall**이 더해진다. N3–N5는 가설상 작아 perf로 제외한다. 이 가설이 맞으면 Framework의 caller·worker CPU를 줄여도 I/O thread 비용은 줄지 않고, **인계 지연을 줄이는 변경(§4.4 수신 역할 인계, B1)과 Core의 깨어남 고정 비용**이 I/O 상한을 정한다.

## 3. 소유 계층 판정

계층 소유 조항: AGENTS.md `:59-63`(계층 소유권: Core·binding이 소유한 결정을 Framework에서 재구현하거나 보상하지 않는다), `:64-67`(금지 패턴), `:68-70`(교차언어 대조), RB `:29,78-79,230`(Framework는 binding 공개 raw API만 사용), RB `:49-50,126`(I/O thread·engine은 Core 소유), `KD/systems/10-hot-path.ko.md:33-43`(Core hot path 표, `process_async_mailbox` 포함), `KD/05-polling.ko.md:84-93`(poller wake 규칙은 Core 소유), S08 §4.4 `:300-316`(수신 역할 ≠ handler 자원), S01 §11 `:490-513`(완료 callback의 새 turn).

| 후보 | 분류 | 판정 근거 | 고칠 수 있는 곳 |
|---|---|---|---|
| N1 깨어남 수 | **(c) 계약상 필요한 차이 + (b) Core 비용** | 인계 자체는 S08 §4.4·S01 §11이 요구한다(c). 그 결과 생기는 "메시지마다 깨어남 1회 ≈ 12–20 µs"는 Core의 깨어남 고정 비용(b): engine read 완료 → `mailbox::send`(eventfd write + asio post) → **다른 I/O thread**의 owner wake → `process_commands` → `signal_pollers`(eventfd write) → poller wake, 즉 inbound 1건에 I/O thread 2개와 syscall 3–4회(`mailbox.cpp:70-101`, `socket_lifecycle_runtime.cpp:593-611`, `socket_base_lifecycle.cpp:559-570`). Framework는 인계 **지연**만 줄일 수 있다(§4.4 인계, B1) | Framework: §4.4 인계·B1(지연). Core: 깨어남당 경로(owner를 engine과 같은 thread에 두거나 통지 합치기) — Core 설계 결정, 별도 이슈 |
| N2 poller 통지 | **(b) Core** | poller 등록 socket의 통지 규칙은 `KD/05-polling.ko.md:84-93`이 소유. 같은 command batch에 primary write 2회(engine `send`, owner `signal_pollers`)는 Core 내부 중복. Framework가 poller 대신 blocking recv를 쓰는 것은 S08 §4.4 수신 역할·completion drain 구조와 맞지 않고 .NET도 poller를 쓴다 | Core: `mailbox::send`의 primary write를 owner 통지에 합치기(`mailbox.cpp:93-97` 조건) |
| N3 flow control frame | (a) Framework 사용 — 단, 빈도가 낮으면 비용 없음 | 압력 전환 빈도는 Framework 임계값(80/60 %) 소유. 전환이 잦으면 hysteresis가 Framework 몫 | Framework(네 언어 공통 임계값) |
| N4 event mask·modify | (a) Framework — application thread 비용 | I/O thread와 무관. `pollout`을 켜는 이유는 S08에 없고 .NET도 켠다(네 언어 공통 형태) | Framework(네 언어 동시), 우선순위 낮음 |
| N5 진행 통지 | (b) Core, 정상 경로에서 0 | HWM 대기 때만 | — |
| N6 | 원인 아님 | — | — |

교차언어: N1·N2·N3은 .NET·Java도 같은 Core 사용 형태(§1 마지막 행)이므로 Core(b) 수정은 네 언어에 같이 효과가 있고, Framework(a) 변경은 네 언어를 함께 바꿔야 한다(AGENTS.md `:68-70`).

## 4. 개선 방안 재취합

처리량 상한 thread. request-backpressure 1 KiB 현재: caller 17.41 µs → 57k(R §1), target I/O 15.37 µs@53k → **≈65k**(X:9, 브리프). 1~7단계 뒤 caller 5.66–7.12 µs → 140–177k(D8 §6.4)이므로 **I/O thread 65k가 바로 상한**이 되고 F÷B ≤ 65/249 = 0.26이다. 0.6(150k)은 I/O ≤ 1e6/150k = **6.7 µs/msg**를 요구한다 — binding은 22k에서 7.42, 253k에서 3.48이므로 I/O 쪽이 binding과 같은 M(깨어남당 메시지 수)에 도달해야만 가능하다. send 1 KiB: source I/O 4.15 µs@183k(X:622) → 241k 상한; 1~7단계 뒤 caller 2.05–2.61 µs → 383–488k(D8 §6.4)이므로 역시 I/O가 먼저 막는다. 단 제출 밀도가 오르면 M이 커져 I/O µs/msg가 내려갈 수 있다(가설, §0.2) — 상한 65k·241k는 **현재 M에서의 값**이다.

표기: 효과는 셀별 가설(µs/msg 또는 처리량), "I/O 상한 밖"은 I/O thread가 막는 동안 처리량에 드러나지 않는 항목.

| 번호 | 방안 | 출처 | bp 1 KiB | bp 4 KiB | send 1 KiB | send 4 KiB | serial | 상한 정하는 thread | 스펙 변경 | 위험 | I/O 상한 밖? |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1~7 | canonical record, claim·batch 삭제, dispatcher 삭제, coroutine coallocate, receiver 연결, header cache, batch publication(R §7) | R §7, D8 §1 | caller −10.3~−11.8, target −2~−5 | 같음 | caller −2.2~−2.7, target −1~−2 | 같음 | RTT −3~−7 µs(126 µs의 3–5 %) | caller→I/O로 이동 | 없음(1단계 #4155 머지 대기, 2단계 진행) | 낮음 | **예**(bp: 57k→65k까지만 보임, +14 %) |
| A | wire: correlation·deadline은 frame 0, parts는 native frame | D8 §4 A | caller −0.63~−1.00, completion −0.58~−0.90, target −1.21~−1.90 | +0.38 추가 | caller −0.37~−0.52 | +0.19 | 작음 | caller | W §2·MM §5·schema, 네 언어 | wire major; **frame 2→3**이면 Core의 frame당 비용(decode·pipe write·encode)이 늘어 I/O µs/msg +0.3~+0.6 가설 | 예(I/O에는 역효과 가능) |
| B1 | 완료 continuation을 binding 완료 자원에서 실행 | D8 §4 B1 | source −0.9~−2.2 CPU; **source 인계 지연 −5~−20 µs 벽시계 → 다음 request 제출 밀도 ↑ → source I/O M ↑(N1)** | 같음 | — | — | RTT −5~−20 µs | source I/O·caller | S08 §4.4·S01 §11 개정 | S02 §13 사례 약화 | **아니오**(I/O에도 효과, 가설) |
| B2 | binding drain 경계 통지로 wake 합치기 | D8 §4 B2 | source −(0.8–2.0)(1−1/K_c′) | | | | | source | binding 스펙 | 작음 | 예 |
| C | pending authority를 binding entry 하나로 | D8 §4 C | caller −0.35~−0.65 | | | | | caller | binding 스펙·계층 경계 | 최대 | 예 |
| H44 | **수신 역할 인계(S08 §4.4)**: target handler가 recv 직후 같은 자원에서 실행 → reply가 요청 read 직후 pipe에 들어감 | S08 `:300-316`, CN B6′ | **target I/O M ↑(N1)**: reply가 같은 깨어남에 들면 target I/O 17.05→≈W_f/1.75≈11 µs(가설); target wake 1회 절감 | 같음 | K≈3,316이라 효과 0 | 0 | RTT −2~−10 µs | **target I/O** | 없음(§4.4가 이미 허용) | 인계 실패 시 현재 경로 유지 조건(S08 `:314-316`) | **아니오** |
| N1-Core | Core 깨어남 고정 비용: owner를 engine과 같은 I/O thread에 두거나 inbound 통지(asio post + eventfd)를 합치기 | §2 N1, §3 | I/O −3~−6 µs/msg(가설: syscall 2회·thread 1회 전환 절감) → 상한 65k→≈85–105k | 같음 | source I/O 4.15→≈3 | | RTT −5~−10 | **I/O** | Core 설계(hot path §2 표 갱신, `KD/systems/10-hot-path.ko.md:149-152`) | Core 변경·gate(§5.1 hotpath_gate) | **아니오** |
| N2-Core | poller 통지 중복 제거(primary eventfd write를 batch당 1회) | §2 N2 | target I/O −1.5~−4 | 같음 | target I/O 소폭 | | 작음 | target I/O | Core(05-polling wake 규칙 유지) | 낮음 | **아니오** |
| N3-Fw | flow state 전환 빈도 확인, 잦으면 hysteresis | §2 N3 | 측정 전 0 | | | | | — | 없음 | 없음 | 측정 뒤 |
| N4-Fw | `pollout` 등 mask 축소·modify 빈도 | §2 N4 | poller thread −0.1~−0.5/poll | | | | | ingress thread | 없음(네 언어 동시) | 없음 | 예(ingress는 상한 아님) |
| A1 | send 벤치 thread 8개 | CN §7 A1 | — | — | 측정 조건 동등화; 제출 밀도 ↑로 I/O M ↑ | | — | — | README | 벤치 동등성 결정 | — |
| A2 | 벤치 reply 소비를 completion thread에서 | CN §7 A2 | caller −0.6~−1.3 | | | | | caller | 없음(벤치) | B1과 중복 | 예 |
| V8/#4145 | handler singleton → 계약대로 생성 | D8 D5 | target +0.18~+0.35(**증가**) | | | | | target worker | 없음 | 측정 전 수정 | 예 |

## 5. 적용 가능성 — 승인된 1~7단계와의 관계

| 방안 | 1~7단계와의 관계 | 판정 |
|---|---|---|
| N1-Core, N2-Core | Framework 코드와 무관. Core hot path(`KD/systems/10-hot-path.ko.md:33-43`) 변경이며 hotpath_gate·release gate 대상 | **별도 결정**(Core 소유자 이슈). 1~7단계와 병행 가능 |
| H44 수신 역할 인계 | 7단계(batch publication·wake 예약)와 같은 자리(`mesh_node_host_service.cpp:3354-3362`). R §4.2는 "인계가 가능하면 쓰되 절감 근거로 가정하지 않음" | **7단계 범위에 포함 가능**, 스펙이 이미 허용. I/O 효과 가설이 새 근거이므로 2단계 뒤 target I/O를 재면서 결정 |
| B1 | 7단계의 completion 쪽 대체(D8 §6.3) | 스펙 개정 필요, 1~7과 충돌 없음. I/O 효과(source M)가 새 근거 |
| A | 6단계 cache 조건 확장(D8 §6.3) | frame +1의 I/O 비용을 §6 P1에서 먼저 잰 뒤 |
| C | 5단계 위 | 보류 유지(D8 D4) |
| N3-Fw·N4-Fw | 1단계 canonical record와 같은 파일(`raw_route_port.cpp`) | 측정 뒤, 네 언어 동시 |
| 1~7 | — | 계속. 단 bp 1 KiB에서는 I/O 상한 때문에 처리량으로는 +14 %까지만 드러난다. **판정은 처리량이 아니라 thread별 CPU(caller·target worker µs/msg)로** 해야 한다 |

## 6. 측정 계획

### P1. perf — I/O thread 깨어남 분해 (가장 먼저, 같은 세션)

- 대상: request-backpressure 1 KiB, 22k 동일 처리율(X와 같은 pacing), binding·Framework 각 1회 + 10k paced 각 1회. 추가로 send 1 KiB 10k paced 1회(N1·N2가 request 경로에만 있는지 대조).
- thread: source·target의 활성 Core I/O thread TID 전부(engine과 async owner를 구분하기 위해 TID별로 따로 집계).
- 이벤트(모두 TID 필터): `perf stat -e syscalls:sys_enter_epoll_wait,sys_enter_read,sys_enter_recvmsg,sys_enter_write,sys_enter_writev,sys_enter_sendmsg,sys_enter_futex,context-switches` → **메시지당 횟수**. `perf record -e cycles -F 4999 --tid=...`로 self time(symbol만; `-g`가 없으므로 callgraph는 frame pointer 빌드 1회가 필요하면 그때 추가). uprobe 계수: `zlink::signaler_t::send`, `zlink::mailbox_t::send`, `zlink::pipe_t::process_activate_read`, `zlink::socket_base_t::process_async_mailbox`, `asio_engine_t::on_read_complete`, `zlink::socket_base_t::set_local_receive_flow_state`, `consume_receive_flow_state_frame`, `socket_poller_t::rebuild`.
- 판정:

| 결과 | 뜻 | 다음 행동 |
|---|---|---|
| `epoll_wait`·`on_read_complete`/msg가 Framework ≥ 1.3×binding | N1(깨어남 수) 확정 | H44 인계를 7단계 안에서 target에 먼저 적용해 target I/O 재측정; Core에 N1-Core 이슈(깨어남 고정 비용, 소유: Core) |
| `write`(eventfd)/msg가 Framework target ≈2, binding target ≈0, source 양쪽 같음 | N2 확정 | Core 이슈(N2-Core) |
| 두 수가 같고 `process_async_mailbox`·`process_commands`·`mailbox_t::send` self time이 Framework에서 큼 | Core 내부의 request/reply 경로 비용(b) | Core 소유자에 profile 전달, `process_completion_records`·`checkout_router_reply_target` 분해 |
| `set_local_receive_flow_state` ≥ 수백/s | N3 | Framework 임계값 hysteresis(네 언어) |
| 아무 것도 다르지 않음 | 측정 오류 | pacing 구현·affinity·turbo 확인 뒤 재측정 |

### P2. 이분 실험 — binding 벤치로 Framework 사용 형태를 하나씩 재현 (kairos, 벤치 코드만)

Core·Framework를 바꾸지 않고 `P/zlink/bench_zlink_cpp_server.cpp`·`P/client/bench_cpp_client.cpp`에 변형을 하나씩 더해 I/O µs/msg가 7.4에서 17로 움직이는 단계를 찾는다. 각 1회, 22k paced.

| 단계 | 변형 | 재현하는 후보 |
|---|---|---|
| S1 | server: poller(`pollin\|pollout\|pollroute\|pollconnect\|pollcompletion`) + `recv(DONTWAIT)` | N2·N4 |
| S2 | S1 + reply를 둘째 thread로 넘겨 제출(queue + condvar) | **N1(target)** |
| S3 | S2 + 200건마다 `set_receive_flow_state` 토글 | N3 |
| C1 | client: poller mask에 `pollin\|pollout\|pollroute\|pollconnect` 추가 | N4(source) |
| C2 | C1 + completion 소비를 둘째 thread로 넘기고 그 뒤 다음 제출 | **N1(source)** |

### P3. 1~7단계 전·후 같은 세션

D8 §6.1 D1 그대로: 1~7단계 전·후 각 1회 thread별 CPU(caller·worker·I/O)와 P1의 계수. 판정 기준은 처리량이 아니라 **thread별 µs/msg**(§5 마지막 행).

## 7. 감독자가 결정할 것

| 번호 | 선택지 | 추천 |
|---|---|---|
| D1 | P1(perf)과 P2(이분)의 순서 | **P1 먼저**(1세션, 빌드 불필요), S2·C2만 P2에서 바로 확인 |
| D2 | Core 이슈(N1-Core·N2-Core) 등록 시점 | P1 결과와 함께 등록. Core 소유 결정이며 Framework에서 보상하지 않는다(AGENTS.md `:59-63`) |
| D3 | H44(수신 역할 인계)를 7단계에 포함할지, 별도 단계로 둘지 | **7단계에 포함**하되 target I/O 재측정을 완료 조건으로 |
| D4 | A(wire)의 frame +1 | P1의 frame당 비용 확인 뒤 설계 확정(D8 D2 유지) |
| D5 | bp 1 KiB 판정 기준 | I/O 상한이 풀리기 전에는 처리량 비율(F÷B)이 아니라 thread별 CPU로 1~7단계를 판정 |

---

**요약(5줄)**
1. 같은 22k에서 Framework의 Core I/O thread 비용(17.05/17.95 µs)은 binding(7.42/7.35)의 2.3–2.4배이고, 활성 I/O thread 둘(engine·async command owner) 모두 1.7–2.4배다. 10k paced send는 양쪽 ≈12 µs로 같다.
2. 코드 대조에서 Framework가 Core에 더 시키는 메시지당 일은 거의 없다 — 옵션·frame 수·timeout·correlation은 원인이 아니다. 차이는 (N1) recv와 reply, completion과 다음 제출이 다른 thread·다른 깨어남에 떨어져 I/O thread가 메시지마다 고정 비용 12–20 µs를 다시 내는 것과 (N2) target의 poller 통지 eventfd write 2회(binding server는 0)다. N1은 계약(S08 §4.4·S01 §11) + Core 깨어남 비용(b), N2는 Core(b)다.
3. 1~7단계는 caller를 57k→140k+로 올리지만 I/O 상한 ≈65k(현재 M 기준)가 바로 막아 bp 1 KiB 처리량은 +14 %까지만 보인다. 0.6에는 I/O ≤6.7 µs/msg, 즉 binding 수준의 깨어남당 메시지 수가 필요하다.
4. I/O 상한에 효과가 있는 방안은 H44 수신 역할 인계(스펙 허용, target I/O 17→≈11 가설), B1(source 제출 밀도), Core의 깨어남 경로(N1-Core·N2-Core)뿐이다. A(wire)는 frame +1로 I/O에 역효과 가능성이 있어 측정 뒤로 둔다.
5. 다음 측정은 I/O thread TID별 `epoll_wait`·`write`·`futex` 횟수/msg와 `on_read_complete`/msg(perf stat·uprobe) 1세션, 그리고 binding 벤치에 "reply를 둘째 thread로" 변형(S2·C2)을 더한 이분 실험이다.
