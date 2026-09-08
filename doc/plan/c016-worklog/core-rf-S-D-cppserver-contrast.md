# S-D — cppserver / 기본 asio / zlink STREAM read·write 경로 대조

> 2026-09-08, 분석·측정 전용. 소스·스펙·build·commit·stash 변경 없음.
> 원본 계측 자료: `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/sd/`

## 0. 결론

| 질문 | 결론 |
|---|---|
| 큰 payload에서 cppserver가 기본 asio보다 빠른 주원인 | **큰 receive buffer와 read/write 독립 진행**이다. 64 KiB 셀에서 기본 asio는 고정 16 KiB buffer 때문에 성공 `recvfrom`이 **5.003회/msg**, cppserver는 성장 가능한 buffer로 **1.004회/msg**였다. 기본 asio는 write가 끝날 때까지 다음 read도 막는다. |
| write 측 이득 | 65,553 B wire frame에서 기본 asio의 composed `async_write`는 `sendto` **2.000회/msg**, cppserver의 `async_write_some`은 **1.000회/msg**였다. cppserver의 main/flush buffer는 read와 write를 겹치게 하지만, 이번 축소 셀에서 여러 frame의 한 syscall 병합은 관측되지 않았다. |
| zero-copy 또는 복사 감소 때문인가 | **아니다.** 1 KiB에서 동적 copy-family 호출은 기본 asio **2.000회/msg**, cppserver **3.000회/msg**다. 64 KiB의 소스상 payload copy 하한도 각각 2×와 3× wire bytes다. cppserver는 더 많이 복사하고도 syscall·handler turn을 줄여 이긴다. |
| socket option·thread 수 차이인가 | 아니다. 계측은 세 서버 모두 I/O thread 1, `TCP_NODELAY=1`, `SO_SNDBUF=SO_RCVBUF=1 MiB`, backlog 32768로 맞췄다. keepalive도 활성화하지 않았다. |
| zlink가 더 느린 이유 | zlink는 Core 계약상 application thread와 I/O thread 사이의 pipe/mailbox/poller 경로를 추가로 지난다. 1 KiB에서 전체 syscall은 **9.239회/msg**로 기본 asio 2.116, cppserver 2.075보다 많고, baseline 차감 self Ir도 **8,686/msg**로 4,986과 3,642보다 크다. |
| zlink 64 KiB의 별도 병목 | 현재 64 KiB+17 B frame은 성공 read **2.025회/msg**로 쪼개진다. 이 때문에 벤치가 RAW message를 그대로 되보내는 fast path를 못 타고 조립+reply 복사를 수행한다. `memcpy` 구현 132,198 Ir/msg와 `memset` 구현 65,565 Ir/msg, 합계 **197,763 Ir/msg**가 이를 보여 준다. |
| 가장 작은 후속 작업 | (B1) `restart_input()`의 무조건 speculative read를 기존 full-read 조건 하나로 합쳐 EAGAIN `recvfrom` **1회/msg** 제거, 2–3 h. (B2) decoder target의 “연속 2회 full hit”를 “1회 full hit”와 대조해 큰 read의 성장 정체를 없애는 실험, 2–3 h. 둘 다 Core engine 소유이며 공개 STREAM 계약 안이다. B2 전에 TCP chunk를 잘못 거부할 수 있는 benchmark parser 결함을 분리해야 한다. |

관측된 CCU 1000 비율(`measure-cppserver-summary.md`, D-B257)은 load 오염 때문에 절대 처리량 근거로 쓰지 않았다. 다만 이 보고서의 idle 축소 계측에서 payload 증가 시 cppserver/asio 격차를 만드는 syscall 구조가 직접 재현됐다.

## 1. 범위와 계측 방법

### 1.1 조건

| 항목 | 값 |
|---|---|
| 서버 | `stacks/asio/test_scenario_stream_asio`, `stacks/cppserver/.../stream_fixed_server`, `stacks/zlink/test_scenario_stream_zlink` |
| payload / wire frame | 1,024 / **1,041 B**, 65,536 / **65,553 B** (`6 B prefix + 11 B "stream.echo" + payload`) |
| 부하 | CCU 20, client I/O thread 1, server I/O thread 1 |
| 구간 | 2 s warm-up client run 뒤 5 s client run. 서버와 계측기는 한 process에서 계속 실행했으므로 syscall·Ir·`recv_msgs` 모두 두 구간을 포함한 같은 범위다. |
| socket | loopback TCP, `sndbuf=rcvbuf=1,048,576`, backlog 32768, `TCP_NODELAY=1` |
| syscall | `/tmp/ws1-tools/root/usr/bin/strace -qq -f -c` |
| 명령 수 | Valgrind 3.23 Callgrind, `--cache-sim=no --separate-callers=8`; stack별 1 s 무클라이언트 baseline을 차감하고 서버 종료 `recv_msgs`로 나눔 |
| 실행 보호 | 매 셀 직전 `ninja=0`, available memory ≥6,000 MiB, load1 ≤2.0 확인; `PERF_LOCK`을 `flock`으로 잡고 포그라운드 직렬 실행 |
| build | 하지 않음. 기존 release 바이너리 사용. GCC 13.3 `-O3`; 기본 서버 Boost.Asio 1.83, cppserver standalone Asio 1.32 |

