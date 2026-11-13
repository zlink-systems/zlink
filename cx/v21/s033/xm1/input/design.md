# C++ Framework 송신 묶음 저하 — 원인 코드 대조와 수정 설계

읽기 전용 분석이다. 코드·스펙·빌드·측정·git 변경은 없다. 측정값은 출처를 적고, 추정은 **가설**과 계산식으로 표시한다.

## 0. 기준과 표기

| 항목 | 값·출처 |
|---|---|
| 저장소 | `D:/worktree/zlink-fable-design`, `39bbfe470d`(읽기 전용) |
| 측정 | `p1-result.md`(이하 P1), `aff-analysis.txt`(AF), `s2c2-analysis.txt`(S2C2), `jfable-syn/xio-report.md`(X). 모두 request-backpressure 1 KiB, 약 22k msg/s, 각 1회 |
| 이전 문서 | `jfable-syn/plan.md`(PL), `jfable-08/design.md`(D8) |

경로 접두어. `K`=`core/src/`, `KD`=`core/doc/spec/core/`, `B`=`bindings/cpp/src/Runtime/`, `C`=`framework/languages/cpp/framework/src/runtime/`, `P`=`framework/bench/grpc/cpp/`, `S01`·`S08`=`framework/doc/framework/common/spec/server/01-execution/{01-submit-and-completion,08-messaging-hot-path}.ko.md`.

## 1. 측정값 읽기

### 1.1 sendto/msg를 "짝 비율"로 바꾼다

한 sendto에 메시지가 1개 또는 2개만 든다고 두면(§2의 규칙상 22k에서는 2개를 넘는 경우가 드물다 — 가설), 짝으로 나간 메시지 비율 p와 sendto/msg s의 관계는 `s = (1 − p) + p/2`, 즉 `p = 2(1 − s)`다.

| thread | s (sendto/msg) | p (짝 비율) | 출처 |
|---|---:|---:|---|
| binding target IO/1, source IO/0 (기본·S2·C2·S2C2) | 0.498–0.501 | **1.00** | S2C2:13-14, 28-29, 44-45, 58-59 |
| Framework target IO/1 | 0.705 | **0.59** | S2C2:75 |
| Framework target IO/1 (다른 회차) | 0.575 | 0.85 | P1:11 |
| Framework source IO/0 | 0.643 | **0.71** | S2C2:76 |

binding은 네 변형 모두에서 소수점 셋째 자리까지 0.50이다. 통계적 우연이 아니라 구조가 만드는 값이다(§4.3). Framework는 회차에 따라 0.575–0.705로 흔들린다(각 1회). 설계는 0.705 한 값에 맞추지 않는다.

### 1.2 target IO/1의 +60 %를 세 인자로 나눈다 (가설, S2C2 perf 부착 실행)

perf stat의 `cycles = instructions / IPC`로 두고 `주파수 = cycles / CPU 시간`을 구한다.

| | instr/msg | IPC | cycles/msg | CPU µs/msg | 유효 주파수 |
|---|---:|---:|---:|---:|---:|
| binding target IO/1 | 18,987 | 0.62 | 30,624 | 10.19 | 3.01 GHz |
| Framework target IO/1 | 23,208 | 0.55 | 42,196 | 16.35 | 2.58 GHz |
| binding target IO/0 (수신) | 18,391 | 0.64 | 28,736 | 9.66 | 2.97 GHz |
| Framework target IO/0 (수신) | 16,487 | 0.57 | 28,925 | 11.85 | 2.44 GHz |
| binding source IO/0 (송신) | 18,813 | 0.62 | 30,344 | 10.33 | 2.94 GHz |
| Framework source IO/0 (송신) | 22,005 | 0.51 | 43,147 | 16.65 | 2.59 GHz |

출처 S2C2:57-59, 74-76. 비율 16.35/10.19 = 1.605 = 명령어 1.222 × IPC⁻¹ 1.127 × 주파수⁻¹ 1.166. 로그 비중은 명령어 43 %, IPC 25 %, 주파수 32 %다. **수신 thread(target IO/0)는 명령어가 10 % 적은데 cycles가 같고 시간만 22 % 길다 — 주파수만의 차이다.** AF의 pinned 실행에서는 Framework IO/0이 18,610/0.55/10.81 = 3.13 GHz, IO/1이 25,473/0.56/16.35 = 2.78 GHz로 올라 차이가 줄었다(AF:68-69; binding pinned는 AF:23-24에서 3.04·3.18 GHz; 각 1회). 이 분해는 perf의 `cycles`가 core clock(ref-cycles가 아님)이라는 전제에 선다. §7 M2가 확인한다.

