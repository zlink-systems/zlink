# S-A — STREAM 경로 실측 프로파일 (측정 전용)

> 2026-09-06, main `285f37792d`. `bindings/c/bench/with_stream` 바이너리(`bindings/c/build/bench/with_stream/`)를 그대로 사용, 소스·빌드 변경 없음.
> 원본 데이터: `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/S-A/`
> (`*_proc.jsonl` = /proc 샘플, `cg_*.out` = callgrind, `ann_*.txt` = callgrind_annotate, `sample.py`/`cell.sh`/`cg.sh`/`buckets.py` = 계측 스크립트)

## 0. 계측 수단 — perf 사용 불가

| 도구 | 상태 |
|---|---|
| `perf` | **없음**(`/usr/bin/perf`, `~/.local/bin` 모두 부재) |
| `perf_event_open(2)` | **커널이 거부**(WSL2 `6.6.87.2-microsoft-standard-WSL2`). HW `instructions`·SW `context-switches` 둘 다 `fd=-1`. 테스트: `/tmp/pe.c` |
| `strace`/`ltrace`/`gdb` | 없음 |
| `valgrind`/`callgrind` | 있음(`~/.local/bin`) — 사용 |

대체 계측 두 축:
1. **CCU 1000 실규모**(브리프 셀 그대로): `/proc/<서버pid>/io`(syscr·syscw·rchar·wchar), `/proc/<pid>/task/*/status`(vol/nonvol ctxsw), `task/*/stat`(스레드별 utime/stime)를 250 ms 간격으로 샘플링. 측정 구간은 각 런의 **마지막 2.5 s**(측정 구간 내부).
   - 주의: `/proc/<pid>/io`의 `syscr/syscw/rchar/wchar`는 **소켓 `recv`/`send`를 세지 않는다**(asio 서버는 전 항목 0). zlink에서 잡히는 값은 전부 **eventfd/파이프 read·write**다. 실제로 `wchar/syscw = 8.00 B`, `rchar/syscr = 7.8 B` → **정확히 eventfd 8바이트**. 즉 이 수치가 곧 wake syscall 수다.
2. **명령 수·심볼 분해**: callgrind(`--cache-sim=no`)로 서버를 감싸고 **CCU 20, warmup 2 s, duration 15 s** 축소 셀 1024 B. 서버 종료 시 찍히는 `METRIC ... recv_msgs=` 로 나눠 메시지당 값을 낸다. 서버 단독 기동분(zlink 3.45 M, asio 2.07 M Ir)은 차감했다(무시 가능).
   - **한계**: valgrind는 스레드를 직렬화하므로 배치·wake 빈도가 실규모와 다르다. 아래 명령 수 비율(2.2×)은 실규모 CPU 비율(1.5×)보다 크게 나오며, 이는 CCU 20 + 직렬화로 **배치 이득이 사라진 zlink 쪽이 더 불리해진** 결과다. 순위·구성비 판독용으로만 쓴다.
   - 심볼: `libzlink.so.0.17.0`은 스트립되지 않았고(로컬 심볼 8894개) callgrind가 내부 함수까지 귀속한다. Release+LTO 그대로 사용, build-dev 대체 불필요.

명령줄(요약): `cell.sh <stack> <size> <port>` = 서버 `--io-threads 4 --sndbuf/--rcvbuf 1048576 --backlog 32768 --tcp-nodelay 1`, 클라이언트 `--ccu 1000 --runs 1 --warmup 3 --duration 5 --io-threads 4`. `cg.sh <stack> <port>` = 위를 valgrind로 감싼 CCU 20 판.

## 1. 실규모(CCU 1000) 결과 — 처리량은 베이스라인과 일치

| 셀 | tps(k) | 서버 CPU | **CPU µs/메시지** | vol ctxsw/msg | eventfd write/msg | eventfd read/msg |
|---|---|---|---|---|---|---|
| zlink 64 B | 267.9 | 425 % | **15.9** | 0.088 | 0.645 | 0.380 |
| asio 64 B | 328.9 | 349 % | **10.6** | 0.047 | 0 | 0 |
| **zlink 1024 B** | 257.8 | 431 % | **16.7** | 0.092 | **0.654** | **0.381** |
| **asio 1024 B** | 319.9 | 353 % | **11.0** | 0.046 | **0** | **0** |
| zlink 65536 B | 30.3 | 283 % | 93.4 | 0.413 | 0.489 | 0.491 |
| asio 65536 B | 37.6 | 377 % | 100.0 | 0.131 | 0 | 0 |