Callgrind는 thread를 직렬화하므로 처리량 비교가 아니라 함수 구성과 Ir/msg 비교에만 쓴다. 1 s baseline은 서버 기동 고정분을 제거하지만, workload에 포함된 20개 connection setup과 vector 초기 성장은 제거하지 않는다. 따라서 특히 64 KiB의 allocation/msg는 **steady-state가 아닌 보수적 상한**이다.

### 1.2 실제 load·memory 기록

| 계측 | stack | payload | 시작→종료 load1 | available MiB 시작→종료 | ninja 시작→종료 |
|---|---|---:|---:|---:|---:|
| strace | asio | 1 KiB | 0.866→0.877 | 10,472→10,464 | 0→0 |
| strace | cppserver | 1 KiB | 0.252→0.293 | 10,495→10,493 | 0→0 |
| strace | zlink | 1 KiB | 0.739→0.854 | 10,624→10,605 | 0→0 |
| strace | asio | 64 KiB | 1.237→1.126 | 10,602→10,598 | 0→0 |
| strace | cppserver | 64 KiB | 0.953→0.957 | 10,620→10,616 | 0→0 |
| strace | zlink | 64 KiB | 0.810→0.905 | 10,617→10,617 | 0→0 |
| Callgrind | asio | 1 KiB | 0.280→0.392 | 10,611→10,609 | 0→0 |
| Callgrind | cppserver | 1 KiB | 0.360→0.612 | 10,609→10,619 | 0→0 |
| Callgrind | zlink | 1 KiB | 0.563→0.557 | 10,617→10,617 | 0→0 |
| Callgrind | asio | 64 KiB | 0.655→0.763 | 10,612→10,611 | 0→0 |
| Callgrind | cppserver | 64 KiB | 0.645→0.700 | 10,609→10,598 | 0→0 |
| Callgrind | zlink | 64 KiB | 0.592→0.625 | 10,611→10,606 | 0→0 |

모든 채택 셀은 idle gate 안에서 시작했다. parse/protocol/send error는 모두 0이었다.

## 2. 정적 경로 대조

### 2.1 전체 표

| 항목 | 기본 asio | cppserver | zlink STREAM/TCP RAW |
|---|---|---|---|
| read API | `socket.async_read_some` | `socket.async_read_some` | transport `async_read_some`; bounded drain에서 동기 `read_some`도 사용 |
| 최초 user read buffer | **16,384 B 고정** | accept 직후 `getsockopt(SO_RCVBUF)` 값으로 vector 생성. host `tcp_rmem` default는 131,072 B지만 accepted socket의 반환값 자체는 별도 기록하지 않음 | RAW decoder storage는 `in_batch=4,160 B`로 생성되지만 engine target cap이 적용되어 **첫 read request는 4,096 B** |
| 성장 | 없음 | read가 buffer를 정확히 채우면 **2×**, 16 MiB limit까지 | 현재 target을 연속 **2회** 정확히 채울 때 2×. 이 셀은 명시한 `rcvbuf=1 MiB`가 max지만, production 기본 `rcvbuf=-1`이면 코드상 max도 초기 4 KiB에 머묾 |
| 64 KiB+17 B 귀결 | 최소 5 read chunk | 초기 buffer가 frame보다 크면 1 read | 4 KiB에서 2-hit로 성장. 동적 결과는 2.025 successful read/msg이며, 64 KiB 부근의 full/short 교대가 hit counter를 reset하는 것이 가장 강한 정적 설명이나 target trace로 직접 기록하지는 않음 |
| decode→echo copy | read chunk→frame buffer 1회 + frame→`pending_write` 1회 | receive buffer→frame buffer 1회 + frame→benchmark `pending_send` 1회 + `SendAsync` main buffer 1회 | decoder allocator→`msg_t`는 zero-copy. 완전한 RAW chunk면 benchmark가 같은 message를 send; 조각이면 frame buffer 조립 1× + reply 1× |
| send staging | vector 하나 | main/flush vector 둘; main에 append 후 flush와 swap | pipe의 `msg_t`; raw encoder는 4 KiB 미만을 batch buffer에 복사, 큰 단일 message는 payload pointer 반환 |
| write API | composed `boost::asio::async_write` | `asio::async_write_some` | 먼저 nonblocking `write_some`, partial/EAGAIN이면 async write |
| write 병합/flush | 현재 read callback에서 완성된 frame들을 한 `pending_write`에 모으나, write 완료 전 read를 멈춰 cross-read 병합 없음 | send 진행 중 새 frame을 main에 append; flush 종료 뒤 swap. timer/delay 없이 준비된 byte만 자연 병합 | 준비된 pipe message를 현재 output target까지 bounded batch. 준비되지 않은 traffic을 기다리지 않음; speculative turn 2 MiB 상한 |
| read/write 겹침 | **없음**. write 완료 후 다음 read arm | **있음**. receive callback 끝에서 즉시 다음 read arm, send는 별도 상태 기계 | engine read/write는 독립. 다만 application echo는 app↔I/O pipe/command 왕복 뒤 output에 도착 |
| handler allocation | 기본 Asio handler allocator/cache | read·write 각각 1 KiB inline `HandlerStorage`, 초과만 heap | read·write 각각 1 KiB inline custom allocator, 초과만 heap |
| strand/executor | connection별 strand. io=1에서도 wrapper·shared state 비용 유지 | `Service(threads, false)`의 io-service-per-thread; connection은 한 service 소유, **strand 미사용** | I/O thread별 io_context와 connection 단일 소유; per-connection strand 없음 |
| server 실행 thread | 지정된 worker 수. 이 셀 1 | 지정된 service worker 수. 이 셀 1 | Core I/O 1 + 별도 benchmark application/poller thread 1 |
| TCP options | nodelay, snd/rcv 1 MiB; keepalive 미설정 | nodelay, snd/rcv 1 MiB; server keepalive 기본 false | nodelay, snd/rcv 1 MiB; keepalive 기본값을 별도로 활성화하지 않음 |
| `writev`/gather | 없음 | 없음 | RAW STREAM은 gather header가 없어 없음 |

