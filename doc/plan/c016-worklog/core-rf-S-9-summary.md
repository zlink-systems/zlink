# S-9 — TCP 수신 경로의 여분 `recv` 제거 (read drain)

worktree `~/project/zlink-work/s9` (detached `6f64e76b51`). 커밋하지 않음.
원자료: `<scratchpad>/S9/` (`cg_base.out`/`cg_after.out` = callgrind, `st_*_server.log` = read 크기 계측,
`bench_after*.log`, `ctest_*.log`).

## 1. 결과 (수치)

| 지표 (1024 B, callgrind 축소셀 CCU 20) | before | after |
|---|---|---|
| **`recv` 호출 / 메시지** | **3.001** | **2.001** |
| `speculative_read()` 호출 / 메시지 | 1.000 | **0** |
| `send` / 메시지 | 1.000 | 1.000 |
| eventfd `read` / `write` | 0.839 / 0.462 | 0.885 / 0.497 |
| `epoll_wait` / `epoll_ctl` | 0.521 / 0.102 | 0.503 / 0.088 |
| 명령 수 Ir / 메시지 | 11,349 | **10,967 (-3.4 %)** |

before 값은 S-A 보고서(§2: recv 3.000, Ir/msg 11,096)와 일치한다 — 같은 방법으로 재현했다.

남은 2회는 (a) 데이터를 실제로 가져오는 read 1회, (b) boost.asio가 `async_read_some()`을
걸 때 스스로 시도하는 non-blocking recv 1회(EAGAIN → epoll 등록)다. (b)는 asio
`epoll_reactor`의 arming 규약이라 엔진 코드에서 없앨 수 없다. asio 레퍼런스 서버의
1.000/msg까지 내려가려면 asio reactor 자체를 우회해야 하므로 이번 범위 밖이다.

## 2. 설계 판단의 근거 — read 크기 실측

계측용 카운터를 임시로 넣은 dev lib로 축소셀(CCU 20, 10 s)을 돌려 얻은 값이다(측정 후 계측 제거).

| | 1024 B | 65536 B |
|---|---|---|
| async read 완료 | 223,963 | 82,354 |
| — 요청을 **가득 채운** read | **0 (0 %)** | 59,981 (73 %) |
| — **짧은** read | 223,963 (100 %) | 22,373 (27 %) |
| 평균 read 크기 | 1,041 B (요청 4,096 B) | 49.2 KB |
| drain의 speculative read | 223,963 (1.00/msg) | 125,561 (2.03/msg) |
| — 데이터를 가져온 것 | **0** | 43,207 (평균 62 B) |
| — EAGAIN(헛수고) | 223,962 | 82,353 (1.33/msg) |

- 1024 B에서는 **모든 read가 짧고, 뒤이은 speculative read는 100 % EAGAIN**이다. 즉
  메시지당 정확히 1회의 순수한 낭비 syscall이 있었다.
- 64 KiB에서도 EAGAIN이 메시지당 1.33회다. 데이터를 가져오는 43,207회 중 요청을 채운 것은
  120회뿐 — 대부분 62 B짜리 꼬리 조각이고, 그 뒤에는 반드시 EAGAIN이 한 번 더 붙었다.

## 3. 설계 비교와 선택

브리프의 세 안은 실은 **같은 규칙을 다른 지점에 적용한 것**이다.

- (A) 짧은 read = "소켓이 비었다" → 즉시 async wait 복귀: drain **루프 안**의 종료 조건.
- (B) 첫 read가 버퍼를 채웠을 때만 speculative: drain **진입** 조건.
- (C) 둘 다.

선택은 **(C), 단 규칙을 하나로 둔 형태**다. `asio_stream_fastpath_policy.hpp`에
`stream_read_filled_request(request, bytes)` 하나를 두고, drain 진입·drain 반복·read target
성장 판정이 **모두 그 함수를 공유**한다. read target 성장 판정에는 이미 같은 뜻의 조건이
따로 적혀 있었으므로(`bytes_transferred_ < last_read_request_size_`), 중복 서술 하나가
오히려 **줄었다**. 새 옵션·플래그·상태는 없다(전달 인자 `last_read_bytes_` 하나뿐).

(A)나 (B)만 고르면 규칙이 한쪽에만 남아 "짧은 read의 의미"가 두 곳에서 달라진다 —
POSDDD(규칙 수 줄이기·중복 금지) 기준으로 (C)+공유 함수가 유일하게 규칙을 늘리지 않는 안이다.

정합성 근거: stream 소켓의 `recv`는 요청 길이까지 **커널에 있는 것을 전부** 돌려준다.
요청보다 적게 돌아왔다면 그 순간 수신 큐는 비어 있다. 그 뒤 도착하는 바이트는 새 readiness
이벤트를 만들고, 게다가 `start_async_read()`가 거는 asio의 arming recv가 한 번 더 확인한다 —
따라서 깨우기를 놓칠 수 없다. 놓친 데이터가 아니라 **가져올 데이터가 없었다는 확인**만
없앤 것이다. 64 KiB처럼 첫 read가 가득 찬 경우에는 종전대로 계속 읽는다(§4 처리량 확인).

## 4. 성능 (with_stream, CCU 1000, runs 1, worktree release lib)

| 셀 | 기준(plan §7.1 Phase 0) | after run1 | after run2 |
|---|---|---|---|
| zlink 64 B | 268.9 | 217.0 | **261.2** |
| zlink 1024 B | 243.0 | 241.6 | **241.1** |
| **zlink 64 KiB** | **30.4** | 27.97 | **29.59** |
| asio 64 B | 322.0 | 314.6 | 316.5 |
| asio 1024 B | 316.4 | 299.9 | **269.7** |
| asio 64 KiB | 39.2 | 37.70 | 37.39 |

