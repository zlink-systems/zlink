# R2 — engine 죽은 분기 · protocol 죽은 코드 · restart_input 분할 (Phase 3 apply)

> 2026-09-07. worktree `~/project/zlink-work/r2` (detached `d424f11453` = main). **커밋하지 않음.**
> 원자료: `<scratchpad>/R2/` (`cg_run.out`, `cg_idle.out`, `cg_run_server.log`), `<scratchpad>/r2-ctest-{1..5}.log`.
> 브리프 지시대로 묶음 (4) handler-allocator-2slot 은 **제외**(D-B152, S-3 효과 0).

## 1. 결과 (수치)

| 지표 | 기준 | after | 판정 |
|---|---|---|---|
| 삭제/추가 줄 | — | **−421 / +147** (9 파일) | 순 −274행 |
| `restart_input_internal()` 길이 | 279행 | **48행** (+ 헬퍼 3개, 최대 60행) | 250행 이하 충족 |
| callgrind 축소셀 Ir/msg (S-A §0, CCU 20 / 1024 B / 15 s) | **9,474** (S-11) | **9,445** | 악화 없음(−0.3 %) |
| `recv` 호출/msg | 2.0 (S-9) | **2.000** | S-9 규칙 유지 |
| `send` 호출/msg | 1.0 | 1.000 | 동일 |
| ctest `engine\|asio\|raw\|zmp\|stream\|tls\|ws` | — | **35/35 통과 ×5회** | 실패 0 |

Ir/msg 식은 S-1/S-11과 동일: `(server I refs 934,125,176 − idle 3,852,368) / recv_msgs 98,494`.
idle 은 이 worktree lib 로 새로 측정했다(3,852,368; S-1 의 3,453,365 과의 차이는 msg 당 0.16 Ir 로 무시 가능).
측정 시 load avg 0.03, PERF_LOCK 하에 단독 실행, 동시 빌드 없음.

## 2. 변경 파일

| 파일 | 내용 |
|---|---|
| `asio_engine.cpp` | 항목 2·3·4 죽은 분기 삭제, `restart_input_internal()` 4함수 분할, 드레인 루프 1개로 통합 |
| `asio_engine.hpp` | `drain_result_t` + 헬퍼 3개 선언, `use_stream_rx_slab()` 삭제 |
| `asio_engine_pipeline.hpp` | `asio_stream_rx_chunk_t`→`asio_rx_chunk_t`, `pending_buffers`/`pending_buffer_pool` 삭제(큐 2쌍 → 1쌍) |
| `asio_stream_fastpath_policy.hpp` | `enable_rx_slab()` 삭제(항상 true 인 게이트) |
| `asio_error_handler.hpp` | **파일 삭제**(항목 6, 103행) |
| `ssl_transport.cpp` / `ssl_context_helper.cpp` | 위 include 제거 |
| `decoder_allocators.hpp` / `decoder.hpp` | `c_single_allocator` 삭제, `decoder_base_t` 의 기본 템플릿 인자 제거(항목 7) |

## 3. 죽은 코드 재확인 (삭제 근거)

- **항목 3·4**: `prepare_gather_output()` 은 `gather_write_enabled()` 게이트 뒤에서만 불린다(683·1350행).
  `use_gather_write_for()` 는 `protocol_builds_gather_header_` 가 거짓이면 무조건 false 이고, 이 값을 true 로
  넘기는 것은 `asio_zmp_engine_t` 뿐이다. 그리고 6개 connecter/listener 전부 `options.type == STREAM` 이면
  `asio_raw_engine_t`(false), 아니면 ZMP 를 만든다. 따라서 이 함수 안 `_options.type == STREAM` 은 항상 거짓 —
  `stream_mode`·`tiny_stream_gather`·STREAM threshold·`use_stream_handler_alloc` 네 갈래 모두 도달 불가.
- **항목 6**: `asio_error::` 참조가 트리 전체에서 0(정의 파일 제외). 두 include 는 심볼을 하나도 쓰지 않음.
- **항목 7**: `decoder_base_t<...>` 인스턴스화는 `zmp_decoder` 의 2인자 형태 하나뿐 — 기본 인자가 선택된 적 없음.

## 4. 설계 비교와 선택 — 드레인 루프

**(A) 두 큐를 유지하고 루프를 소스로 파라미터화**(브리프의 첫 안: 템플릿/드레인 객체로 `pending_buffers` 와
`pending_stream_rx_chunks` 를 공통 뷰로 감싼다). 규칙 수는 줄지 않는다: 큐 2개, 풀 2개, `use_stream_rx_slab()`
게이트 1개, "소비된 바이트를 어떻게 버리는가"(vector `erase` vs `offset`) 2가지가 그대로 남고 어댑터 계층이 **추가**된다.