### 1.3 추가 sendto 한 번의 비용 (가설)

명령어 증가분을 sendto 증가분으로 나눈다. S2C2: (23,208 − 18,987) / (0.705 − 0.500) = **20,590 instr/sendto**. P1(§1·§2, 다른 회차): (18,756 − 17,404) / (0.575 − 0.445) = **10,400 instr/sendto**. loopback TCP의 송신 경로는 송신 syscall, netfilter, `loopback_xmit`, 같은 CPU에서 처리되는 수신 softirq(`tcp_v4_rcv`)와 상대 I/O thread 깨우기를 포함하므로(P1:22-23 symdiff) 1–2만 명령어는 타당한 범위다. 시간으로는 10,400–20,590 / (0.55 × 2.58 GHz) = **7.3–14.5 µs/sendto**. 따라서 p를 1.0으로 되돌릴 때 target IO/1 절감은 (0.705 − 0.5) × 7.3–14.5 = **1.5–3.0 µs/msg**(0.575 기준이면 0.5–1.1)다. X의 차이 9.63 µs(17.05 − 7.42, X:18-19) 중 16–31 %다. 나머지는 §1.2의 주파수(≈ 17.05 × 0.14 ≈ 2.4 µs)와 IPC·cache(§4.5)다.

## 2. Core의 송신 묶음 규칙 — 한 sendto에 무엇이 드는가

Core에는 "n개를 모아 보낸다"는 규칙이 없다. 규칙은 하나다: **engine이 pipe를 비우는 순간 pipe에 있던 메시지 전부(8 KiB 한도)가 한 sendto가 된다.** 경로는 다음과 같다.

| 단계 | 코드 | 내용 |
|---|---|---|
| 1 | `K/runtime/core/pipe_write.cpp:1426-1469` `flush_unlocked` | `_out_pipe->flush()`가 false(reader가 잠듦)일 때만 `send_activate_read(peer)`. ypipe의 판정은 `K/runtime/core/ypipe.hpp:76-91`(c==NULL이면 reader sleeping) |
| 2 | `K/runtime/core/object.cpp:389` → `K/runtime/core/mailbox.cpp:70-101`, `338-362` | command를 I/O thread mailbox에 쓰고 `boost::asio::post`(executor 있음). I/O thread가 `run_for`에서 자고 있으면 asio가 깨운다 |
| 3 | `K/runtime/engine/asio/asio_poller.cpp:450-465`, `KD/systems/03-io-thread.ko.md:70-78` | I/O thread는 `poll()`로 준비된 handler를 한 번에 처리하고, 없으면 `run_for(≤100 ms)` |
| 4 | `K/runtime/core/pipe_transport.cpp:51-69` `process_activate_read` → `K/runtime/core/session_base.cpp:416-430` `read_activated` → `K/runtime/engine/asio/asio_engine.cpp:1629-1643` `restart_output` | `_output_stopped=false` 뒤 `speculative_write()` |
| 5 | `asio_engine.cpp:1478-1529` `speculative_write` → `1314-1358` `prepare_output_buffer` | `while (prepared_size < target_out_batch)`로 `_next_msg`(`K/runtime/core/session_base_pipe_io.cpp:11-25` `pull_msg` → `pipe_t::read`)를 반복해 encoder buffer에 채운다. pipe가 비면 `pipe_t::read_internal`이 `_in_active=false`로 reader를 재운다(`K/runtime/core/pipe_receive.cpp:259-261`) |
| 6 | `asio_engine.cpp:1528-1531` `write_some` | **동기 sendto 1회** |
| 7 | `asio_engine.cpp:1581-1618` `while (prepare_output_buffer ()) { write_some }` | pipe에 메시지가 더 있으면 **그때마다 sendto 1회 더**. 비면 `_output_stopped=true`(`:1616`) |
| 한도 | `output_target_batch`(`asio_stream_fastpath_policy.hpp:403-411`)는 TCP ZMP에서 `out_batch_size`=8192(`K/runtime/core/options.cpp:76,130`; `asio_engine.hpp:337-341`의 adaptive는 STREAM·message-boundary 전용) | 1.4 KiB 메시지 5개 |

