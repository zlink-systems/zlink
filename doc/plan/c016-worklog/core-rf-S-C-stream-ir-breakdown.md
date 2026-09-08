# S-C — STREAM 잠금 축소 뒤의 메시지당 명령 비용

이 보고서는 잠금 축소 뒤 STREAM과 Asio·zmq의 차이를 설명하고, 계약을 유지하는 다음 작업의 우선순위를 정하기 위한 분석이다. **분석 전용이며 소스·스펙·commit·stash 변경은 없다.**

> 측정일: 2026-09-08. 지정 ref `wip/0.17.3-all2`, detached HEAD `1a79625d3d`, worktree `/home/hep7hep7/project/zlink-work/sc`.
> VERSION은 이 ref에 기록된 **0.17.2**다. 요청의 “0.17.4 베이스” 표기와 달라, 본 보고서는 지정 ref·commit을 기준으로 삼는다. ALL-2b·ALL-3의 병행 미커밋 변경을 포함하지 않는다.
> 원본·스크립트: `/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/sc/`.

## 1. 결론과 측정 범위

STREAM은 ZMP 메시지를 해석하지 않아도 **일반 socket API의 소유권·lifecycle, byte HWM을 가진 pipe, command mailbox, Asio 실행기**를 거친다. 이번 축소 셀에서 zlink의 `pthread_mutex_lock` 호출은 약 **8.55회/msg**로 Asio 약 **12.0회/msg**보다 적다. socket turn의 별도 atomic RMW 비용까지 없다는 뜻은 아니다. 남은 차이는 잠금 횟수보다 수신 API와 I/O↔앱 전달에 있다. 이 전달 자체는 pull 모델의 설계 비용이며, 이를 제거하자는 D-c는 철회된 상태로 유지한다.

- hotpath RAW: **13,804.21 Ir/msg**. 제시된 13,909 대비 −0.75%. reference를 수정하거나 전체 gate를 반복하지 않았다.
- with_stream zlink RAW: 빈 서버 시작·종료 비용을 뺀 **9,040.52 Ir/msg**. Asio **4,998.56**, zmq **7,273.83 Ir/msg**. 모두 같은 빈 서버 차감 방식이다.
- **더 줄일 후보는 있다.** 다만 아래 B 범위는 수정·after 측정이 없는 설계 추정이다. 후보의 중복 없는 작업 범위를 합쳐 축소 셀 **600–1,000 Ir/msg**, hotpath **700–1,200 Ir/msg**를 다음 캠페인의 검증 범위로 제안한다. 달성 보장은 아니다.

### 1.1 요청의 packet 명칭과 실제 셀

| 확인 항목 | 실제 경로와 해석 | 기준 코드(모두 지정 ref) |
|---|---|---|
| hotpath `stream_tcp` | RAW, 1 connection, 1개 payload in flight, 1024 B. `recv_part/send_part_rid`; PACKET API 아님 | `core/tests/perf/hotpath_bench.cpp:497`, `:510`, `:400`, `:441` |
| with_stream `zlink` | RAW, public poller→recv_part→프레임 확인. 완전한 frame 하나가 chunk에 들어오면 같은 msg를 send_part_rid로 echo; 그 밖에는 조립/복사 | `bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp:174`, `:202`, `:317` |
| 요청의 `recv_packet` pump·framing | **두 zlink 측정 셀에서 실행 0**. 따라서 해당 단계는 RAW recv API로 대응한다. 별도 `zlink_packet` 셀 결과를 섞지 않는다 | `core/src/runtime/sockets/stream/stream.cpp:1027`, `:1073`; 08-stream §3·§5·§6 |
| engine decode | allocator-buffer 안의 raw byte span은 msg로 감싸고, 밖의 입력에는 owned-copy fallback을 사용. PACKET 길이 prefix 파서가 아님 | `core/src/runtime/protocol/raw_decoder.cpp:38` |
| routing-ID envelope | zlink RAW는 4-byte RID를 별도 출력으로 복사한다. 별도 RID msg를 생성하지 않는다. zmq STREAM은 RID+payload msg API를 사용한다 | `stream.cpp:1060`; libzmq 원본 `src/stream.cpp:146` 이후 |
| Asio thread | 전체 worker는 4개. 연결별 strand에서 read callback→write를 이어 별도 앱 thread로 전달하지 않는다. 서버 전체가 단일 thread라는 뜻은 아니다 | `bindings/c/bench/with_stream/stacks/asio/test_scenario_stream_asio.cpp:128`, `:239`, `:249`, `:293` |

### 1.2 측정 조건과 재현