- **1024 B에서 메시지당 CPU 1.52×**(16.7 vs 11.0 µs), 64 B에서 1.50×. §1.1의 "고정 비용 1.5배"가 실측으로 확인된다.
- **자발적 컨텍스트 스위치 2.0×**(0.092 vs 0.046 /msg). 64 KiB에서는 3.2×(0.413 vs 0.131).
- asio 서버는 **eventfd wake가 0**이다(io_context를 깨울 일이 없음 — 모든 일이 read 완료 핸들러 안에서 끝난다). zlink는 **메시지당 eventfd write 0.65 + read 0.38**.

### 스레드별 CPU 분포 (1024 B)

| | I/O 스레드 4개 | 앱 스레드 |
|---|---|---|
| zlink | 87.1 % 씩(usr 31 / sys 56) = 348 % | **82.8 %**(usr 71 / **sys 12**) |
| asio | 88.3 % 씩(usr 27 / sys 61) = 353 % | 없음 |

zlink의 I/O 스레드 총합(348 %)은 asio 4스레드(353 %)와 거의 같은데 **처리량은 19 % 적다**. 즉 I/O 쪽만 봐도 메시지당 13.5 µs vs 11.0 µs(+23 %)이고, 그 위에 **앱 스레드 3.2 µs/메시지(사실상 코어 하나 전체)** 가 순수 추가 비용으로 얹힌다. 앱 스레드는 usr 71 %/sys 12 % — 잠금·명령 처리·recv/send API의 사용자 공간 비용이다.

### 64 KiB 셀의 I/O 스레드 idle 원인 (한 문단)

64 KiB에서 zlink의 I/O 스레드는 4개 모두 **45–49 %** 로 절반 놀고 있고, 앱 스레드가 **93.4 %(usr 77.9)** 로 포화한다. 즉 이 크기에서는 병목이 I/O가 아니라 **단일 앱 스레드**다: 에코 1회당 `zlink_poller_wait` → command drain(`process_commands`) → `zlink_recv_part` → `zlink_send_part_rid`가 전부 한 스레드에서 직렬로 돌고, 64 KiB 프레임은 메시지당 I/O 스레드 일(read/write 시스템콜)이 상대적으로 커도 앱 스레드 일은 프레임 크기에 거의 무관한 고정 비용이라 앱 스레드가 먼저 찬다. 그 증거로 메시지당 CPU는 zlink 93.4 µs < asio 100 µs 인데도 처리량은 zlink가 19 % 낮다 — CPU가 모자란 게 아니라 **한 스레드에 직렬화**되어 있다. 또 vol ctxsw가 0.413/msg로 세 배 — I/O 스레드가 앱 스레드의 command를 기다리며 자고 깨는 왕복이다.

## 2. 메시지당 syscall (callgrind, CCU 20, 1024 B) — 호출 횟수 실측

| syscall | zlink /msg | asio /msg |
|---|---|---|
| `recv` (TCP read) | **3.000** | **1.000** |
| `send` (TCP write) | 1.000 | 1.000 |
| `read` (eventfd, = `signaler_t::recv_failable`) | 0.921 | 0 |
| `write` (eventfd, = `signaler_t::send`) | 0.524 | 0 |
| `epoll_wait` | 0.476 | 0.107 |
| `epoll_ctl` | 0.066 | 0.000 |
| **합계** | **≈ 5.99** | **≈ 2.11** |

- **`recv` 3회/메시지**가 가장 큰 syscall 격차다. read drain 루프(`asio_engine.cpp:1043-1080`)가 EAGAIN을 받을 때까지 더 읽고, speculative read(`asio_engine.cpp:539-580`)가 한 번 더 붙는다. asio 레퍼런스는 정확히 1회.
- eventfd wake는 실규모(CCU 1000)에서도 write 0.65 / read 0.38 로 **여전히 메시지당 0.5~1회 수준**이다. "부하가 높으면 배치당 1회로 줄어든다"는 기대는 **성립하지 않았다**.

## 3. 명령 수와 비용 버킷 (callgrind, 1024 B)