### 2.2 코드 근거

| 사실 | 근거 |
|---|---|
| 기본 asio 16 KiB, strand | `bindings/c/bench/with_stream/stacks/asio/test_scenario_stream_asio.cpp:66-73, 220-246` |
| 기본 asio frame copy와 read/write 직렬화 | 같은 파일 `:249-312`; `on_read_chunk`가 frame을 복사해 `async_write`하고 completion에서만 다음 read를 시작 |
| 기본 asio worker/socket options | 같은 파일 `:128-130, 176-184` |
| cppserver buffer 초기화 시점 | `.../upstream/source/server/asio/tcp_session.cpp:59-86`; kernel buffer 크기를 조회해 user vector를 만든 뒤 첫 read를 arm하고, 그 뒤 derived `onConnected()` 호출 |
| cppserver benchmark의 1 MiB 설정은 첫 arm 뒤 | `.../test_scenario_stream_cppserver.cpp:81-87`; 따라서 이후 kernel option은 바뀌지만 최초 user vector 크기는 바꾸지 않음 |
| cppserver receive 성장/rearm | `.../tcp_session.cpp:429-485`; exact-full이면 2×, callback 뒤 write와 무관하게 `TryReceive()` |
| cppserver main/flush와 partial send | `.../tcp_session.cpp:257-307, 487-562`, header `.../tcp_session.h:252-264` |
| cppserver service/thread/strand | `.../service.cpp:16-47, 94-96`; pool=false이면 service-per-thread, strand 불필요 |
| cppserver handler storage | `.../memory.h:17-54`, `.../memory.inl:12-35`; read/write별 1,024 B inline |
| zlink batch/target 시작값 | `core/src/runtime/sockets/stream/stream.cpp:110-118`, `stream_batch_policy.hpp:12-26`; output 4,096 B, input 4,160 B. `asio_stream_fastpath_policy.hpp:235-280`의 cap으로 첫 engine read target은 4,096 B |
| zlink RAW zero-copy decode | `core/src/runtime/protocol/raw_decoder.cpp:32-77`; decoder allocator 범위면 ownership을 message로 이전 |
| zlink 작은-copy/큰-zero-copy encode | `core/src/runtime/protocol/encoder.hpp:45-99`; message가 output buffer 이상이면 pointer를 직접 반환 |
| zlink target 성장의 2-hit 조건 | `core/src/runtime/engine/asio/asio_engine.cpp:797-817`, `asio_stream_fastpath_policy.hpp:380-410` |
| zlink bounded read/write | `asio_engine.cpp:979-1026, 1216-1408`; read 64 loops/1 MiB, speculative write 2 MiB |
| zlink app 경로 | `.../stacks/zlink/test_scenario_stream_zlink.cpp:193-214, 320-370, 375-395`; poller→DONTWAIT drain→echo |

### 2.3 복사 하한

wire frame `F = payload + 17`로 계산했다. 이는 application/engine에서 명시적으로 수행하는 payload-sized copy의 하한이며 kernel copy, allocator metadata, vector 재할당 시 기존 byte 이동은 제외한다.

| stack/path | 1 KiB | 64 KiB | 설명 |
|---|---:|---:|---|
| 기본 asio | **2,082 B/msg (2F)** | **131,106 B/msg (2F)** | read chunk들을 frame buffer에 append하는 총 F + 완성 frame을 output vector에 복사하는 F |
| cppserver | **3,123 B/msg (3F)** | **196,659 B/msg (3F)** | frame buffer F + benchmark pending F + CppServer main send buffer F; main/flush swap 자체는 0-copy |
| zlink, 완전한 RAW chunk | **1,041 B/msg (F)** | **0 B/msg** | 1 KiB는 raw encoder의 4 KiB batch로 복사; 큰 message는 decode·echo·encode 모두 ownership/pointer 전달 |
| zlink, 이번 64 KiB 조각 경로 | 해당 없음 | **131,106 B/msg (2F)** | frame buffer 조립 F + 새 reply F; 큰 reply encode는 zero-copy |

따라서 cppserver의 우위를 “복사 0회”로 설명할 수 없다. 반대로 zlink는 큰 RAW chunk를 그대로 돌려보낼 때 이론상 가장 좋은 zero-copy 경로가 있지만, 이번 64 KiB 셀의 실제 chunk 분할에서는 그 경로를 놓친다. TCP는 frame 경계를 보존하지 않으므로 Core가 “frame당 chunk 하나”를 보장하거나 목표로 삼아서는 안 된다.

## 3. 동적 대조 — syscall/msg

### 3.1 socket/reactor syscall

괄호의 `E`는 실패 횟수, 이 셀에서는 사실상 `EAGAIN`이다. `recvfrom`의 성공 횟수는 `호출-E`다.