| 항목 | 조건 |
|---|---|
| 빌드 | `JOBS=4`. hotpath: `core/build-gate`, Release+LTO, tests ON, 정적 링크. zlink 서버가 쓰는 Core: `core/build`, Release+LTO, tests OFF, 공유 링크. **벤치 실행 파일들은 C binding 기본 Release(-O3), ENABLE_LTO=OFF**. zmq는 기존 배포 라이브러리. dev 수치 혼용 없음 |
| 라이브러리 확인 | `ldd` 및 실행 `/proc/PID/maps`가 sc의 libzlink와 복제한 기존 libzmq 4.3.5 배포 파일을 가리킴. `provenance.json`에 SHA256 보존 |
| 자원·직렬화 | 빌드·valgrind 전 ninja=0, available≥6000 MB. 측정은 추가로 load 1분≤2.0 및 PERF_LOCK. 부족하면 60초 단위 대기. 후반 측정은 make/cc1plus/lto1/ld도 검사. 시작·종료 load 및 대기 기록 보존 |
| hotpath 구간 | warmup 100회는 Callgrind instrumentation 밖. 20,000 echo loop를 측정. 같은 프로세스의 BSD client send/recv·검증 포함 |
| 서버 구간 | CCU20, size1024, io_threads4, sndbuf/rcvbuf1048576, backlog32768, tcp-nodelay1. 현행 client에는 `--warmup`이 없어 2초 client와 15초 client를 같은 서버에 순차 실행. 연결은 다시 생성되며 **서버 recv_msgs 전체**를 분모로 사용. msg는 echo 앱 frame이며 TCP segment·RAW chunk 수가 아님 |
| 빈 서버 차감 | 같은 명령의 서버를 기동하고 연결 없이 1초 뒤 정상 종료한 Ir를 차감. 두 client 실행의 누적 40개 연결 생성·해제 비용은 남는다(CCU는 각20). 단계 표에도 같은 baseline을 단계별 차감 |
| Callgrind | `--tool=callgrind --dump-instr=no --collect-jumps=no --cache-sim=no --separate-callers=8`; hotpath만 `--instr-atstart=no` |
| syscall | 별도 `strace -f -c` 짧은 셀. Callgrind의 wrapper 호출 수와 동일 실행으로 취급하지 않음 |
| 한계 | Ir는 사용자 공간 명령 수다. kernel 실행, cache miss·원자 연산 지연·CPU 주파수는 세지 않는다. Valgrind는 thread를 직렬화해 batch·wake 비율을 바꾼다. CCU20 Ir 비를 native CCU1000 처리량 비로 직접 역산하지 않음 |

재현 명령과 stdout·METRIC은 `run_cell.py`, `measure.sh`, `resource_gate.py`, 최종 잔여 측정의 `lock_remaining.py`·`remaining.sh`, `cg_*.json`, `cg_*.server.log`, `cg_*.client*.log`에 있다. hotpath 실행은 `cg_hotpath.log`와 `cg_hotpath.out`에 보존했다. 모두 포그라운드에서 종료를 확인한다.

| Callgrind 셀 | 분모 msg | gross Ir | 빈 서버 Ir | net Ir/msg | 시작 KST | 시작 load 1/5/15분 | 종료 load 1/5/15분 | 시작 available MB |
|---|---:|---:|---:|---:|---|---|---|---:|
| hotpath | 20,000 | 276,084,191 | 0 | 13,804.21 | 15:20:28 | 1.83 / 6.05 / 8.93 | 미기록 | 10527 |
| zlink | 52,974 | 482,530,511 | 3,618,084 | 9,040.52 | 15:28:20 | 1.84 / 6.64 / 8.46 | 1.73 / 6.38 / 8.34 | 10547 |
| asio | 128,749 | 645,637,050 | 2,076,821 | 4,998.56 | 15:31:07 | 1.77 / 4.59 / 7.38 | 1.96 / 4.49 / 7.30 | 10557 |
| zmq | 75,504 | 552,507,749 | 3,304,863 | 7,273.83 | 15:34:49 | 1.77 / 3.53 / 6.39 | 1.88 / 3.47 / 6.32 | 10552 |

| 빈 서버 baseline | Ir | 시작 KST | 시작 load 1/5/15분 | 종료 load 1/5/15분 | 시작 available MB |
|---|---:|---|---|---|---:|
| zlink | 3,618,084 | 15:37:36 | 1.49 / 2.55 / 5.55 | 1.45 / 2.53 / 5.52 | 10579 |
| asio | 2,076,821 | 15:57:48 | 1.63 / 4.10 / 5.08 | 1.63 / 4.10 / 5.08 | 10505 |
| zmq | 3,304,863 | 16:54:35 | 1.27 / 2.29 / 2.15 | 1.27 / 2.29 / 2.15 | 10562 |

모든 채택 실행의 시작 ninja=0. hotpath는 시작 load를 보존했으나 초기 wrapper가 종료 load를 기록하지 않았다. 나머지 원본 load는 `cg_*.json`, `baseline_*.json`, `resources.log`에 있다. 5/15분 load에는 앞선 다른 job의 빌드 이력이 남아 있으며, 시작 판정은 현재 ninja·메모리와 1분 load 기준이다.

## 2. 경로 단계별 Ir/msg

**합산하는 열은 self 비용이다.** 함수 본체와 호출자 문맥으로 각 self 비용을 한 번만 귀속한다. libc·allocator 같은 공통 함수는 가까운 호출 경로를 따른다. 8 caller 밖에서도 모든 유입 경로가 같은 단계이면 그 단계로 귀속한다. 방향을 확정하지 못한 zmq mailbox는 별도 공통 행에 남겨 정확한 방향별 수치처럼 만들지 않는다.

PACKET pump·packet queue·PACKET prefix 파서는 전 셀의 해당 Core 경로에서 0이다. 표의 06은 실제 RAW recv 경로다. 07에는 벤치 echo 코드와 public send 제출을 함께 넣었으므로 그 행 전체를 “벤치 앱만의 비용”으로 빼면 안 된다. 12는 read와 write가 공유하는 reactor/실행기 본체다.