| 항목 | zlink | asio | 비 |
|---|---|---|---|
| **명령 수 / 메시지 (Ir)** | **11,096** | **4,989** | **2.22×** (직렬화 보정 전, §0 한계 참조) |
| `pthread_mutex_lock/unlock` | 12.91 % (1,440 Ir/msg), **lock 19.4회/msg** | 14.94 % (747 Ir/msg), lock 12.0회/msg | Ir 1.93× |
| `__tls_get_addr` (동적 TLS) | **3.43 % (383 Ir/msg), 31.9회/msg** | 0.00 % | zlink 전용 |
| `malloc/free` | 1.17 % (130 Ir/msg), **malloc 1.31 + free 1.47 /msg** | **0.00 %** (0.001/msg) | zlink 전용 |
| `memcpy/memmove/memset` (libc 심볼) | 0.00 % | 0.00 % | 둘 다 무시 가능 |
| syscall stub | 1.65 % | 1.30 % | — |
| futex | **0** | 0 | 경합 없음 |

zlink 상위 자체비용 심볼(전체 대비, `ann_zlink.txt`): `pthread_mutex_lock` 7.13 %, `pthread_mutex_unlock` 5.77 %, `__tls_get_addr` 3.43 %, `zlink_recv_part` 2.63 %, `asio_poller_t::loop` 2.11 %, `pipe_t::read` 1.92 %, `ypipe_t::read` 1.76 %, `pipe_t::account_inbound_frame` 1.69 %, `msg_t::size` 1.68 %, `socket_base_t::process_commands` 1.48 %, `asio_engine_t::start_async_read` 1.37 %, `msg_t::close` 1.37 %, `prepare_output_buffer` 1.36 %, `recv_routed` 1.34 %, `wait_for_completion_submit_admission` 1.29 %, `mailbox_t::recv` 1.15 %, `mailbox_t::send` 1.11 %, `stream_t::xsend_routed` 1.08 %, `send_direct_with_retry` 1.05 %, `prepare_gather_output` 0.78 %.

핵심 호출 횟수(메시지당): `mailbox_t::send` **2.00**, `mailbox_t::recv` **1.92**, `signaler_t::send` 0.52, `shared_message_memory_allocator::allocate` **2.00**, `prepare_gather_output` **1.00**, `process_commands` 0.68.

## 4. 메시지당 비용 순위 (zlink − asio, 1024 B)

| 순위 | 항목 | 실측 격차 | 후보 원인 file:line |
|---|---|---|---|
| 1 | **I/O↔앱 핸드오프 command 왕복** — mailbox send 2.00/msg, recv 1.92/msg, eventfd write 0.65 + read 0.38/msg, vol ctxsw 2×, 앱 스레드 CPU 3.2 µs/msg(코어 1개) | asio는 전부 0 | `core/src/runtime/core/pipe.cpp:4109`, `core/mailbox.cpp:56-82`, `core/signaler.cpp:191-210`, `pipe.cpp:2738-2766`, `sockets/common/socket_base_dispatch.cpp:1603-1678` |
| 2 | **TCP `recv` 3회/메시지**(asio 1회) — read drain + speculative read의 EAGAIN 왕복. sys CPU 8.8 µs vs 7.6 µs/msg | +2 syscall/msg | `engine/asio/asio_engine.cpp:1043-1080`, `:539-580`, `:466-509` |
| 3 | **재귀 pthread mutex** — lock 19.4회/msg(asio 12.0), 자체 Ir 1,440 vs 747/msg | 1.93× | `utils/fast_mutex.hpp:41-46`, `utils/mutex.hpp:89-94` |
| 4 | **동적 TLS(`__tls_get_addr`) 31.9회/메시지, 383 Ir/msg** | zlink 전용 | 공유 라이브러리 내 `thread_local`(예: `sockets/common/socket_message_recv_api.cpp` `recv_tls_view`, `zlink::recv_tls_view::storage()` 0.80 %) |
| 5 | **수신 버퍼 malloc/free 1.3+1.5회/메시지**, `allocate()` 2.00회/메시지 | asio 0 | `protocol/decoder_allocators.cpp:249-287, 126-163`, `engine/asio/asio_raw_engine.cpp:88-89` |
| 6 | **소켓 상태 기계·API 계층 자체비용**(`recv_routed`+`receive_once_guarded`+`process_commands`+`send_direct_with_retry`+`xsend_routed`+`submit_public_send_record`+`wait_for_completion_submit_admission` ≈ 8 % Ir) | asio에 대응물 없음 | `socket_base_msg.cpp:53-98, 320-399, 929-967`, `socket_send_submit.cpp:471-511`, `stream.cpp:918-994` |
| 7 | **회계·no-op 잔여**: `account_inbound_frame` 1.69 %, `msg_t::size` 1.68 %, `prepare_gather_output` 1.00회/msg(0.78 %) | 소 | `pipe.cpp:3843-3951`, `asio_engine.cpp:757-802` |
| — | **memcpy** | **격차 없음(양쪽 0 %)** | — |