| payload | stack | recv_msgs | `recvfrom` /msg (E) | 성공 recv /msg | `sendto` | `writev` | `epoll_wait` | socket+reactor 합계 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 KiB | asio | 40,557 | 1.002 (0.001) | **1.001** | 1.000 | 0 | 0.101 | **2.103** |
| 1 KiB | cppserver | 39,778 | 1.003 (0.002) | **1.001** | 1.000 | 0 | **0.051** | **2.054** |
| 1 KiB | zlink | 16,680 | 2.002 (**1.000**) | **1.002** | 1.000 | 0 | 0.102 | **3.104** |
| 64 KiB | asio | 11,594 | 5.007 (0.003) | **5.003** | **2.000** | 0 | 0.353 | **7.360** |
| 64 KiB | cppserver | 37,271 | 1.009 (0.004) | **1.004** | **1.000** | 0 | **0.051** | **2.060** |
| 64 KiB | zlink | 11,420 | 3.025 (**1.000**) | **2.025** | 1.000 | 0 | 0.102 | **4.127** |

64 KiB에서 cppserver는 기본 asio 대비 성공 receive 약 **4회**, send **1회**, `epoll_wait` **0.302회**, 합계 약 **5.30 kernel 경계/msg**를 없앤다. 이 값이 큰 payload 처리량 1.46×의 직접적인 경로 근거다.

1 KiB에서 socket read/write 수는 기본 asio와 cppserver가 사실상 같지만 cppserver의 `epoll_wait`이 절반이다. read와 write를 직렬화하지 않고 strand wrapper가 없어서 ready work를 한 reactor turn에 더 오래 처리하는 정적 구조와 일치한다.

### 3.2 zlink의 app↔I/O syscall까지 포함

`eventfd2`는 fd 생성 syscall이라 process 기동 때뿐이다. 실제 eventfd/pipe notification은 아래 일반 `read`/`write`에 잡힌다. 기본 asio와 cppserver의 일반 read/write는 종료·signal 등 기동 고정분뿐이다.

| payload | stack | 일반 `read` /msg (E) | 일반 `write` | `poll` | `eventfd2` | 전체 syscall/msg |
|---:|---|---:|---:|---:|---:|---:|
| 1 KiB | asio | 0.00015 (0.00002) | 0.00007 | 0 | 0.00002 | **2.116** |
| 1 KiB | cppserver | 0.00033 | 0.00013 | 0 | 0.00003 | **2.075** |
| 1 KiB | zlink | **2.956 (1.954)** | **1.002** | **0.956** | 0.00030 | **9.239** |
| 64 KiB | asio | 0.00060 (0.00009) | 0.00026 | 0 | 0.00009 | **7.413** |
| 64 KiB | cppserver | 0.00035 | 0.00013 | 0 | 0.00003 | **2.086** |
| 64 KiB | zlink | **3.764 (2.379)** | **1.385** | **1.015** | 0.00044 | **12.898** |

zlink의 고정 격차는 Boost.Asio가 아니라 공개 socket 모델에서 생긴다. I/O thread가 RAW part를 pipe로 넘기고 app thread가 poller에서 깨서 `zlink_recv_part`로 받고, `zlink_send_part_rid`가 다시 pipe/command를 통해 I/O thread에 도달한다. cppserver/basic asio는 같은 I/O callback 안에서 echo를 완결한다.

## 4. 동적 대조 — Callgrind Ir/msg, allocation, copy 호출

### 4.1 총 Ir/msg

| payload | stack | recv_msgs | gross Ir | 무클라이언트 baseline | baseline 차감 self Ir/msg |
|---:|---|---:|---:|---:|---:|
| 1 KiB | asio | 139,618 | 698,260,570 | 2,067,730 | **4,986** |
| 1 KiB | cppserver | 171,803 | 629,172,423 | 3,443,084 | **3,642** |
| 1 KiB | zlink | 64,327 | 562,296,112 | 3,534,471 | **8,686** |
| 64 KiB | asio | 1,812 | 509,082,788 | 2,067,730 | **279,810** |
| 64 KiB | cppserver | 1,859 | 510,439,783 | 3,443,084 | **272,725** |
| 64 KiB | zlink | 2,316 | 501,451,088 | 3,534,471 | **214,990** |

64 KiB에서 zlink의 Ir/msg가 가장 낮지만 실제 처리량이 가장 낮은 것은 모순이 아니다. Callgrind Ir는 kernel 대기와 thread handoff wall time을 세지 않고 thread를 직렬화한다. strace는 zlink가 cppserver보다 syscall을 6배가량 더 수행함을 보여 주며, CCU 1000 관측에서는 별도 app thread가 포화한다.

### 4.2 heap allocation·copy-family 호출/msg

allocation은 terminal allocator(`malloc/calloc/aligned_alloc/realloc`) 호출을 합쳐 중첩 `operator new→malloc` 이중 집계를 피했다. copy-family는 libc resolved `memcpy` 구현과 `memcpy/memmove/*_chk` 호출 합이다. 모두 무클라이언트 baseline 차감값이다.

| payload | stack | heap alloc/msg | copy-family calls/msg | `memset` calls/msg | 해석 |
|---:|---|---:|---:|---:|---|
| 1 KiB | asio | 0.0035 | **2.000** | 2.000 | frame append + output copy; handler cache가 heap을 거의 제거 |
| 1 KiB | cppserver | 0.0019 | **3.000** | 1.000 | 세 payload copy가 정확히 드러남; inline handler storage 작동 |
| 1 KiB | zlink | **1.180** | **1.040** | 0.001 | raw read ownership 이전 후 작은 output batch copy 1회; message/allocator lifetime 비용 |
| 64 KiB | asio | 0.334 | **6.066** | 6.022 | 약 5 read chunk append + output copy; connection/vector 성장분 포함 |
| 64 KiB | cppserver | 0.225 | **3.067** | 1.088 | payload copy 3회 + connection/vector 성장분 |
| 64 KiB | zlink | **5.403** | **5.273** | 2.121 | 2개 read part의 조립/reallocation + reply; 완전-chunk fast path 실패 |