| 단계 | hotpath RAW | with_stream zlink RAW | asio | zmq STREAM |
|---|---:|---:|---:|---:|
| 01 TCP read syscall 진입·복귀 | 133.0 | 133.0 | 64.1 | 64.0 |
| 02 engine read 준비·RAW decode | 1,649.5 | 1,663.3 | 1,349.8 | 689.7 |
| 03 수신 pipe write·flush | 439.8 | 452.0 | 0.0 | 200.1 |
| 04 I/O→앱 명령 발행 | 451.5 | 167.6 | 0.0 | 43.6 |
| 05 앱 wake·poll·mailbox drain | 2,018.5 | 713.2 | 0.0 | 445.4 |
| 06 public recv·수신 pipe read | 2,300.9 | 1,661.2 | 0.0 | 1,032.5 |
| 07 앱 echo·public send 제출 | 1,644.0 | 1,425.9 | 1,037.9 | 2,765.5 |
| 08 송신 pipe write·flush | 334.2 | 339.1 | 0.0 | 152.4 |
| 09 앱→I/O 명령·drain | 1,064.6 | 436.7 | 0.0 | 182.1 |
| 10 engine output·송신 pipe read | 1,325.2 | 1,177.1 | 1,154.0 | 1,205.5 |
| 11 TCP write syscall 진입·복귀 | 64.0 | 64.0 | 64.0 | 64.0 |
| 12 공유 I/O reactor·실행기 | 1,667.8 | 804.7 | 1,328.6 | 102.8 |
| 13 동일 프로세스 client·검증 | 711.0 | 0.0 | 0.0 | 0.0 |
| 14 미귀속·시작/종료·공통 잔여 | 0.1 | 2.8 | 0.2 | 2.3 |
| 15 방향 미확정 mailbox 공통 | 0.0 | 0.0 | 0.0 | 323.7 |
| 합계 | 13,804.2 | 9,040.5 | 4,998.6 | 7,273.8 |

- hotpath의 client/harness **711 Ir/msg**는 별도 행이다. 그 외에도 hotpath 서버 echo helper의 malloc·복사와 blocking recv가 있으므로 711만 빼도 with_stream과 같은 셀이 되지는 않는다.
- zlink RAW의 정상 allocator-buffer 경로는 decoder에서 zero-copy로 전달되며, allocator 밖 입력에는 owned-copy fallback이 있다(`raw_decoder.cpp:45–69`). encoder의 output batch 복사도 남는다. hotpath에는 recv scratch 복사와 새 echo msg로의 복사도 있다.
- libc IFUNC를 이름 없는 함수로 누락하면 복사를 과소 집계한다. 이 환경에서 `memcpy/memmove=libc+0x188c00`, `memset=+0x189600`, `memcmp/bcmp=+0x1884a0`를 실제 로드 주소로 확인했다(`libc-ifunc.json`). **LTO가 caller 안에 인라인한 복사는 이 libc 합계에도 포함되지 않는다.** S-A의 “libc mem* 0%”를 무복사의 증거로 재사용하지 않는다.

| Callgrind에서 관찰한 호출 (calls/msg) | hotpath | zlink | asio | zmq |
|---|---:|---:|---:|---:|
| pthread mutex lock | 15.7346 | 8.5451 | 12.0073 | 1.9570 |
| TCP recv wrapper | 3.0000 | 2.0007 | 1.0011 | 1.0005 |
| TCP send wrapper | 2.0000 | 1.0000 | 1.0000 | 1.0000 |
| malloc | 3.0027 | 1.3451 | 0.0031 | 2.0179 |
| free | 3.1568 | 1.5115 | 0.0032 | 2.1518 |
| memcpy/memmove IFUNC | 2.0000 | 1.0511 | 2.0003 | 5.2239 |

빈 서버 비용을 차감하지 않은 호출 수다. hotpath의 lock 15.73/msg에는 blocking recv의 대기·condition variable 경로가 포함되며, public poller로 읽는 with_stream의 8.55/msg와 셀이 다르다. `malloc`과 `free`의 차이는 시작·종료, aligned allocation 등 서로 다른 entry point 때문에 생길 수 있으며 leak 판정값이 아니다. Callgrind의 futex 심볼 부재를 무경합의 증거로 쓰지 않는다.

### 2.1 비교 격차를 다시 묶은 표

| 차이 묶음 (Ir/msg) | zlink | asio | zlink−asio | zmq | zlink−zmq | 해석 |
|---|---:|---:|---:|---:|---:|---|
| 앱 recv 경계·pipe·양방향 명령/wake | 3,769.7 | 0.0 | +3,769.7 | 2,380.0 | +1,389.7 | pull/turn/HWM 자체는 A; receive 중간 표현 일부는 B |
| engine read 준비 + 공유 I/O reactor | 2,468.0 | 2,678.4 | -210.4 | 792.4 | +1,675.6 | Asio와 합산 비교 시 zlink가 더 크지는 않음; zmq의 직접 epoll 경로와는 격차 |
| 앱 echo + public send 제출 | 1,425.9 | 1,037.9 | +388.0 | 2,765.5 | -1,339.7 | 앱과 Core가 섞인 행. B2만 국소 후보 |
| engine output + TCP read/write + 잔여 | 1,376.9 | 1,282.2 | +94.7 | 1,335.9 | +41.0 | 필수 I/O와 복사 포함; B4는 조건부 후보 |
| 합계 | 9,040.5 | 4,998.6 | 4,042.0 | 7,273.8 | 1,766.7 | 단계 self를 다시 묶은 값; 새 비용을 더한 것이 아님 |

Asio에 별도 public recv 경계가 없다는 뜻이며, Asio가 recv 처리를 하지 않는다는 뜻은 아니다. 대응하는 callback 수신 처리는 engine read 행에 있다. 첫 묶음 약3,770 Ir/msg는 A와 B가 섞인 현재 구현 비용으로, 전부 제거 가능한 핸드오프 낭비가 아니다. 이 사용자 공간 분해는 kernel·대기 시간의 원인별 비율까지 측정한 것이 아니므로 native 0.80…0.84의 정확한 귀속률로 사용하지 않는다.

### 2.2 함수 상위 40개 — hotpath