따라서 메시지 m1, m2가 한 sendto에 들려면 **m2의 flush가 m1의 drain(5단계의 `pull_msg` 실패 시점)보다 앞서야** 한다. 그 창 W는 I/O thread가 자고 있으면 "asio post → 깨어남 → handler 실행"의 지연이고, 깨어 있으면 "현재 handler 종료 + queue 순서"다. 5단계 뒤에 도착한 m2는 다시 1단계부터 시작해 7단계에서 **별도 sendto**가 된다. 2개를 넘는 묶음은 W 안에 3개 이상이 들어올 때만 생긴다.

Nagle은 끼어들지 않는다: `tcp_nodelay` 기본 1(`K/runtime/core/options.cpp:116`, `K/runtime/transports/tcp/tcp.cpp:30-36`)이라 drain 하나가 곧 segment 하나다. gather 경로는 환경 변수로만 켜지고 임계값 64 KiB다(`asio_stream_fastpath_policy.hpp:14-27`). 어느 쪽도 두 구현에 같다.

## 3. 제출 경로 대조 — Framework와 binding 벤치가 Core에 넣는 방식

| 항목 | C++ Framework | C++ binding 벤치 | 같은가 |
|---|---|---|---|
| request API | `C/backend/raw_route_port.cpp:388-405` `_socket->request(rid).message(p0).message(p1).timeout(t).async()` → `B/Messaging/operation_submit.hpp:130-200` `zlink_request(parts=2, DONTWAIT, timeout)` | `P/client/bench_cpp_client.cpp:636-642` 같은 builder·`.async()` | **같다** |
| request의 Core 쓰기 | `K/api/socket/socket_request_reply_submit_api.cpp:172-211` `submit_pull_dontwait_request` → `request_admission_submit` → part마다 `write_*`, 마지막 part(`!more`)에서만 `flush_unlocked`(`pipe_write.cpp:115-124`). `try_write_complete_record_and_flush`(`:851-903`)는 PAIR 전용(`K/runtime/sockets/pair/pair.cpp:129`) | 같음 | **같다**: 메시지당 flush 1회, activate_read ≤ 1회 |
| reply API | `raw_route_port.cpp:515-537` `_socket->reply(rid, token).message(p0).message(p1).submit()` → `B/Messaging/reply_operations.cpp:16-50` `zlink_reply(parts=2)`(blocking) | `P/zlink/bench_zlink_cpp_server.cpp:62` `received.reply().message(header).message(echo).submit()` | **같다** |
| reply의 Core 쓰기 | `socket_request_reply_submit_api.cpp:472-515` `public_router_reply_submit` → `K/api/socket/socket_request_reply_runtime_io.cpp:1436-1458`: 앞 part는 `write`, 마지막 part만 `write_and_flush` | 같음 | **같다**: flush 1회 |
| pipe | 연결 1개(PL §1 "연결 수")의 session pipe 하나 | 같음 | 같다 |
| request 제출 thread | 벤치 thread에서 `_lane.run_sync`(`C/mesh/raw_mesh_node_owner.cpp:1536-1537`; `C/execution/state_lane.hpp:289-346`는 호출 thread에서 inline 실행) → `_socket_mutex`(`raw_route_port.cpp:390`) | 벤치 thread(coroutine slot, `P/client/bench_cpp_client.cpp:628-657`) | thread 1개로 같다. lane·mutex 대기는 정상 경로에서 0 |
| reply 제출 thread | **offload executor worker**: ingress thread가 record를 service mailbox에 넣고(`C/mesh/mesh_node_host_service.cpp:2301`), owner가 ready가 되면 `try_submit_internal`(`:3356-3361`) → worker가 `drain_application_owner`(`:3861-3915`)로 **owner당 하나씩 순차**(`C/mesh/service_mailbox.cpp:453-461` draining 배타) 처리, 10 ms 예산(`C/dispatch/dispatch_limits.hpp:15`) 뒤 다른 worker로 넘어갈 수 있음. reply는 handler turn 안에서 `runtime::host::reply`(`C/mesh/mesh_record_dispatcher.cpp:61-66`) | **recv와 같은 thread**에서 즉시(`bench_zlink_cpp_server.cpp:41-66`) | **다르다**: 제출 thread가 바뀌고(S2 변형이 재현, +1.2 µs: S2C2:28 대 58), 무엇보다 **연속 두 reply 사이 간격**이 다르다(§4.1) |
| 수신 | ingress poller thread, poll당 record 1건 `recv(DONTWAIT)`(`raw_route_port.cpp:475-503`), 회전당 ≤64건(`dispatch_limits.hpp:10`, `mesh_node_host_service.cpp:2188`) | blocking `recv` 루프 | Core 수신 경로 같음 |
| 메시지 모양 | 2 frame, 13 + 1,388.65 B(X:606) | 2 frame, 201 + 1,027 B(X:604) | §4.4 |