64 KiB allocation 수는 20 connection setup과 buffer 성장을 포함한 상한이지만, 세 stack을 같은 방식으로 센 값이다. zlink가 cppserver보다 약 24배 많은 것은 fragment별 decoder storage와 benchmark 조립/reply allocation이 동시에 생기는 정적 경로와 일치한다.

### 4.3 함수별 self Ir/msg 상위 20 — 1 KiB

표의 `b::a`=`boost::asio`, `a::d`=`asio::detail`, `z::`=`zlink::`, `s::`=`std::`다. 긴 template 인자는 잘랐다. exclusive self Ir이며 baseline 차감 후 양수인 함수 순위다.

| # | 기본 asio 함수 | Ir/msg | cppserver 함수 | Ir/msg | zlink 함수 | Ir/msg |
|---:|---|---:|---|---:|---|---:|
| 1 | `s::_Sp_counted_base::_M_release` | 588.093 | `libc memcpy_impl` | 408.068 | `pthread_mutex_lock` | 415.559 |
| 2 | `pthread_mutex_lock` | 433.582 | `pthread_mutex_lock` | 325.962 | `pthread_mutex_unlock` | 310.542 |
| 3 | `pthread_mutex_unlock` | 313.142 | `TCPSession::TrySend` | 280.000 | `zlink_recv_part` | 272.260 |
| 4 | `b::a::reactive_send_op::do_complete` | 281.000 | `pthread_mutex_unlock` | 235.417 | `z::pipe_t::read` | 215.033 |
| 5 | `s::__shared_count copy` | 270.046 | `s::_Sp_counted_base::_M_release` | 182.039 | `z::ypipe_t::read` | 197.415 |
| 6 | `b::a::reactive_recv_op::do_complete` | 265.077 | `TCPSession::TryReceive` | 173.040 | `z::asio_poller_t::loop` | 194.457 |
| 7 | `libc memcpy_impl` | 259.263 | `a::d::epoll_reactor::start_op` | 169.673 | `z::asio_engine_t::prepare_output_buffer` | 185.000 |
| 8 | `asio_session_t::start_read_chunk` | 254.080 | `a::d::scheduler::do_run_one` | 130.075 | `z::pipe_t::account_inbound_frame` | 159.494 |
| 9 | `asio_session_t::on_read_chunk` | 254.009 | `stream_echo_session_t::onReceived` | 125.016 | `z::asio_engine_t::start_async_read` | 153.095 |
| 10 | `b::a::write_op::operator()` | 229.000 | `TCPSession::SendAsync` | 123.000 | `z::socket_base_t::try_admit_send_parts_scoped` | 144.000 |
| 11 | `b::a::scheduler::run` | 207.375 | `scheduler::wake_one_thread_and_unlock` | 112.723 | `z::msg_t::close` | 142.771 |
| 12 | `b::a::thread_info_base::allocate` | 172.048 | `epoll_reactor::descriptor_state::perform_io` | 104.981 | `wait_for_completion_submit_admission` | 140.000 |
| 13 | `libc memset_impl` | 160.699 | `libc memset_impl` | 93.280 | `z::socket_base_t::recv_routed` | 135.444 |
| 14 | `io_context executor::execute(strand invoker)` | 154.022 | `s::__shared_count copy` | 90.010 | `z::pipe_t::write_message_unlocked` | 133.003 |
| 15 | `executor_op<write>::do_complete` | 113.000 | `reactive_service_base::do_start_op` | 82.013 | `z::tcp_transport_t::async_read_some` | 131.081 |
| 16 | `scheduler::post_immediate_completion` | 98.000 | `socket_ops::non_blocking_recv1` | 78.909 | `z::encoder_base<raw>::encode` | 130.018 |
| 17 | `executor_op<read>::do_complete` | 97.028 | `reactive_recv_op::do_complete` | 77.018 | `libc memcpy_impl` | 129.335 |
| 18 | `epoll descriptor_state::do_complete` | 90.226 | `reactive_send_op::do_complete` | 77.000 | `epoll descriptor_state::do_complete` | 122.008 |
| 19 | `strand invoker::~invoker` | 84.012 | `recv` | 65.364 | `z::mailbox_t::send` | 119.151 |
| 20 | `vector<unsigned char>::_M_default_append` | 72.015 | `libc pthread helper` | 54.117 | `z::stream_t::xsend_routed` | 118.000 |

1 KiB의 cppserver 이득은 복사 비용(+149 Ir/msg)보다 strand/shared handler와 reactor scheduling 감소가 더 크다. 기본 asio의 strand 관련 `executor::execute`, 두 `executor_op`, invoker만 표 안에서 약 448 Ir/msg이고, shared pointer release/copy 격차도 크다.

### 4.4 함수별 self Ir/msg 상위 20 — 64 KiB