전체 함수의 self 순위 상위 40개를 경로 단계별로 배열했다. 함수는 caller 문맥을 합친 값이다. **inclusive는 자식까지 포함한 참고값이며 서로 더하지 않는다.** 공통 함수의 self는 앞 표에서 호출 문맥별로 나눠 귀속했다. 이름을 복원하지 못한 libc 내부 함수는 주소와 확인된 상위 호출을 표시했다. 전체 이름·함수표는 `cg_hotpath.functions.tsv`, 문맥별 값은 `cg_hotpath.parsed.stage-functions.tsv`다.

| 경로 단계 | self 순위 | 함수 | self Ir/msg | inclusive Ir/msg | calls/msg |
|---|---:|---|---:|---:|---:|
| 02 | 24 | `asio_engine_t::start_async_read()` | 153.00 | 1,021.53 | 1.000 |
| 02 | 35 | `tcp_transport_t::async_read_some` | 130.00 | 656.00 | 1.000 |
| 03 | 33 | `pipe_t::write_message_unlocked(msg_t const*, bool, bool, pipe_message_admission_t*)` | 132.00 | 244.20 | 1.000 |
| 04 | 38 | `pthread_cond_broadcast` | 121.06 | 121.06 | 0.931 |
| 05 | 6 | `socket_base_t::process_commands(int, bool, bool, unsigned long const*, bool) [clone .constprop.1]` | 297.27 | 2,067.68 | 0.965 |
| 05 | 32 | `mailbox_t::wait_for_command_signal(int, unsigned long const*)` | 133.03 | 837.86 | 0.965 |
| 05 | 36 | `pthread_cond_clockwait` | 125.72 | 272.32 | 0.931 |
| 06 | 7 | `zlink_recv_part.constprop.0` | 260.00 | 3,967.83 | 1.000 |
| 06 | 15 | `socket_base_t::recv_routed` | 182.81 | 3,303.83 | 1.000 |
| 06 | 25 | `receive_once_guarded<recv_routed>` | 147.43 | 893.12 | 1.965 |
| 06 | 29 | `fq_t::recvpipe(msg_t*, pipe_t**)` | 142.22 | 505.34 | 1.965 |
| 07 | 17 | `stream_send_echo` | 177.00 | 2,774.17 | 1.000 |
| 07 | 26 | `socket_base_t::wait_for_completion_submit_admission` | 145.00 | 1,844.17 | 1.000 |
| 07 | 27 | `libc+0xa9c90 (malloc 하위)` | 144.00 | 168.00 | 1.000 |
| 07 | 28 | `socket_base_t::try_admit_send_parts_scoped` | 144.00 | 1,555.08 | 1.000 |
| 07 | 40 | `stream_t::xsend_routed` | 115.00 | 1,292.08 | 1.000 |
| 08 | 39 | `pipe_t::write_single_message_and_flush_no_recursive_hwm_check` | 117.00 | 1,130.08 | 1.000 |
| 10 | 19 | `asio_engine_t::prepare_output_buffer() [clone .part.0]` | 171.00 | 1,175.22 | 2.000 |
| 10 | 34 | `encoder_base_t<raw_encoder_t>::encode(unsigned char**, unsigned long)` | 130.00 | 480.00 | 3.000 |
| 12 | 2 | `asio_poller_t::loop()` | 460.09 | 5,999.63 | 0.000 |
| 12 | 31 | `.LTHUNK0.lto_priv.0` | 140.45 | 264.71 | 2.004 |
| 12 | 37 | `boost::asio::detail::epoll_reactor::descriptor_state::do_complete` | 122.00 | 2,878.89 | 1.000 |
| 13 | 18 | `memcmp/bcmp (IFUNC)` | 176.00 | 176.00 | 2.000 |
| 13 | 30 | `memset (IFUNC)` | 142.00 | 142.00 | 2.000 |
| 공통 01,05,12,13 | 11 | `libc+0x98630 (syscall 취소 진입)` | 195.26 | 195.26 | 10.848 |
| 공통 01,05,12,13 | 21 | `libc+0x986b0 (syscall 취소 복원)` | 162.72 | 162.72 | 10.848 |
| 공통 02,07,09 | 20 | `malloc` | 164.18 | 731.47 | 3.003 |
| 공통 02,10,12 | 13 | `libc+0xab250 (free 하위)` | 190.14 | 284.38 | 3.003 |
| 공통 04,05,09,12 | 1 | `pthread_mutex_lock` | 566.62 | 567.62 | 15.735 |
| 공통 04,05,09,12 | 3 | `pthread_mutex_unlock` | 413.30 | 420.66 | 15.735 |
| 공통 04,09 | 16 | `mailbox_t::send(command_t const&)` | 181.70 | 1,247.43 | 1.965 |
| 공통 05,06 | 22 | `clock_t::now_ms()` | 154.37 | 190.33 | 4.792 |
| 공통 05,06,07 | 14 | `socket_lifecycle_coordinator_t::lock_public_api_sync()` | 187.55 | 187.55 | 5.861 |
| 공통 05,09 | 5 | `mailbox_t::recv(command_t*, int, bool) [clone .constprop.0]` | 324.98 | 602.01 | 4.896 |
| 공통 06,07,10 | 12 | `msg_t::close()` | 195.13 | 391.64 | 6.965 |
| 공통 06,10 | 8 | `memcpy/memmove (IFUNC)` | 250.00 | 250.00 | 2.000 |
| 공통 06,10 | 9 | `pipe_t::read(msg_t*)` | 213.54 | 665.69 | 4.965 |
| 공통 06,10 | 10 | `ypipe_t<msg_t, 64>::read(msg_t*, bool*)` | 196.96 | 197.15 | 3.965 |
| 공통 06,10 | 23 | `pipe_t::account_inbound_frame(msg_t const*, bool)` | 154.00 | 230.00 | 2.000 |
| 공통 07,09 | 4 | `libc+0xab650 (malloc 하위)` | 366.30 | 567.30 | 2.003 |