결론: **Core에 넣는 방식(API·flags·multipart·flush 횟수·pipe)은 메시지 단위로 완전히 같다.** 다른 것은 두 가지뿐이다 — (1) 같은 묶음으로 도착한 두 메시지에 대한 **두 제출의 시간 간격**, (2) 제출 thread의 수와 배치.

## 4. 원인 판정

### 4.1 target: reply 간격이 Core의 묶음 창 W를 넘는다

- binding server: 같은 packet의 요청 2개는 한 read에서 함께 decode되어 socket pipe에 들어가고(`asio_engine.cpp:1270-1311`, flush 1회), server thread는 `recv → parse → encode → reply → recv → reply`를 한 thread에서 돈다. 메시지당 app CPU 5.8–6.3 µs(S2C2:52, AF:6)이므로 **reply1과 reply2의 간격 ≈ 5–6 µs**. I/O thread가 activate_read로 깨어 drain하기까지의 W(수 µs–수십 µs, 공유 CPU에서는 runqueue 지연 포함)보다 짧아 **거의 항상 묶인다**(p = 1.00).
- Framework target: 같은 두 요청은 ingress thread가 받아 mailbox에 넣고, worker 하나가 **순차로** handler turn을 돈다(§3 "reply 제출 thread"). worker CPU 합은 11.48 + 11.20 = **22.7 µs/msg**(AF:30-31)이므로 **reply1과 reply2의 간격 ≈ 20 µs 이상**이다. 간격이 W보다 길면 reply1이 먼저 drain되어(§2 5–7단계) reply2는 별도 sendto가 된다. 측정 p = 0.59–0.85는 "W가 20 µs를 넘는 회전이 59–85 %"라는 뜻이다(가설). W가 회차·CPU 공유 상태에 따라 달라지므로 p도 흔들린다(§1.1).
- 두 조건은 S2 변형이 둘을 가른다: binding에 "reply를 둘째 thread로"를 더해도 두 reply는 같은 queue batch에서 연속 제출되므로 간격이 그대로 짧고 p = 1.00을 유지했다(S2C2:28). 즉 **thread 전환 자체가 아니라 제출 간격**이 원인이다.

### 4.2 source: 도착 짝 + 완료 경로

- source의 두 request가 묶이려면 두 완료가 함께 와야 한다. 완료 두 개가 한 packet으로 오면 ingress thread가 한 poll에서 둘을 drain하고(`C/backend/raw_binding_adapter.hpp:147-199`) 각각 `post_completions`(`C/foundation/operation_registry.cpp:269-290`) → `dispatch_completion`(`C/dispatch/provider_task_context.cpp:174-191`, dispatcher `post`) → 벤치의 `completion_signal->notify` → 벤치 thread는 한 번 깨어 `take_completed`로 둘을 받고(`P/client/bench_cpp_client.cpp:921-923`), 루프 한 바퀴에 request **하나**를 제출한다(`:925-951`, `if (can_submit)`이지 `while`이 아님). 두 번째 제출은 완료 처리(`:953-991`) 뒤 다음 바퀴에서 나오므로 간격은 수 µs다. admission 게이트(`:915-917`)는 `.async()`가 OK를 돌려주면 inline으로 즉시 완료되므로(`raw_binding_adapter.hpp:163-183`, `admission` 없음 → `admitted->complete(success)`) 간격을 늘리지 않는다.
- 따라서 **source의 p(0.71)는 target의 송신 짝(0.59–0.85)에 묶여 있다.** 도착이 짝이 아니면 제출도 짝이 될 수 없다. target을 고치면 source는 따라온다. binding C2 변형(완료를 둘째 thread로)이 p = 1.00을 유지한 것(S2C2:14)도 같은 이유다 — 둘째 thread가 batch로 받는 한 간격은 짧다.

### 4.3 binding의 "정확히 2개"는 pacer가 만든다 (가설, 확인 필요)