| # | 기본 asio 함수 | Ir/msg | cppserver 함수 | Ir/msg | zlink 함수 | Ir/msg |
|---:|---|---:|---|---:|---|---:|
| 1 | `libc memcpy_impl` | 133,622.587 | `libc memcpy_impl` | 199,480.008 | `libc memcpy_impl` | 132,198.001 |
| 2 | `libc memset_impl` | 131,542.053 | `libc memset_impl` | 69,800.636 | `libc memset_impl` | 65,565.478 |
| 3 | `s::_Sp_counted_base::_M_release` | 1,883.130 | `pthread_mutex_lock` | 341.661 | `pthread_mutex_lock` | 857.670 |
| 4 | `reactive_recv_op::do_complete` | 1,330.918 | `TCPSession::TrySend` | 280.000 | `libc malloc internal` | 707.124 |
| 5 | `pthread_mutex_lock` | 1,322.901 | `pthread_mutex_unlock` | 246.755 | `pthread_mutex_unlock` | 651.141 |
| 6 | `asio_session_t::start_read_chunk` | 1,276.071 | `s::_Sp_counted_base::_M_release` | 187.272 | `zlink_recv_part` | 622.720 |
| 7 | `pthread_mutex_unlock` | 955.428 | `TCPSession::TryReceive` | 180.817 | `main` | 396.129 |
| 8 | `s::__shared_count copy` | 939.576 | `epoll_reactor::start_op` | 175.345 | `z::msg_t::close` | 353.625 |
| 9 | `asio_session_t::on_read_chunk` | 638.684 | `scheduler::do_run_one` | 137.179 | `z::socket_base_t::recv_routed` | 346.694 |
| 10 | `thread_info_base::allocate` | 605.714 | `stream_echo_session_t::onReceived` | 129.271 | `libc free internal 0xab250` | 313.883 |
| 11 | `scheduler::run` | 590.658 | `TCPSession::SendAsync` | 123.000 | `z::socket_base_t::process_commands` | 303.489 |
| 12 | `reactive_send_op::do_complete` | 562.000 | `wake_one_thread_and_unlock` | 117.429 | `libc free internal 0xab070` | 300.731 |
| 13 | `io_context executor::execute(strand invoker)` | 540.700 | `descriptor_state::perform_io` | 110.486 | `malloc` | 292.304 |
| 14 | `executor_op<read>::do_complete` | 487.141 | `s::__shared_count copy` | 91.820 | `z::pipe_t::write_message_unlocked` | 281.877 |
| 15 | `write_op::operator()` | 410.000 | `reactive_service_base::do_start_op` | 84.132 | `z::pipe_t::account_inbound_frame` | 262.309 |
| 16 | `scheduler::post_immediate_completion` | 329.028 | `socket_ops::non_blocking_recv1` | 81.879 | `z::ypipe_t::read` | 252.577 |
| 17 | `strand invoker::~invoker` | 294.927 | `reactive_recv_op::do_complete` | 80.479 | `z::mailbox_t::recv` | 239.665 |
| 18 | `reactive_recv_op_base::do_perform` | 287.737 | `reactive_send_op::do_complete` | 77.000 | `z::pipe_t::read` | 237.181 |
| 19 | `executor_op<write>::do_complete` | 226.000 | `recv` | 67.800 | `z::fq_t::recvpipe` | 195.785 |
| 20 | `handler_work_base ctor` | 225.413 | `libc pthread helper` | 56.885 | `free` | 188.844 |

payload-sized memory work가 64 KiB 표를 지배한다. cppserver는 3F copy 때문에 `memcpy_impl`이 기본 asio보다 약 65.9k Ir/msg 크지만, 기본 asio는 vector resize zero-fill과 5 read/strand handler turn 때문에 총 Ir 차이를 대부분 회수한다. zlink는 fragment path의 2F copy와 약 F zero-fill이 총 Ir의 92.0%다.

## 5. 원인 판정

| 가설 | 판정 | 정적+동적 근거 |
|---|---|---|
| 큰 read buffer가 여러 TCP segment/frame byte를 한 번에 읽는다 | **주원인, 확인** | 64 KiB 성공 recv/msg: asio 5.003, cppserver 1.004. cppserver buffer는 full-read 때 2×, asio는 16 KiB 고정. |
| main/flush가 여러 frame을 한 write syscall에 병합한다 | **기능은 존재하나 이번 셀에서는 반박** | cppserver `sendto=1.000/msg`; 1 frame당 1회라 cross-frame syscall 병합은 없다. 다만 send 중에도 read/main append가 가능해 직렬화를 없앤다. |
| `async_write_some`이 65,553 B를 한 번에 낸다 | **확인** | cppserver 1.000 vs 기본 composed `async_write` 2.000 sendto/msg. wire frame이 64 KiB보다 17 B 크다는 경계 효과다. |
| cppserver가 zero-copy라 빠르다 | **반박** | 1 KiB copy call 3 vs asio 2, 정적 copy 하한 3F vs 2F. |
| handler allocation 회피가 주원인이다 | **보조 원인** | 둘 다 heap alloc은 1 KiB에서 0.004/msg 미만. cppserver/zlink의 1 KiB inline allocator는 유효하지만 큰 payload syscall 격차보다 작다. |
| strand가 차이를 만든다 | **1 KiB 보조 원인, 확인** | 기본 asio는 io=1에서도 connection strand를 사용하고 관련 함수가 상위 Ir에 남는다. cppserver는 pool=false에서 strand를 사용하지 않는다. |
| socket options/thread 수 차이다 | **반박** | 계측 인자를 동일화했다. |
| standalone Asio가 Boost.Asio보다 본질적으로 빠르다 | **근거 없음** | API 구현은 다르지만 관측 차이는 buffer/상태 기계/strand 선택으로 설명된다. library 브랜드만 분리하는 실험은 수행하지 않았다. |
| zlink는 I/O 복사가 많아서 느리다 | **1 KiB 반박, 64 KiB 조건부 확인** | 1 KiB payload copy는 asio보다 적지만 고정 API/pipe 비용으로 느리다. 64 KiB는 target 고착이 fragment slow path를 유발해 2F copy가 실제 병목이 된다. |