## 3. Syscall과 wake

| syscall (calls/msg) | hotpath (client 포함) | zlink | asio | zmq |
|---|---:|---:|---:|---:|
| recvfrom | 3.0000 | 2.0011 | 1.0008 | 1.0013 |
| sendto | 2.0001 | 1.0001 | 1.0000 | 1.0001 |
| recvmsg | 0.0002 | 0.0003 | 0.0000 | 0.0003 |
| sendmsg | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| read | 2.0007 | 0.6012 | 0.0001 | 0.4564 |
| write | 1.0004 | 0.5296 | 0.0001 | 0.4562 |
| epoll_wait | 2.1469 | 0.4364 | 0.1021 | 0.4228 |
| epoll_ctl | 0.2083 | 0.0085 | 0.0010 | 2.0053 |
| poll | 0.0000 | 0.0751 | 0.0000 | 1.0863 |
| ppoll | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| futex | 7.8509 | 1.5404 | 0.2704 | 0.0146 |
| getpid | 1.0011 | 0.3958 | 0.0000 | 1.9344 |
| total | 19.2222 | 6.6238 | 2.3801 | 8.4098 |

| strace 셀 | 분모 msg | 총 calls | syscall 오류 반환 수 | 시작 KST | 시작 load 1/5/15분 | 종료 load 1/5/15분 | 시작 available MB |
|---|---:|---:|---:|---|---|---|---:|
| hotpath | 20,100 | 386,366 | 67,324 | 16:54:47 | 1.54 / 2.32 / 2.16 | 1.38 / 2.23 / 2.13 | 10583 |
| zlink | 17,586 | 116,487 | 23,232 | 16:54:36 | 1.27 / 2.29 / 2.15 | 1.33 / 2.29 / 2.15 | 10564 |
| asio | 49,771 | 118,462 | 2,688 | 16:54:40 | 1.33 / 2.29 / 2.15 | 1.54 / 2.32 / 2.16 | 10592 |
| zmq | 15,725 | 132,244 | 63 | 16:54:43 | 1.54 / 2.32 / 2.16 | 1.54 / 2.32 / 2.16 | 10590 |


hotpath strace는 instrumentation과 무관하게 warmup도 세므로 분모는 20,100 echo다. 같은 프로세스의 client TCP send/recv 각 약1회/msg를 포함한다. 서버 단독 syscall과 직접 비교할 때 이 차이를 고려한다. 시작·종료 syscall은 strace에서 별도 차감하지 않았다. `strace -c`의 read/write 합계는 fd를 구분하지 않으므로 eventfd의 방향별 수치로 재명명하지 않는다. syscall 오류 반환에는 정상 비차단 동작의 EAGAIN도 포함될 수 있으며 서버 METRIC 오류와 별개다.

이번 strace의 zlink recvfrom은 35,192 calls 중 17,586회 오류 반환, Asio는 49,813 calls 중22회다. 오류 종류는 `strace -c`로 확정할 수 없지만, 소스의 비차단 arming probe와 부합한다. futex는 zlink1.540/msg, Asio0.270/msg이며 **mutex 경합만의 카운터가 아니다**. hotpath의 blocking recv는 futex7.851/msg로 별도 셀의 동작 차이를 보여준다. strace 시간 비율은 대기 시간을 포함하므로 CPU 원인 비율로 사용하지 않는다.

Callgrind 호출에서는 zlink TCP recv가 **2.001/msg**, Asio는 약1/msg다. S-9가 제거한 read drain의 추가 EAGAIN을 다시 후보로 올리지 않는다. 남은 Asio arming probe는 실제 수신보다 먼저 다음 async read를 제출하는 시점과 관련된다. 이를 없애려면 transport의 readiness 처리 방식부터 검증해야 하며, 기존 API로 확인되지 않은 private Asio 우회나 두 번째 poller를 제안하지 않는다.

## 4. 차이의 분류와 절감 후보

여기의 **A/B/C는 비용 분류**다: A=설계·계약 비용, B=계약을 유지한 축소 후보, C=계약 변경 필요. 저장소의 변경 분류 A/B/C/D와 혼동하지 않는다. 구현 변경은 없으며, B 후보가 실제로 성립하는지는 별도 job이 증명한다.

### 4.1 A와 실행되지 않는 후보

