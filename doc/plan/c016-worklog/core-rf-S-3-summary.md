# S-3 — decoder 수신 버퍼 재사용 확대 (결과: 전제 반증 + 실제 원인 교정)

worktree `~/project/zlink-work/s3` (detached `430abce139`). 커밋하지 않음.

## 1. 결과 요약

**S-3의 전제(“raw decoder가 메시지당 malloc/free 1쌍을 낸다”)는 측정으로 반증됐다.**
decoder spare 1칸은 이미 정상 상태에서 100 % 재사용 중이었고, S-A §3·§4#5가 decoder에
귀속한 `malloc 1.31 + free 1.47 /msg`는 전부 **boost::asio 핸들러 op 할당**이다.

| 항목 (축소 callgrind 셀, zlink 1024 B, CCU 20, 15 s) | baseline (S-A `cg_zlink.out`, 57,663 msg) | 4칸 spare 링 적용 (80,155 msg) |
|---|---|---|
| `shared_message_memory_allocator::allocate()` 호출 | 2.000 /msg | 2.000 /msg |
| **그중 malloc 으로 내려간 횟수** | **41회 총계 = 0.0007 /msg** | **41회 총계 = 0.0005 /msg** |
| 프로세스 전체 malloc / free | 1.31 / 1.47 /msg | 1.253 / 1.413 /msg |
| `operator new` ← `asio_engine_t::start_async_read()` | **1.026 /msg** | 1.020 /msg |
| `operator delete` | 1.039 /msg | 1.029 /msg |
| `aligned_alloc` ← `boost::asio::…thread_info_base::allocate` | 0.263 /msg | 0.222 /msg |
| Ir/msg | 11,096 | 10,167 |

41회는 연결 수(20 CCU + 여분)와 같다. 즉 **decoder는 연결당 첫 버퍼 한 번만 malloc하고
그 뒤로는 spare 1칸으로 완전히 재사용**한다. 링을 4칸으로 늘려도 malloc은 0.0002/msg만
줄었다. Ir/msg 차이(11,096→10,167)는 valgrind 직렬화 아래 같은 15 s에 처리한 메시지 수가
57.7 k→80.2 k로 다른 런이라 **변경에 귀속할 수 없다**(S-A §0 한계).

## 2. 실제 원인 — `handler_allocator`의 인라인 블록 1칸

`core/src/runtime/engine/asio/handler_allocator.hpp`는 1 KiB 인라인 블록 **1칸**과 `_in_use`
불리언을 둔다. 그런데 다음 read는 이전 read의 **완료 핸들러 안에서** `start_async_read()`로
재무장되고, ASIO는 op 메모리를 **핸들러가 반환된 뒤에** 해제한다. 따라서 재무장 시점에는
언제나 `_in_use == true`이고 `::operator new`로 폴백한다 — 정확히 메시지당 1회.
callgrind가 이를 그대로 보여준다: `start_async_read()` → `operator new` **1.026 /msg**.

**변경**: 블록을 2칸으로 늘려 그 겹침을 덮는다. 규칙·플래그·옵션 추가 없음(상수 1→2와
선형 스캔). 계약·회계·관측 동작 어느 것도 건드리지 않는다.

## 3. 설계 비교

| 안 | 판단 |
|---|---|
| **(A) spare 1칸 → N칸 링** (구현·측정 후 되돌림) | malloc 이득 **0.0002/msg**. 대신 연결당 보유 바이트 상한이 `1×read_buffer`(기본 8 KiB) → `4×`(32 KiB), 1,000 연결이면 8 MiB → 32 MiB. **이득 0에 비용만 4배 → 채택하지 않음** |
| **(B) `raw_decoder`의 `max_messages_`를 늘려 한 버퍼를 여러 메시지로 절단** | raw는 read 1회 = 메시지 1개라 엔진 read 경로가 버퍼 잔여부로 읽어야 성립(=S-9 파일). 게다가 느린 메시지 하나가 큰 버퍼 전체를 고정(head-of-line)해 보유 바이트 상한이 `read_target`(bench에서 rcvbuf 1 MiB)까지 커진다. **채택하지 않음** |
| **(C) 채택: `handler_allocator` 1칸 → 2칸** | 메시지당 `operator new`+`operator delete` 1쌍(전체 malloc의 ~82 %)을 제거. 보유 바이트는 연결당 고정 1 KiB → 2 KiB(가변 아님, 회계 대상 아님) |