## 6. zlink가 계약 안에서 채택할 항목

### 6.1 후보와 예상 이득

먼저 계측 application의 독립 결함이 있다. `test_scenario_stream_zlink.cpp:243-250`은 한 RAW chunk에서 frame 하나를 읽은 뒤 다음 frame의 일부가 남으면 정상 TCP suffix를 malformed로 처리한다. 큰 read target은 이 경우를 더 쉽게 만든다. B2 검증 전에 suffix를 per-RID frame buffer로 넘기도록 고치는 benchmark-owner 작업이 필요하다(**≤1 h**, B 기존 결함). 이 작업은 Core 성능 수정이 아니며 본 job에서는 소스를 바꾸지 않았다.

| 우선 | 항목 / 소유 계층 | 계약 적합성·규칙 변화 | 현재 관측 | 예상 이득 | job 크기 | 분류 |
|---:|---|---|---|---|---:|---|
| 1 | **decoder read target을 full hit 1회에 성장시키는 A/B 실험**. `asio_stream_fastpath_policy`/Core Asio engine 소유 | 기존 `full_hits`/“연속 2회” 규칙을 없애고 “요청 buffer를 채우면 현 target 2×, 기존 max로 clamp” 한 규칙만 둔다. queue/HWM/ownership은 불변. | 64 KiB에서 성공 recv 2.025/msg이고 current 2-hit는 short read마다 reset된다. | Core 기대값은 loopback 64 KiB 성공 recv **최대 약 -1/msg**(2.025→≈1)와 그 read handler/allocator turn 감소다. TCP chunk 경계에 따라 application copy 이득은 **0~131,106 B/msg**, 관측 copy+zero-fill self 이득은 **0~197.8k Ir/msg** 범위라 별도 측정으로 판정한다. | **2–3 h** | **B 기존 결함 후보** |
| 2 | **`restart_input()` speculative read를 기존 full-read evidence gate와 통합**. Core Asio engine 소유 | `maybe_drain_stream_reads()`와 restart 경로가 speculative read 조건을 각각 재정의하지 않게 한다. I/O thread 소유·bounded drain은 유지. | 두 payload 모두 실패 `recvfrom`이 정확히 **1.000/msg**. `asio_engine.cpp:1668-1673`는 직전 read fullness와 무관하게 speculative read를 시도한다. | `recvfrom(EAGAIN)` **-1/msg**. 전체 syscall 기준 1 KiB **9.239→약 8.239(-10.8%)**, 64 KiB **12.898→약 11.898(-7.8%)** 상한. Ir 이득은 kernel 대기를 Callgrind가 세지 않아 별도 측정 필요. | **2–3 h** | **B 기존 결함 후보** |
| 3 | 위 두 변경 결합 검증 | 새 상태를 더하지 않고 기존 target과 read-evidence 규칙만 단순화. | current: 성공 read 2.025 + EAGAIN 1, heap alloc 5.40/msg | socket recv **3.025→약 1/msg**가 loopback 이상적 목표다. application copy/allocation 감소는 chunk 경계 종속이며 Core 계약 결과로 약속하지 않는다. | 각 job 후 별도 **≤3 h** 검증 | B |

`197.8k Ir/msg`는 `memcpy_impl 132,198 + memset_impl 65,565`의 관측 self 합이다. complete RAW chunk에서는 benchmark `:320-329`가 같은 `msg_t`를 그대로 send하고 큰 raw encoder도 pointer를 직접 반환하므로 best case에는 대부분 사라질 수 있다. 그러나 이는 benchmark frame과 TCP chunk가 우연히 맞을 때의 **best-case 상한**이지 Core가 보장할 이득은 아니다.

B1의 syscall callsite 귀속은 strace가 stack을 남기지 않으므로 정적 경로와 Callgrind 호출수를 합친 판단이다. 64 KiB Callgrind에서 `asio_engine_t::speculative_read()`가 2,556회/2,316 msg(**1.104회/msg**) 호출된 것도 EAGAIN 1회/msg와 일치한다. 구현에 들어가기 전에는 해당 분기의 hit/result counter로 한 번 확인하고 임시 계측은 제거해야 한다.

### 6.2 이미 채택되어 추가 job이 아닌 것

| cppserver 특성 | zlink 상태 | 판단 |
|---|---|---|
| `async_read_some` | 이미 사용 | 변경 없음 |
| read/write 독립 진행 | engine에서 이미 독립 | 변경 없음 |
| 1 KiB handler inline storage | read/write별 이미 사용 | 변경 없음 |
| 큰 message write pointer 전달 | RAW encoder가 이미 수행 | 변경 없음 |
| `write_some` | speculative write가 이미 사용; 64 KiB `sendto=1.000/msg` | 변경 없음 |
| bounded output batch | 이미 output target까지 준비된 message만 묶음 | 변경 없음 |
| gather/`writev` | RAW STREAM에는 별도 header가 없어 gather할 두 buffer가 없음 | 채택 대상 아님 |
| main/flush vector | pipe message ownership과 encoder output state가 같은 역할을 더 적은 payload copy로 수행 | CppServer buffer를 추가하면 중복 owner·복사만 늘어남 |