## 5. S-B 가설 확인/반박

| S-B 항목 | 판정 | 근거 |
|---|---|---|
| F3 / §3 "격차는 복사량이 아니라 고정 비용" | **확인** | libc mem* 자체비용 양쪽 0.00 %. asio가 2회 복사하고도 메시지당 명령 수·CPU가 절반 |
| §3 1위 "핸드오프가 최대 비용" | **확인(1위 유지)** | mailbox send 2.00/recv 1.92/msg, eventfd 1.03 syscall/msg, ctxsw 2×, 앱 스레드 CPU 3.2 µs/msg 전액이 zlink 전용 |
| §5 "eventfd가 메시지당인가 배치당인가" | **메시지당에 가깝다(배치 퇴화 반박)** | 실규모 CCU 1000에서도 write 0.654 + read 0.381 /msg. F7(비-coalescing)의 실제 영향 확인 |
| §3 2위 "재귀 mutex 남용" | **확인, 다만 2위가 아님** | 메시지당 lock 19.4회로 정적 추정(7–10쌍)의 두 배. 그러나 자체 Ir 격차는 693 Ir/msg로 총 격차(6,107 Ir/msg)의 11 %. futex 0 → **경합은 없다**(§5 "경합 여부" 질문의 답: 비경합) |
| §3 3위 "수신 버퍼 malloc/free" | **확인(빈도), 비중은 예상보다 작음** | malloc 1.31 + free 1.47 /msg(spare 재사용 실패가 상시). 그러나 Ir 비중 1.17 %(130 Ir/msg) |
| §3 4위 `prepare_gather_output` 죽은 작업 | **확인** | 메시지당 정확히 1.00회, 자체 0.78 % + 하위 호출 |
| §5 `xsend_writable_target_ready`가 정상 경로에 없음 | **확인** | 상위 심볼·호출 목록에 등장하지 않음(호출 0) |
| §5 `fq_t` round-robin 헛읽기 | **문제 없음** | `fq_t::recvpipe` 0.95 %, 1회/메시지 수준. 1000 pipe에서도 선형 탐색 비용 없음 |
| §5 "`_out_sync`/`receive.sync` 경합" | **반박** | futex 호출 0 — 전부 비경합. 비용은 경합이 아니라 **락 횟수와 재귀 mutex 상수** |
| S-B가 놓친 항목 | **신규**: (a) TCP `recv`가 메시지당 3회(asio 1회), (b) `__tls_get_addr` 31.9회/메시지 = 3.4 %, (c) 64 KiB에서 병목은 I/O가 아니라 포화한 **단일 앱 스레드** | 본 보고서 §2·§3·§1 |

## 6. 후속 job에 주는 함의 (측정에서 직접 도출)

1. **S-1(핸드오프 축소)은 여전히 1순위**. 다만 "eventfd 줄이기"보다 **command 왕복 자체와 앱 스레드 3.2 µs/msg**가 본체다.
2. **신규 S-9 후보: read drain의 여분 `recv` 제거**(3→1~2회). syscall 격차의 절반 이상이며 sys CPU에 직결. `asio_engine.cpp:1043-1080`.
3. **신규 S-10 후보: 핫 경로의 `thread_local` 접근 정리**(`__tls_get_addr` 31.9회/msg). initial-exec TLS 모델 또는 TLS 포인터 1회 캐시로 383 Ir/msg 대부분 제거 가능, 계약 영향 없음.
4. **S-2(비재귀 mutex)의 기대 이득 하향**: 경합이 0이므로 이득은 락당 상수 절감뿐 — 총 격차의 ~11 % 중 일부. 위험(데드락) 대비 우선순위를 S-9·S-10 뒤로.
5. **64 KiB 셀은 별도 문제**: CPU가 아니라 앱 스레드 직렬화. 여기서는 앱 스레드 경로(poller wait + command drain + recv/send API) 단축이 유일한 지렛대다.