X:627은 pacing을 "bench client에만 추가한 `--target-rate`"로 적었고 저장소의 `P/client/bench_cpp_client.cpp`에는 그 코드가 없다(grep `target_rate` 0건). 따라서 pacer의 제출 구조는 코드로 확인할 수 없다. 숫자로 보면: unpaced binding은 253k(X:9)인데 상한 32k에서 실제 22k가 나왔고 벤치 thread의 wchan이 `hrtimer_nanosleep`이다(X:95). nanosleep은 일반 thread에서 timer slack 50 µs를 더하므로 slot 31 µs(32k)로 자면 깨어날 때 **slot 2개가 함께 만기**가 되고, 2개/≈91 µs = 22k가 된다. 이 가설이 맞으면 **"2개씩 묶임"은 Core나 binding의 성질이 아니라 pacer의 제출 burst**이고, Framework도 48k 상한(slot 20.8 µs)에서 같은 burst를 받았을 것이다. 결론은 변하지 않는다 — 양쪽 모두 source에서 2개씩 넣기 시작했고, **Framework target의 handler 간격만이 짝을 깬다.** 확인은 §7 M3.

### 4.4 메시지 크기·frame 배치 (질문 2)

| 항목 | 값 | 영향 |
|---|---|---|
| wire byte/msg (가설: ZMP frame header 2 B(<256 B)·9 B(이상)) | binding 201+2 + 1,027+9 = 1,239; Framework 13+2 + 1,388+9 = 1,412 (+14 %) | sendto당 kernel 복사 +14 %. 횟수 차이(+41 %)보다 작다 |
| 송신 batch 8192 B | binding 6개, Framework 5개까지 한 sendto | 22k에서 묶음은 2개 수준이라 한도에 닿지 않음 |
| 수신 read target | `decoder_initial_read_target` = min(in_batch 8192, cap 4096) = **4096 B**(`asio_stream_fastpath_policy.hpp:221-223, 246-256`); 성장은 STREAM 전용(`:358-360` `can_grow_stream_target`) → ROUTER는 4096 고정 | binding은 3개(3,717 B), Framework는 **2개**(2,824 B; 3개면 4,236 B)까지 한 recvfrom. 3개 이상 burst에서만 Framework가 read 1회 더. 지금의 p≤1 짝 수준에서는 영향 없음 |
| VSM | `max_vsm_size` 29 B(`K/runtime/core/msg.hpp:227, 410`) | Framework 13 B envelope은 inline(malloc 0), binding 201 B header는 malloc. Framework가 방향당 malloc 1회 적다. 역방향 영향 |
| TCP MSS(loopback 65,495) | 2개 모두 segment 하나 | 없음 |

판정: **바이트·frame 배치는 sendto 횟수 차이의 원인이 아니다.** sendto당 비용을 +14 % 올리는 정도다.

### 4.5 IPC 저하·cache miss (질문 3)

| 후보 | 코드 근거 | 판정 |
|---|---|---|
| 추가 sendto의 kernel 코드 혼입 | §1.3. kernel TCP 경로의 IPC를 0.40으로 두면 blended IPC = 23,208 / (18,987/0.62 + 4,221/0.40) = **0.56** ≈ 측정 0.55 | **IO/1의 IPC 저하는 sendto 증가로 설명된다**(가설, 계산 일치) |
| 유효 주파수 저하 | §1.2. target에서 Framework는 busy thread 5개(AF:28-32), binding은 3개(AF:4-6); source는 Framework 12개(X:560-569의 1.7 µs thread 8개 포함) 대 3개 | 활성 core 수가 많을수록 all-core turbo가 낮다(가설). **수신 thread의 +22 %는 전부 여기서 나온다**(cycles 동일). pinned에서 차이가 줄었다(AF) |
| 다른 core가 만든 buffer를 I/O thread가 읽음 | 송신: Framework는 `external_message_t`로 worker가 만든 `raw_bytes_t`를 zero-copy 참조(`raw_binding_adapter.hpp:211-221`), binding은 app thread에서 `message_t::from` 복사. 둘 다 "다른 core가 쓴 1.4 KiB(22 cache line)를 I/O thread가 읽고 해제". 수신: decoder가 I/O thread에서 할당, 소비 thread가 해제 — 양쪽 같음 | **구현 간 차이 없음**. Framework는 producer core가 worker마다 바뀌지만 line 수는 같다 |
| CPU 공유·migration | migration/msg 0.0028 → 0.0150(IO/1), cache-miss 953 → 1,522(S2C2:58,75); pinned에서 cache-miss 1,580 → 1,167, IPC 0.55 그대로(AF:47,69) | cache miss의 일부는 공유에서 오나 **IPC에는 거의 영향 없음**. 7 % 절감(브리프) |

정리: IPC·cache 항목 중 코드로 구분되는 Framework 고유 원인은 없다. "같은 일을 더 느리게" 하는 몫은 (i) 추가 sendto의 kernel 명령어, (ii) 활성 thread 수에 따른 주파수다. 둘 다 §4.1·§4.2의 제출 구조와 thread 배치에서 나온다.