### 6.3 채택할 수 없는 항목

| 항목 | 이유 | 분류 |
|---|---|---|
| I/O completion callback에서 application echo 직접 호출 | `08-stream` §5는 수신 part ownership을 public caller에게 넘기고 caller가 close하도록 한다. `11-synchronization-model`은 application socket state와 connection engine/pipe session end의 실행 owner를 분리한다. callback fusion은 public receive/send 의미와 owner를 함께 바꾸므로 구현 최적화가 아니다. | **D spec gap / 새 API 설계** |
| 준비되지 않은 다음 message를 기다려 write batch 채우기 | bounded batch는 현재 준비된 byte까지만 내고 traffic을 기다리지 않아야 한다. latency/backpressure 규칙을 바꾸는 delayed flush는 금지. | **C 우회** |
| 고정 128 KiB 이상 buffer를 모든 connection에 즉시 할당 | 64 KiB 셀은 해결하지만 작은 payload/고CCU memory를 항상 지불한다. 기존 adaptive target의 full-hit 규칙을 단순화하는 쪽이 같은 문제를 규칙 하나로 해결한다. | 채택 기각 |
| `rcvbuf=-1`일 때도 decoder max를 OS default 크기로 올리기 | cppserver와 가까워지지만 현재 코드는 4 KiB를 max로 삼고, OS socket buffer와 user-space owned buffer의 memory budget은 다른 결정이다. CCU별 memory 상한을 먼저 정해야 한다. | **D spec/policy gap, 분석 2–3 h** |

B2는 명시적 `rcvbuf`가 있는 경우에도 saturated small-frame connection의 target을 더 빨리 키울 수 있다. 기존 1 MiB max를 그대로 지키고 64 B/1 KiB의 처리량과 connection당 RSS를 함께 재측정해, syscall 이득이 memory 증가를 정당화할 때만 채택해야 한다.

## 7. 계약 대조와 소유권

| 계약 | 관련 조항 | 이 보고서의 적용 |
|---|---|---|
| `core/doc/spec/core/socket/08-stream.ko.md` | §5 `:150-171` RAW part ownership; §6.3 `:239-242` bounded queue/backpressure; §10 `:274-317`; runtime defaults `:374-410` | read buffer 성장과 speculative drain은 Core engine 내부 최적화로 허용되지만 part ownership, queue bound, 순서를 바꾸면 안 됨 |
| `core/doc/spec/core/protocol/01-zmp.ko.md` | §9 `:411-414` header/payload copy 회피, WS bounded batch `:489-492` | 이 계측은 RAW라 ZMP wire 계약의 직접 대상은 아니다. 다만 engine batching은 “준비된 byte만 상한까지, 대기하지 않음”이라는 같은 bounded 규율을 유지해야 함 |
| `core/doc/spec/core/systems/11-synchronization-model.ko.md` | §1 app/I/O owner `:21-47`, §3.1 socket turn `:69-82`, §3.2 pipe SPSC `:99-121`, §3.3 mailbox wake `:131-151`, §4 hot-path lock `:191-204` | read target·read evidence는 connection I/O thread 한 owner 안에서 바꿈. app/I/O 경계를 callback으로 우회하거나 동일 상태를 둘로 만들지 않음 |

소유 계층은 다음처럼 분리된다.

| 결정 | owner |
|---|---|
| kernel read 크기, speculative read 여부, async rearm | Core Asio engine / RAW decoder policy |
| RAW part ownership과 public recv/send | STREAM socket 공개 계약 |
| app↔I/O 전달, pipe wake와 mailbox | Core pipe/mailbox 및 synchronization model |
| benchmark frame 조립/echo | benchmark application; Core가 이 protocol을 알아서는 안 됨 |

Framework runtime 변경은 없고 교차언어 public 동작도 바꾸지 않았다. 따라서 Framework 언어 구현 대조 대상은 아니다.

## 8. 산출물·검증·한계

| 항목 | 결과 |
|---|---|
| source/spec 변경 | 없음 |
| build/test | 없음(분석 전용, 기존 바이너리 사용) |
| 동적 셀 | strace 6개 + Callgrind 6개 + 무클라이언트 baseline 3개, 전부 정상 종료 |
| protocol 결과 | 모든 셀 `parse_error=protocol_error=send_error=0` |
| 원본 | scratch `sd/strace_*.summary`, `callgrind_*.out`, `*.functions.tsv`, `*.json`, server/client logs |
| 제한 | loopback CCU20/io1 축소 셀이다. Callgrind는 thread를 직렬화한다. allocation은 connection setup 포함 상한이다. TCP `async_read_some`은 큰 buffer라도 논리 frame 1회 수신을 보장하지 않는다. |

최종 판정: cppserver의 큰 payload 이득은 **적은 read syscall + 64 KiB 경계의 적은 send syscall + read/write overlap + strand 제거**의 합이다. 그중 zlink가 아직 얻지 못한 실질적 이득은 CppServer 구조를 복제하는 것이 아니라, 이미 가진 zero-copy RAW 경로가 작동하도록 **read target 성장 규칙과 speculative read 진입 규칙을 각각 하나로 단순화**하는 데 있다.