| 항목 | 판정 | 계약·코드 근거 |
|---|---|---|
| I/O↔앱 pull 전달·SPSC sleep→activate_read | A. zmq에도 존재. 메시지마다 지연 없이 진행해야 하는 활성 전이는 제거 대상 아님 | 11-synchronization-model §3.2·§3.3; `core/src/runtime/core/ypipe.hpp:72`, `pipe_write.cpp:1359` |
| send 경로 command 생략·추가 wake 병합 | 소비자가 이미 깨어 있으면 기존 ypipe flush가 activate_read를 생략한다. 잠든 I/O owner를 깨우는 명령은 A. 앱이 직접 engine write를 실행하거나 새 pending 플래그·지연 timer로 보상하는 안은 B에 포함하지 않음 | 11 §3.2·§3.3의 전이 시 알림과 I/O thread의 engine 소유권; `ypipe.hpp:72–92` |
| thread-safe socket turn·close admission | A. `public_api_state` bit를 RMW로 보존하는 소유권 규칙은 유지 | 11 §3.1·§3.4; `socket_lifecycle_runtime.cpp:95`, `:118` |
| byte HWM·credit 회계 | A인 요구와 B가 될 구현 중복을 구분. STREAM도 backpressure를 제공하므로 raw라는 이유로 회계를 없앨 수 없음 | 08-stream §4·§8, 11 §3.2; `pipe_receive.cpp:707`, `pipe_write.cpp:1110` |
| packet당 msg_t heap→pool | 이 RAW 셀의 원인을 잘못 지정한 가설. decoder buffer는 spare 재사용하며 stack msg 자체가 packet마다 malloc되는 것이 아님. hotpath echo helper의 `init_size(1024)`는 벤치 선택 | `raw_decoder.cpp:65`, `hotpath_bench.cpp:445`; S-3 후속 반증 |
| raw pipe→packet queue 이중 버퍼, RID envelope, prefix per-byte parser | 측정 경로에서 0. 제거해도 이 셀 이득 0 | 08-stream §3·§5·§6, `stream.cpp:1027`, `:1073` |
| 무조건적인 refcount RMW 제거 | A의 수명 pin과 B의 중복 복사를 구분해야 함. RAW msg close는 shared flag가 없으면 content refcount sub를 생략함. RID 조회도 이미 turn 안에서 snapshot을 직접 읽음 | `msg.cpp:414`, `stream.cpp:159`, 11 §3.1·§3.2 |
| 중복 private eventfd | G-7이 이미 executor만 사용하는 mailbox의 private signal을 생략함. 같은 절감치를 다시 합산하지 않음 | `mailbox.cpp:89`, `:219`; 11 §3.3 |
| 빈 queue 확인 전부 제거 | 불가. consumer가 sleep marker를 발행해야 다음 producer의 wake가 발생한다. B4는 확인 자체의 제거가 아닌 중복 준비 절차의 통합 후보 | 11 §3.2; `ypipe.hpp:94`, `asio_engine.cpp:1244` |

### 4.2 B 후보 — 실측 비용 안에서의 설계 추정

각 수치는 해당 경로의 일부만 줄인다는 추정이며 inclusive 값을 통째로 절감액으로 사용하지 않았다. B1/B2는 recv/send, B3/B4는 read/write 경계로 나눠 서로 같은 명령을 세지 않는다. 성공 경로만 빨라지고 실패 소유권을 깨뜨리는 안은 채택하지 않는다.

| 우선 | 후보·소유 모듈 | 관찰한 비용·근거 | 예상 절감 Ir/msg (CCU20 / hotpath) | 난이도·위험 | 유지할 계약 |
|---|---|---|---:|---|---|
| B1 | **단일-part receive의 중간 출력 표현 통합**, Core API receive | RAW도 `recv_parts_once`→thread-local `recv_tls_view` 임시 출력→`zlink_msg_move`→`multipart_close`를 거침. 06 비용 zlink1,661, hotpath2,301. 모두 절감 가능하다는 뜻은 아님 | 250–450 / 300–500 | 중·중. multipart continuation과 source RID 수명, output 실패 원자성 | 08 §5·§7, 11 §3.1·§3.4. `socket_message_api.cpp:186–224`, `socket_message_recv_api.cpp:117–194` |
| B2 | **routed submit의 계층·인자 전달 통합**, Core send admission | `send_completion_submit_blocking`→`wait_for_completion_submit_admission`→`try_admit_send_parts_scoped`가 같은 정상 제출을 전달. wait 함수 self 약145/msg, try_admit self 약143/msg. S-5의 단순 guard 지연·강제 inline 반증은 유지 | 150–250 / 150–250 | 중상·높음. 거절과 wait 등록의 원자성, errno, DONTWAIT completion | 08 §4·§7, 11 §3.1·§3.4. `socket_send_submit.cpp:389`, `:429`, `:476`, `:509` |
| B3 | **transport completion 표현 통합**, Core Asio transport | custom allocator handler가 `completion_handler_t=std::function`으로 변환되고 TCP lambda가 이를 감쌈. read당 `operator new` 약1회, new/delete의 inclusive 합은 약161 Ir/msg. weak/shared 소유권 복사·호출 전달 비용도 별도로 존재. Release asm은 custom wrapper에 **32-byte new** 확인 | 100–200 / 150–250 | 높음·높음. late callback·cancel·재연결 세대 수명. 단순 raw pointer 대체 금지 | 08 §7, 11 §3.2·§6. `i_asio_transport.hpp:55`, `asio_engine.cpp:498`, `tcp_transport.cpp:413`; `start_async_read.asm` |
| B4 | **output queue drain 판정과 다음 write 준비 통합**, Core engine | `prepare_output_buffer` **2/msg**, 송신 쪽 pipe read **3/msg**: batch를 채우며 empty를 확인한 뒤 write 완료 후 다시 prepare. `_output_stopped`와 queue sleep 전이를 같은 결정으로 사용할 수 있는지 검증 | 100–180 / 100–180 | 중상·높음. producer가 중간에 새 데이터를 넣는 경쟁, partial write, lost wake. 구현 전 반례 검사 필수 | 08 §4·§10, 11 §3.2·§3.3. `asio_engine.cpp:1220`, `:1244`, `:1346`, `:1372` |

**합산 검증 범위는 보수적으로 CCU20 600–1,000, hotpath700–1,200 Ir/msg**로 둔다. 위 개별 범위 끝점을 모두 동시에 달성한다고 전제하지 않는다. B4가 실제 중복을 증명하지 못하면 그 항목은 0으로 내려야 한다. B3도 allocator 정보가 지워진다는 사실과 이득 있는 대체 표현이 존재한다는 판단을 구분한다.

### 4.3 대안 비교와 규칙 수