## 5. 소유 계층 판정 (질문 4)

근거 조항: AGENTS.md `:59-63`(계층 소유권), `:64-67`(금지 패턴), `:68-70`(교차언어), `:72-78`(규칙 수), `KD/08-runtime-boundary.ko.md:25-32, 78-80`(Framework는 binding 공개 raw API만), `KD/systems/10-hot-path.ko.md:34-42`(Core hot path 표, 마지막 행 "pipe_t::flush → activate_read"), `KD/systems/03-io-thread.ko.md:70-78`(I/O 루프), `KD/05-polling.ko.md:84-93`(poller 깨어남 규칙), S08 §4.4 `:300-316`, S01 §11 `:490-513`.

| 사실 | 소유 계층 | 분류 | 판정 |
|---|---|---|---|
| 한 sendto = drain 시점 pipe 내용(§2) | Core(engine·pipe) | 계약대로 동작 | **결함 아님. Core 수정 없음.** 모으기 위한 지연·timer는 금지 패턴(AGENTS.md `:64-67`)이고 latency를 올린다 |
| activate_read 통지 경로(eventfd·asio post) | Core | 양쪽 동일 | 이 문제의 원인 아님. PL N1-Core·N2-Core는 별개의 Core 결정으로 남긴다 |
| reply 간격 = handler turn 비용 × owner 순차 drain(§4.1) | **Framework** | (a) Framework 사용 + 일부 (c) 계약(owner 순서는 S08 §4.4 "owner queue") | 순차성은 유지, **turn 비용을 줄이는 것**이 수정이다. 하위 계층에서 보상하지 않는다 |
| request 간격 = 완료 → continuation → 다음 제출(§4.2) | Framework + 벤치 구조 | (a) + 측정 조건 | target이 고쳐지면 따라온다. 벤치 두 driver의 제출 구조 차이는 측정 조건 항목(§6 F4) |
| 활성 thread 수 → 주파수(§4.5) | Framework thread 배치 | (a) | H44·B1이 다루는 항목 |
| envelope 13 B·payload +14 %(§4.4) | 계약(wire) | (c) | 영향 작음. D8 A(wire 변경)의 근거로 쓰지 않는다 |
| pacer burst(§4.3) | 벤치 | 측정 조건 | 저장소에 넣어 재현 가능하게 한다 |

## 6. 수정 설계 (질문 5)

원칙: **새 규칙을 더하지 않는다.** Core에 "모아 보내기" 규칙, Framework에 "reply를 모아 제출" queue·flag를 두지 않는다(둘 다 판정 위치 +1, 금지 패턴). 묶음은 Core의 기존 규칙 하나가 이미 한다. 고칠 것은 **그 규칙이 묶을 수 있도록 제출 간격을 줄이는 것과 활성 thread를 줄이는 것**이며, 둘 다 이미 승인된 항목 안에 있다.