측정 시 load average 5.5(run1) / 3.1(run2). **같은 러너 안의 asio 레퍼런스가 1024 B에서
316.4 → 269.7(-15 %)로 흔들렸다** — 이 머신은 다른 job의 빌드가 함께 도는 동안 셀 간
±15 % 편차가 있다. 그 편차 범위 안에서 세 크기 모두 기준과 같다고 읽는다(64 KiB 29.6/30.4,
1024 B 241/243, 64 B 261/269). **64 KiB가 떨어지지 않는지**가 이 job의 필수 확인이었고,
첫 read가 가득 찬 경우 drain을 그대로 유지하는 설계 덕에 유지된다(계측: 64 KiB read의 73 %가
full → drain 진입).

명령 수 기준 이득은 노이즈 없는 축소셀에서 확인된다: **Ir/msg -3.4 %, 메시지당 syscall 6 → 5**.

## 5. 변경 파일

- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp` — `stream_read_filled_request()`
  추가, `next_decoder_read_target()`의 두 분기를 그 함수로 합침.
- `core/src/runtime/engine/asio/asio_engine.cpp` — `maybe_drain_stream_reads(size_t)`가 방금
  끝난 read의 크기를 받아 drain 진입·반복 조건에 위 규칙을 적용.
- `core/src/runtime/engine/asio/asio_engine.hpp` — 위 시그니처.

`core/include/**`, `libzlink.vers`, 계약 테스트 기대값은 건드리지 않았다. write 경로
(`prepare_gather_output` 등, S-4 담당)는 손대지 않았다.

## 6. 테스트

- `ctest --test-dir core/build-dev -R 'stream|engine|asio|raw|zmp|large|64k|fragment|progress'`
  (55개) **5회 전부 100 % 통과**, 남은 실패 없음.
- TSan 트리(worktree에 `core/build-tsan`을 `-fsanitize=thread`로 새로 구성) STREAM/engine 7개 1회:
  **변경본과 baseline(변경을 stash한 동일 트리) 둘 다 같은 방식으로 실패**한다.
  - baseline: 7개 중 7개 실패(`test_stream_threadsafe` 포함, `test_stream_multiclient_delivery` SEGFAULT)
  - 변경본: 7개 중 6개 실패(`test_stream_threadsafe`는 통과, `test_stream_socket` SEGFAULT)
  실패 내용은 두 쪽 모두 (a) WSL2에서의 `FATAL: ThreadSanitizer: unexpected memory mapping`
  (환경 문제, 테스트 시작 0.01 s 만에 죽음)과 (b) `ypipe_t::check_read()` ↔
  `mailbox_t::reschedule_if_needed()` 사이의 **mailbox/ypipe data race**다. 둘 다 이 job이 만진
  코드(engine read 경로)와 무관하며, 변경 전후에 동일하게 나타난다. 즉 **이 변경이 새로 만든
  race는 없다**. (원자료 `S9/tsan.log`, `S9/tsan_base.log`)

## 7. 스펙 재확인

| 절 | 확인 |
|---|---|
| `systems/03-io-thread.ko.md` §3 (read 완료 → decoder → pipe → 다시 `async_read_some()`) | 서술한 순환 그대로다. 없앤 것은 그 사이의 EAGAIN 확인 read뿐이며 문서에 그 read를 규정한 문장은 없다. |
| `systems/10-hot-path.ko.md` | 수신 hot path 호출 트리(§: `pipe_t::read`~)에 변화 없음. |
| `socket/08-stream.ko.md` "현재 STREAM 런타임 기본값" — `read drain: 활성`, `max loops 64`, `max bytes 1048576` | 셋 다 그대로다(drain은 계속 활성, 상한 값 불변). 문서는 drain의 **종료 조건**을 규정하지 않는다. |
| `socket/08-stream.ko.md` "Packet 조립 구현"(도착 fragment 누적 + bounded receive queue) | PACKET 모드의 조립 규칙은 pipe의 packet state가 담당하며 **raw read 횟수와 무관**하다. 같은 바이트가 같은 순서로 같은 stage에 들어간다. |

관찰 가능한 동작(도착 순서, READY/DISCONNECTED, POLLIN/POLLOUT level, WRITABLE wake,
completion 순서)에 대한 **어느 문장도 다른 동작이 되지 않았다**. 바뀐 것은 커널에 묻는 횟수뿐이다.

## 8. 변경 분류

**B (기존 결함)** — 도착한 데이터를 다 읽었음이 이미 확정된 상황에서 커널에 한 번 더 묻던
메시지당 1회의 무익한 syscall 제거.

## 9. 멈춘 지점

- **TSan 트리가 이 환경에서 baseline부터 깨져 있다**(WSL2 memory mapping FATAL + 기존 mailbox/ypipe
  race). 변경 전후를 같은 트리에서 각각 돌려 "차이 없음"까지만 확인하고 멈췄다. mailbox race 자체는
  S-1/S-2 소관이며 이 job에서 손대지 않았다.
- `recv`를 asio 레퍼런스와 같은 1.000/msg까지 낮추려면 boost.asio `epoll_reactor`의
  arming recv(EAGAIN 1회)를 우회해야 한다 — 엔진이 소켓 readiness를 직접 관리하는 구조 변경이
  필요하므로 이 job 범위 밖으로 남긴다(후속 후보).