ZMP decoder도 같은 allocator를 쓰지만(`zmp_decoder`는 1-인자 생성자 = counter 다수) decoder
쪽 malloc이 애초에 0이므로 이득 대상이 아니다. (C)는 STREAM·ZMP 공통 asio 엔진 경로다.

## 4. 변경 파일

- `core/src/runtime/engine/asio/handler_allocator.hpp` — 인라인 블록 1 → 2, `_in_use` 불리언 → 배열, 선형 스캔.
- decoder/allocator 파일은 **최종적으로 변경 없음**(측정용으로 링을 넣었다가 되돌림).
- S-4(`asio_raw_engine.cpp` 생성자·`asio_stream_fastpath_policy.hpp`)·S-9(`asio_engine.cpp` read 경로,
  policy 헤더)와 **겹치는 파일 없음** — rebase 충돌 없음. 엔진 read 경로는 건드리지 않았다.

## 5. 재확인한 스펙 절

- `systems/05-connection-memory` §3.1 — 가변 memory는 “directional queue가 보관 중인 frame의
  byte charge(payload + `sizeof(zlink_msg_t)`)”. §4 — monitor는 “allocator·kernel overhead 전체”를
  보고하지 않는다. 핸들러 op 블록도 decoder 버퍼도 이 산식에 들어가지 않는다.
- `systems/06-auto-hwm` L388 — “`sizeof(msg_t)`는 allocator 사용량을 측정한 값이 아니다”.
  L393 `provisionalCharge`는 payload buffer 할당 **전** 예약값이므로 버퍼 재사용과 무관.
- `protocol/02-raw` L93 — `raw_decoder_t`는 byte span을 `zlink_msg_t`로 만든다(버퍼 출처 무규정).

**어느 문장도 다른 동작이 되지 않았다.** 회계 값·completion·READY/DISCONNECTED·POLLIN/POLLOUT
level·WRITABLE wake의 순서와 조건은 변경 대상이 아니며 코드상 손대지 않았다.

## 6. 테스트

- dev 트리(`core/build-dev`, RelWithDebInfo/LTO OFF) 빌드 OK.
- (링 안, 이후 되돌림) `ctest -R 'decoder|raw|stream|zmp|memory|hwm'` 32 tests × 5회 전부 통과.
- (채택안) `ctest -R 'decoder|raw|stream|zmp|memory|hwm|engine|asio'` 35 tests × 8회 →
  7회 전부 통과, 1회에서 `test_stream_socket_recv_multiclient_ready_regression` 1건 실패.
  단독 10회 재실행에서 1회 실패(≈10 %) — **타이밍 flake로 보이나 baseline 실패율은 확인하지 못했다**
  (baseline 확인에는 lib 재빌드가 필요, 시간 상한 초과).
- **미실행(시간 상한)**: ASan 1회, with_stream CCU 1000 성능 셀. 채택안은 **성능 미측정**이다.

## 7. 변경 분류

**B(기존 결함)** — 완료 핸들러 안에서 재무장하는 구조 때문에 `handler_allocator`의 인라인
블록이 정상 경로에서 100 % 빗나가고 있었다. 계약 변경 없음, D 항목 없음.

## 8. 멈춘 지점 / 후속

1. `handler_allocator` 2칸 안의 **성능 측정(with_stream CCU 1000)과 ASan 1회가 남았다.**
2. 남은 수신측 alloc: `thread_info_base::allocate` → `aligned_alloc` 0.22–0.26 /msg
   (핸들러 alloc을 안 쓰는 write/그 밖의 op). 같은 기법으로 줄일 수 있는지는 S-4/S-9와 함께 볼 것.
3. **S-A §3·§4#5의 “수신 버퍼 malloc/free” 행은 오귀속이므로 정정 필요.** 후속 job 우선순위에서
   S-3(decoder)은 내려가고 그 자리는 asio 핸들러 op 할당이 가져간다.