| 번호 | 고칠 곳 | 바꾸는 규칙 하나 | 판정 위치 전/후 | 다른 언어 | 예상 효과(가설) | 승인 항목과의 관계 |
|---|---|---|---|---|---|---|
| **F1** | target handler turn 비용: `C/mesh/mesh_node_host_service.cpp:3764`(`dispatch_application`) 이하 — #1639 1~7단계가 건드리는 record·claim·dispatcher·header 경로 | 규칙 변경 없음(승인된 1~7단계 그대로). 결과로 reply 간격 22 → 17–20 µs(D8의 target −2~−5 µs) | 각 단계 문서대로 | 1~7단계는 C++ 전용. 같은 메커니즘(간격 vs W)은 네 언어 공통이므로 **다른 언어는 sendto/msg를 측정해 간격 문제가 있는지부터 확인**(§7 M1을 언어별 1회) | p = P(W > 간격). W 분포를 모르므로 수치는 M1 뒤. 상한: p → 1.0이면 target IO/1 −1.5~−3.0 µs(§1.3) | 1~7단계 완료 조건에 **"target sendto/msg"를 추가** |
| **F2** | S08 §4.4 수신 역할 인계(H44): `mesh_node_host_service.cpp:2158-2365`(수신 역할) ↔ `:3356-3361`·`:3861-3915`(owner drain) | "handler turn을 어느 자원이 실행하는가"의 판정을 §4.4 규칙 하나로: 수신 자원이 인계에 성공하면 직접 실행, 아니면 현재 경로 | **2 → 1**(지금은 수신 역할의 enqueue `:2301`과 executor의 drain `:3895` 두 곳이 결정) | 네 언어 동시(S08은 공통 스펙). .NET `ZLinkManagedMeshNode.cs:7164`, Java `ZLinkMeshApplicationDispatcher.java:95,110`, Node `node-raw-mesh-backend.ts:2683-2685`가 같은 "handler turn 안 reply" 구조 | binding server와 같은 `recv → handle → reply` 구조가 되어 target busy thread 5 → ≈3. 주파수 가설이 맞으면 I/O µs × (2.58/3.0) ≈ **−14 %**(17.05 → ≈14.7). 간격 자체는 바꾸지 않으므로 p에는 중립 | PL §5의 결정대로 **7단계에 포함**. 완료 조건에 "target I/O µs/msg와 sendto/msg" |
| **F3** | source 완료 continuation(B1, D8 §4): `provider_task_context.cpp:174-191`의 dispatcher `post`를 binding 완료 자원에서의 실행으로 | 완료 callback의 실행 자원 판정 1곳(§4.4 규칙) | D8 기준 | 스펙 S08 §4.4·S01 §11 개정이 먼저(감독자). 네 언어 동시 | source의 1.7 µs thread 8개(X:560-569, 합 14 µs/msg)의 메시지당 깨어남이 사라져 활성 core ↓ → 주파수 가설이 맞으면 source I/O −14 %. 제출 간격에는 중립(§4.2) | 7단계의 completion 쪽 대체(D8 §6.3). **M2 결과 뒤 결정** |
| **F4** | 벤치: pacer 코드를 `P/client/bench_cpp_client.cpp`에 넣고, binding driver(coroutine slot `:628-657`)와 Framework driver(루프 1건 제출 `:925-951`)의 제출 구조를 맞춘다 | 측정 조건. runtime 규칙 아님 | — | — | 두 구현의 source 제출 burst를 같게 해 target 효과만 남긴다 | PL A1·A2와 같은 범주 |
| 하지 않음 | Core `speculative_write` 지연·coalescing, `out_batch_size` 변경, `tcp_nodelay` 0, read target 성장 확장, Framework reply 모아 제출 | — | — | — | 규칙 +1 또는 latency 증가. 효과도 없음(8192는 한도에 닿지 않음, §4.4) | — |

scheduler PR과의 관계: 브리프의 입력 문서에는 그 PR의 내용이 없다. 그 PR이 handler turn·completion의 실행 자원 배치를 바꾼다면 바뀌는 양은 이 문서의 두 양(제출 간격, 활성 thread 수)과 같다. 머지 판정에 **M1·M2의 지표(sendto/msg, 유효 주파수, thread별 µs/msg)**를 넣어 역행을 막는다.

처리량 상한 계산(가설, X:9 기준 target I/O 15.37 µs@53k → ≈65k): F1로 p → 1.0이면 12.4–13.9 µs → 72–81k; F2의 주파수 회복까지 더하면 10.7–12.0 µs → 83–93k. 0.6(≈150k, I/O ≤ 6.7 µs, PL §4)에는 이 둘로는 닿지 않는다. 나머지는 PL §0.2의 "깨어남당 메시지 수 M"과 handler 비용 자체다. 즉 **이 문서의 수정은 I/O 상한을 65k에서 80–90k로 옮기는 항목**이다.

## 7. 수정 전 확인 측정 (질문 6)

각 1회, 22k 동일 조건, 빌드 불필요(M3·M4 제외).