| 대상 | 제외하는 안 | 검증할 안·선택 이유 | 수정 전→목표 규칙/소유자 수 |
|---|---|---|---|
| B1 | STREAM만 내부 API를 직접 호출하는 별도 fast path | 기존 단일 terminal-part 출력 경로를 공통 receive 소유자에 통합. thread-local 임시 출력→caller 인계의 중복 표현을 줄임 | 단일 part 중간 출력 표현 2→1 |
| B2 | 새 옵션, retry, wait guard 지연만 추가 | 제출·거절·wait 등록을 같은 admission 소유자에서 처리하고 전달 계층을 합침 | 같은 제출의 여러 전달 단계→admission owner1; 공개 규칙 추가0 |
| B3 | handler pool 크기 증가 또는 수명 pin 삭제 | 기존 completion 표현을 하나로 정리해 type erasure와 allocator 전달 단절을 함께 해결. 내부 transport 전반 대조가 선행 | callback 표현 wrapper→std::function→transport lambda 3경계→목표 단일 소유 표현 |
| B4 | `_next_msg` miss 확인 삭제, N개/Tµs 지연 batching | 기존 queue sleep/`_output_stopped`가 다음 activation까지 결정의 근거가 되게 통합 | 시점이 다른 drain 관측 2회→1회 통합 가능성 검증; 중복 여부 미확정 |

B4의 첫 empty 관측은 consumer sleep을 발행하고, 후속 관측은 그 사이 producer publication을 확인한다. `restart_output()`이 async write 중 실행되면 `speculative_write()`가 `write_pending`에서 반환하므로, completion과 activation 순서를 보존할 수 있다는 증명이 선행해야 한다(`asio_engine.cpp:1277–1280`, `:1463–1477`; `ypipe.hpp:101–118`).

현재 실제 수정 전/후 규칙 수는 **동일(변경0)**이다. 표의 화살표는 다음 구현 job의 수용 조건이며 이미 구현했다는 뜻이 아니다.

### 4.4 C — 계약 변경이 필요한 D 항목

| D 후보 | 충돌 조항 | 얻을 수 있는 범위와 판정 |
|---|---|---|
| thread safety를 끄고 단일 caller만 허용 | 08 §7, socket 공통 §2, 11 §3.1 | turn 비용 일부. 설계 철학과 충돌하므로 B 예측에서 제외 |
| 앱 send/command wake를 N개 또는 Tµs까지 지연 | 08 §4의 진행·WRITABLE, 11 §3.3의 전이 시 알림, Polling level | wake 빈도 일부 감소 가능하나 latency/readiness 변경. 새 budget·timer 안은 제외 |
| RAW public owned msg를 다음 recv까지의 borrowed payload로 변경 | 08 §5의 msg ownership | API 표현·소유권 비용 일부. source RID의 borrowed view와 payload ownership을 혼동하면 안 됨. ABI/API 변경 필요 |

**D-c “pull 핸드오프 제거”는 철회됐으며 위 D 후보로도 재등록하지 않는다.** Asio와의 모델 차이는 비교 설명이고, 구현 목표는 같은 pull 모델인 zmq에 근접하는 것이다.

## 5. zmq가 같은 핸드오프를 싸게 처리하는 위치

libzmq 배포판은4.3.5이며 읽기 대조 소스는 저장소 원본 커밋 `e01e8afe31`의 `src/`다. 서로 다른 thread-safety와 byte-HWM 계약을 숨긴 채 zmq 코드를 그대로 복제하지 않는다.

| 비교 항목 | zlink의 위치 | zmq의 방식 | 판정 |
|---|---|---|---|
| reactor·read 준비 | engine read 약1,663 + shared reactor 약805 Ir/msg | 약690 +103. epoll readiness→raw engine read, 별도 Asio handler 구성 없음 | 주요 격차. transport callback B3는 일부만 줄임. reactor 교체 전체를 ≤3h job으로 약속하지 않음 |
| pipe 쓰기·회계 | RX452 + TX339 | RX200 + TX152. `pipe.cpp` write→ypipe→flush가 얇고 byte-credit 회계가 다름 | 차이 약439. 계약상 필요한 byte HWM은 A; 모든 차이를 B로 잡지 않음 |
| mailbox publish | MPSC 삽입 외에 public poller 알림·async executor post·수명/재스케줄 상태 | `mailbox.cpp:33`: 삽입 lock→cpipe write/flush→필요 시 signaler. `pipe.cpp:249`는 sleep 전이 때만 activate_read | 같은 핸드오프도 실행기까지 전달하는 단가가 다름. zmq publish 경계 inclusive RX190/TX217 Ir/msg. 단계 표의 공통323.7을 방향별 행에 다시 더하지 않음 |
| syscall 개수와 단가 | strace 총6.624/msg, futex1.540 | 총8.410/msg이지만 futex0.0146. 대신 epoll_ctl2.005, poll1.086, getpid1.934/msg가 있음 | zmq의 낮은 사용자 공간 Ir를 적은 syscall 총수로 설명할 수 없음. syscall별 kernel 비용과 ptrace가 바꾼 batch를 별도로 봐야 함 |
| public receive | zlink 약1,661 Ir/msg, single-part 출력도 공통 part 인계 | zmq 약1,033. RID envelope까지 있지만 public socket 호출과 FQ 경로가 상대적으로 얇음 | B1 근거. zmq STREAM은 thread-safe socket이 아니므로 turn 비용 전체를 제거 목표로 삼지 않음 |
| write engine | zlink 약1,177 | zmq 약1,206. output batch와 empty 확인이 양쪽에 있음 | 엔진 write 전체가 격차라는 가설은 성립하지 않음. B4도 국소 검증만 |
| 벤치 echo·public send | zlink 약1,426 | zmq 약2,766. RID 문자열 map, frame 조립, `zmq_send` 응답 buffer 복사도 있음 | zlink가 유리한 항목. 총 Ir만으로 순수 Core 격차를 계산하지 않음 |

