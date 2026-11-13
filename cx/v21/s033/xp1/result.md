# P1 결과 (Core I/O thread 깨어남 분해) — 2026-11-10 08:0x–08:3x UTC, origin/main 39bbfe470d, libzlink 1.23.0 (sha 6f496304…)

각 1회. 구현마다 request-backpressure 1 KiB를 equal22k pacing(binding 32k 상한, Framework 48k 상한)으로 돌렸다. source CPU 0-7, target CPU 8-15, measure.lock 안. 모든 계수는 perf를 TID별로 붙여 얻었다(값은 메시지당).

## 1. 횟수 (raw/p1, uprobe 8종 + syscall; raw/p1-net, sendto·recvfrom·packet)
| thread | binding | Framework |
|---|---|---|
| target IO/1 epoll_wait | 0.935 / 0.888 | 0.901 / 0.977 |
| target IO/0 epoll_wait | 0.475 / 0.453 | 0.460 / 0.464 |
| target on_read_complete | 0.475 | 0.459 |
| target IO/1 sendto = 송신 packet | 0.445 / 0.446 | 0.575 / 0.576 |
| target IO/0 recvfrom | 0.900 | 0.885 |
| target IO/0 signaler_t::send | 0.469, 그 뒤 futex 0.473·eventfd write 0 | 0.374, 그 뒤 eventfd write 0.373·futex 0.016 |
| target IO/1 process_activate_read | 0.468 | 0.552 |
| process_async_mailbox (I/O thread) | 0 | 0 |
| set_receive_flow_state / consume_receive_flow_state_frame / poller rebuild | 0 | 0 |
| source 프로세스 전체 signaler_t::send | 0.513 | 1.542 |

## 2. CPU 효율 (raw/p1-ipc, perf stat instructions·cycles)
| thread | binding instr/msg | Framework instr/msg | binding IPC | Framework IPC | cache-miss/msg B→F | migration/msg B→F | CPU µs/msg B→F |
|---|---:|---:|---:|---:|---|---|---|
| target IO/1 | 17,404 | 18,756 | 0.58 | 0.49 | 1,044→1,436 | 0.009→0.018 | 13.50→17.83 |
| target IO/0 | 17,027 | 14,742 | 0.57 | 0.50 | 897→1,120 | 0.007→0.021 | 13.63→15.00 |
| source IO/0 | 19,623 | 19,796 | 0.55 | 0.45 | 1,171→1,855 | 0.013→0.050 | 17.29→22.04 |
| source IO/2 | 16,500 | 15,637 | 0.58 | 0.48 | 733→1,465 | 0.006→0.041 | 13.67→16.90 |

cycles만 쟀다(uprobe·stat 없음, raw/p1-cycles). 메시지당 cycles는 target IO/1이 27.6k→43.4k, target IO/0이 22.4k→30.4k, source IO/0이 28.4k→43.9k였다. Framework에서 늘어난 cycles는 커널 TCP·netfilter 경로(nft_do_chain, tcp_transmit_skb, tcp_v4_rcv, loopback_xmit 등)에 고르게 퍼져 있다(raw/p1-cycles/target-io1-symdiff.txt). 특정 Core 함수 하나에 몰린 것이 아니다.

## 3. 판정 (plan §6 P1 표)
- N1(깨어남 수): 아니다. epoll_wait·on_read_complete/msg 비율이 0.93–1.04다.
- N2(eventfd write): 일부만 맞다. target 엔진 thread의 앱 깨우기가 binding은 futex 0.47/msg, Framework는 eventfd write 0.37/msg다. 횟수는 같고 방식만 다르다. "Framework ≈2, binding ≈0"은 아니다. source 프로세스의 signaler send는 3배다(1.54 대 0.51). 다만 이것은 대부분 앱 thread에서 일어난다.
- (b) Core 내부 request/reply 경로: 아니다. I/O thread의 메시지당 명령어 수가 같다(±8–15%). process_async_mailbox는 I/O thread에서 불리지 않는다.
- N3(flow state): 아니다. 호출 0회다.
- 새 사실: 같은 일을 더 느리게 한다. IPC가 0.57→0.49로 떨어지고, cache miss가 1.4–2배, migration이 2–6배다. target CPU 8-15를 Framework thread들과 함께 쓰면서 생기는 cache·스케줄링 간섭으로 본다. Core가 일을 더 하는 것이 아니다.

## 4. 주의
- perf를 붙이면 binding I/O의 µs/msg가 xio의 7.4에서 13.5로 커진다. 그래서 절대값은 xio 값을 쓰고, 이 측정은 구현 사이의 비율과 횟수로만 읽는다. 모두 각 1회다.
- binding send 10k paced는 bench client가 'bad optional access'로 끝나 빠졌다(raw/p1-binding-10k-send.failed). send의 I/O 비용은 xio에서 이미 같았다.
- 준비 job의 script에서 결함 네 개를 고쳤다. lib 경로(inode), local symbol(주소로 probe), TID에 ps 사용, 창 길이(14 s → 8 s)다. 첫 시도 출력은 raw/p1-attempt*에 남겼다.

## 5. 다음 측정 제안
- affinity 이분: target에서 Core I/O thread를 전용 CPU 2개(8-9)에 두고, Framework thread를 10-15에 둔다. 각 1회 같은 rate로 I/O µs/msg를 잰다. IPC·cache miss가 binding 수준으로 돌아오면 간섭이 확정된다. 그러면 개선은 Framework thread 수와 배치(7단계 수신 역할 인계 포함)에서 나온다.
- P2의 S2·C2(reply·completion을 둘째 thread로)는 같은 간섭을 binding 벤치에서 재현하는지 보는 대조로 그대로 둔다.