**(B) 채택 — 큐를 하나로 만들어 파라미터 자체를 없앤다.** 두 루프가 다른 진짜 이유는 저장 형식뿐이었다:
STREAM 은 `{data, offset}` 청크, 나머지는 `std::vector` + `erase` 로 앞을 잘라내는 것. `asio_rx_chunk_t` 는
소켓 타입과 무관하게 성립하므로 `pending_buffers`/`pending_buffer_pool` 을 삭제하고 모든 타입이
`pending_rx_chunks` 를 쓰게 했다. 그 결과 **`use_stream_rx_slab()` 술어와 `enable_rx_slab()` 게이트가 사라지고**,
드레인은 루프 하나 + 회계 규칙 하나("디코더가 먹은 바이트만큼 offset 을 밀고 `total_pending_bytes` 에서 뺀다")로
줄었다. POSDDD 기준(규칙 수 감소·중복 금지)에서 (A) 는 규칙을 늘리고 (B) 는 3개를 없앤다.

분할은 `classify_drain_stop(rc)` 하나를 중심으로 했다. 원본은 "EAGAIN → 정지 / io_error → connection_error /
그 외 -1 → protocol_error" 3분기를 **세 번** 복사해 갖고 있었고, 이제 한 곳에만 있다. 나머지는 그 규칙을 쓰는
세 단계(`retry_stopped_message` → `drain_current_input` → `drain_pending_chunks`)다.

**S-9 유지 확인**: 이 job 은 `stream_read_filled_request` 와 read drain(`maybe_drain_stream_reads`,
`speculative_read`, `start_async_read`)을 건드리지 않았다. 실측으로도 `recv` 2.000/msg 가 그대로다.

## 5. 실행한 테스트

- `ctest --test-dir core/build-dev -R 'engine|asio|raw|zmp|stream|tls|ws'` **5회, 35/35 통과, 실패 0**.
- dev 빌드(JOBS=4) 경고 0(새 경고 없음; libstdc++ `atomic_base.h` 경고는 기존 것).
- callgrind 축소 셀 1회(§1). TSan 은 pipe/mailbox/mutex 를 만지지 않아 생략.

## 6. 재확인한 스펙 절

- `03-io-thread.ko.md` §4(턴당 준비 버퍼 1개, Proactor 재무장): 쓰기 턴의 분기 수만 줄었고 `async_writev`
  호출 자체·완료 핸들러 재무장은 그대로다.
- `08-stream.ko.md` §395-401(STREAM speculative write 예산), `02-raw`(raw 는 헤더를 만들지 않는다):
  삭제한 STREAM gather 분기는 애초에 raw 엔진에서 도달할 수 없었으므로 이 문장들의 동작은 불변이다.
- backpressure 계약(`stop_input_for_current_backpressure` → `session->flush()` → 정지 유지): 세 지점 모두
  같은 순서·같은 조건으로 `classify_drain_stop()` 에 모였다.
- **어느 문장도 다른 동작이 되지 않았다** — 단 §7 의 두 항목은 예외로 명시한다.

## 7. 동작이 달라진 두 지점 (분류 B)

1. **비-STREAM 드레인의 EAGAIN 정산**. 기존 `pending_buffers` 루프는 backpressure 시
   `if (buffer_remaining > 0 && buffer_pos > 0)` 일 때만 앞부분을 잘라냈다. 버퍼 마지막 메시지에서 EAGAIN 이 나면
   `buffer_remaining == 0` 이라 **소비한 바이트가 버퍼에 그대로 남아 다음 restart 때 다시 디코딩**된다(중복 전달).
   slab 루프에는 이 조건이 없었다(`chunk_pos > 0` 만). 통합하면서 slab 쪽 규칙을 택했으므로 이 결함이 사라진다.
2. **`retry_stopped_message()` 의 `io_error` 검사**. 원본의 첫 블록만 `io_error` 를 보지 않고 바로
   `protocol_error` 를 냈다. 통합 규칙에서는 세 지점 모두 `io_error` 를 먼저 본다. `_pipeline.io_error` 가 참이면
   이미 `error()` 가 나간 뒤라 실제로 동시 성립하지 않는 조합이고, 규칙을 하나로 두기 위해 통일했다.

## 8. 변경 분류

**B(기존 결함) + 1(죽은 코드 삭제)** — 새 옵션·플래그·상태 추가 0, 공개 헤더·`libzlink.vers` 무변경.

## 9. 멈춘 지점 / D 항목

- **D**: `ZLINK_ASIO_STREAM_GATHER_THRESHOLD` / `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD`.
  두 값을 읽던 유일한 코드가 죽은 분기였으므로 `asio_engine.cpp` 의 상수 2개는 삭제했고, 이제 **두 env 는
  어떤 동작에도 영향이 없다**. `asio_stream_fastpath_policy.hpp` 의 접근자 2개는 남겼다 —
  삭제하려면 `08-stream.ko.md:403-404` / `.en.md:430-431` 의 문장을 지워야 하고, 그건 스펙 변경이라
  공통 규칙에 따라 여기서 멈추고 감독관 판단으로 남긴다. (같은 이유로 `use_gather_write_for()` 안
  `socket_type_ == STREAM && stream_gather_enabled()` 절과 `ZLINK_ASIO_STREAM_DISABLE_GATHER` 도 도달 불가다.)
- 인벤토리 항목 5·8·10 은 브리프 범위 밖이라 손대지 않았다.