## 6. 도달 범위와 후속 job

| 모델 | 추정과 의미 |
|---|---|
| B4 제외, B1–B3만 성립하는 경우 | CCU20 절감 추정 500…900 Ir/msg → **8,140…8,540 Ir/msg**. callback 대체의 feasibility도 여전히 검증 대상 |
| 후보 적용 후 CCU20 | 9,040.5 − (600…1,000) = **8,040…8,440 Ir/msg** |
| 후보 적용 후 hotpath | 13,804.2 − (700…1,200) = **12,604…13,104 Ir/msg** |
| with_stream zlink/asio, CPU 비용 절반만 가속된다는 민감도 가정 | 제공된 native 기준 R₀=0.80…0.84, S=600…1,000, I=9,040.5에 `R=R₀/(1−0.5×S/I)` 적용: **약0.83…0.89** |
| 전체 병목이 해당 사용자 공간 명령에 비례한다는 낙관적 상한 모델 | `R=R₀/(1−S/I)` → **약0.86…0.94**. syscall·cache·스케줄링 병목을 무시하므로 약속할 수 있는 성능이 아님 |
| zmq 목표 | 같은 모델에 최근 계획의 zlink/zmq≈0.91을 넣으면 부분 가속 약0.94…0.96, 낙관 모델 약0.97…1.02. **1.0 근접 가능성은 있지만 이 분석만으로 도달을 확정하지 못함** |

0.80…0.84는 사용자가 제시한 native CCU1000 범위다. 이번 CCU20 Callgrind 처리량을 그 기준의 새 native 측정값으로 바꾸어 쓰지 않았다. 모델의 0.5는 실측 계수가 아닌 민감도 가정이다. B 후보 적용 후 반드시 idle native 셀로 확인한다.

| 우선 job | 상한 | 완료 조건 |
|---|---:|---|
| SC-1 단일-part receive 인계 통합 | 3h | 기존 terminal output 경로 재사용 가능성 확인→구현 시 중간 소유 표현 제거, RAW/PAIR/DEALER multipart 및 RID·실패 ownership 회귀 검증 |
| SC-2 send admission 전달 계층 통합 | 3h | 같은 admission·wait 등록 owner 유지, 성공뿐 아니라 EAGAIN·route 제거·DONTWAIT completion 검증. 단순 inline만의 무효 변경 제외 |
| SC-3 completion 표현·allocator 전달 진단과 최소 대안 | 3h | 32-byte boxing과 callback 수명별 호출 수 확인, TCP/TLS/WS/IPC 계약 대조. 보상 상태·수명 pin 삭제 없이 성립하는 안만 다음 구현 대상으로 승인 |
| SC-4 output drain 결정 통합 타당성·수정 | 3h | 두 번째 empty probe 통합의 공개 API repro. activation이 write_pending 중 소비된 뒤 completion되는 순서, producer sleep/wake, partial-write/blocked-send/lost-wake 검증. 반례가 나오면 미채택 |

구현 시 각 job의 관련 suite→STREAM 공개 계약→변경된 동기화 경계 TSan 순으로 검증하고, 동일 조건 hotpath와 native1024 B 셀을 한 번 비교한다. ALL-2b의 HWM snapshot·bounded drain 수정과 ALL-3 allocator 변경은 각 job의 소유 범위이며 여기서 중복 작업으로 제안하지 않는다. 후속 job 착수 전 해당 통합 결과를 확인하고, 특히 B4의 engine write 관측이 이미 달라졌으면 변경된 경계만 다시 검증한다.

## 7. 산출물과 검증

- 변경 파일: 이 보고서와 `progress-S-C.md`만 메인 저장소에 기록. sc worktree의 tracked source/spec diff 없음.
- 실행: Release+LTO hotpath와 공유 Core 라이브러리, Release(-O3)·LTO OFF 벤치 실행 파일 빌드, hotpath stream_tcp Callgrind1회, zlink/asio/zmq 축소 셀 각1회, 빈 서버 baseline, 짧은 strace. 구현 수정이 없어 correctness suite·전체 gate는 실행하지 않음.
- 집계 검증: 모든 Callgrind self 합계와 `totals:` 일치; 단계 합계 검산. 서버 METRIC의 parse/protocol/send 오류0. 원본과 전체 함수·edge·문맥표 보존. `final-validation.txt`에 검산, `artifact-manifest.json`에 원본 SHA256 기록.
- 독립 문서 리뷰: 코드 부합·문서 원칙을 읽기 전용으로 검토했다. 감독이 지적된 코드와 11 §3.2·§3.3을 직접 재확인하고, 단일 frame echo 조건, decoder 복사 fallback, B4의 경쟁 검증 조건, thread-local 명칭, 목록 개수 표현을 모두 채택해 보정했다. B1–B3 근거에는 추가 충돌이 없었다. 최종 수치 추가 리뷰에서도 비교표·syscall·추정 구분을 확인했고, 실행 요약의 LTO 적용 범위를 명확히 고쳤다.
- 재확인 스펙: 08-stream §3·§4·§5·§6·§7·§8·§10, 11-synchronization-model §3.1–§3.4·§6·§8. **어느 문장도 다른 동작이 되지 않았다** — 분석만 했고 runtime·스펙을 바꾸지 않았다.
- 저장소 변경 분류: **분석 전용, runtime 변경 해당 없음**. 비용 B 후보는 향후 구현 전 별도 승인·검증 대상. 비용 C는 D 표로 분리.
- 남은 실패/한계: 채택 측정의 실패 없음. hotpath Callgrind 종료 load는 초기 wrapper에서 미기록. B는 after 수치가 없는 추정이며 native 비율은 민감도 모델이다.