| 번호 | 무엇을 세는가 | 원인 확정 기준 | 결과별 다음 행동 |
|---|---|---|---|
| **M1** 제출 간격 vs 창 | uprobe 타임스탬프: `zlink_reply`(target worker TID), `zlink_request`(source 벤치 TID), `asio_engine_t::restart_output`·`speculative_write` 진입(I/O TID). 연속 두 제출의 간격 g와 "첫 제출 → drain" 지연 W의 분포 | binding g 중앙값 ≈ 5 µs, Framework ≈ 20 µs; Framework에서 g < W인 쌍의 비율 ≈ p(0.59–0.85) | 일치 → §4.1 확정, F1 완료 조건에 sendto/msg 추가. Framework g ≈ 5 µs인데 p < 1 → 원인은 간격이 아니라 I/O thread 쪽(§2 7단계의 두 번째 `write_some`) — `prepare_output_buffer` 호출 수/msg를 세어 재판정 |
| **M2** 주파수 | perf stat `cycles,ref-cycles,instructions` I/O TID별 + `/proc/cpuinfo` MHz 1 s 간격 | `cycles/ref-cycles` 비가 binding 대비 Framework에서 10 % 이상 낮음 | 낮음 → §1.2 확정, F2·F3의 효과 근거. 같음 → 수신 thread의 +22 %는 cache·memory stall이며 `perf c2c`로 공유 line을 찾는다 |
| **M3** pacer burst | pacer 소스 확보 + 벤치 thread wake당 `zlink_request` 호출 수 히스토그램(두 구현) | 양쪽 모두 2개/wake가 다수 | 같음 → §4.3 확정, F4로 저장소에 고정. 다름 → source 비교를 F4 뒤 재측정 |
| **M4** binding으로 간격 재현 | S2 변형의 둘째 thread에 reply마다 20 µs 작업(또는 busy loop)을 넣어 간격만 늘린다 | target sendto/msg 0.50 → 0.6–0.75 | 상승 → "간격이 원인"이 Framework 코드 없이 확정. 불변 → Framework 고유 원인 재조사(§4.5 후보 재배열) |
| **M5** source의 1.7 µs thread 8개 | TID → stack 1회 샘플(`perf record -g` 짧게) 또는 thread 이름 부여 | completion dispatcher worker인지 executor worker인지 | dispatcher → F3 근거. executor → H44/7단계의 배치 결정에 반영 |

M1·M2·M5는 한 세션에서 함께 돌린다. F1(1~7단계)은 M1 결과와 무관하게 계속한다. F2는 M2 뒤, F3은 M2·M5 뒤 결정한다.

## 8. 감독자가 결정할 것

| 번호 | 선택지 | 추천 |
|---|---|---|
| D1 | sendto 차이의 분류: (가) Framework 제출 간격(a) + 계약(c), Core 결함 아님 / (나) Core에 묶음 규칙 추가 | **(가)**. Core 이슈를 열지 않는다. 묶음 규칙 추가는 규칙 +1·timer |
| D2 | M1·M2·M5를 1~7단계 2단계 착수 전에 할지, 병행할지 | **병행**. 측정은 빌드가 없고, 1~7단계는 M1 결과와 무관 |
| D3 | 1~7단계 완료 조건에 "target sendto/msg·thread별 µs/msg·유효 주파수"를 넣을지 | **넣는다**. 처리량은 I/O 상한에 가려 변화를 못 보인다(PL §5) |
| D4 | H44를 7단계에 포함(PL D3 유지) 여부와 B1 시점 | H44 **포함**, B1은 **M2·M5 뒤** |
| D5 | pacer 코드 저장소 편입(F4) | **편입**. 지금은 핵심 측정 조건이 저장소 밖에 있다 |

---

**요약(5줄)**
1. Core의 송신 규칙은 하나다 — engine이 pipe를 비우는 순간 pipe에 있던 메시지 전부가 한 sendto가 된다(`asio_engine.cpp:1478-1618`, `pipe_receive.cpp:259-261`). 메시지당 API·flush·multipart·pipe는 Framework와 binding 벤치가 완전히 같다(§3).
2. 차이는 같은 묶음으로 도착한 두 메시지에 대한 **제출 간격**이다. binding server는 한 thread에서 5–6 µs 간격으로 reply 둘을 넣고(p = 1.00), Framework target은 worker의 순차 handler turn이 20 µs 이상 벌려 I/O thread가 먼저 drain한다(p = 0.59–0.85). source의 짝(0.71)은 target의 송신 짝에 묶여 있다.
3. target IO/1 +60 %(perf)는 명령어 +22 %(추가 sendto, 10–21k instr/sendto), IPC −11 %(kernel 코드 혼입으로 설명됨), 유효 주파수 −14 %(활성 thread 수)로 나뉜다(가설). 수신 thread의 +22 %는 cycles가 같아 주파수만이다. 바이트·frame 배치는 원인이 아니다.
4. Core·binding 수정은 없다(계약대로 동작). 수정은 승인된 1~7단계(handler turn 비용 → 간격), H44 수신 역할 인계(판정 2 → 1, thread 수), B1(dispatcher hop)이며, 완료 조건에 sendto/msg·주파수·thread별 µs/msg를 넣는다. 효과는 I/O 상한 65k → 80–90k(가설).
5. 확정 측정은 제출 간격 g와 drain 창 W의 분포(uprobe), cycles/ref-cycles 주파수, pacer burst(소스 미확보), binding S2에 20 µs 간격을 넣는 재현이다. 한 세션, 빌드 없음.
