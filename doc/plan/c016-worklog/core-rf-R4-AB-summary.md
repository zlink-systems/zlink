# R4-AB — Phase 3 적용 (묶음 A 기계적 정리 + 묶음 B S-13 send 프레임)

> 2026-09-07, worktree `~/project/zlink-work/r4` (detached `ca563e65f2`). **커밋하지 않음.**
> diff 9파일 +63/−133 (파일 2개 삭제). 공개 헤더·`libzlink.vers`·계약 테스트 미변경.
> 원본 데이터: `<scratchpad>/R4/` (`cg_full.out`, `cg_idle.out`, `full_server.log`, `idle_server.log`, `cg_cell.sh`).

## 1. 결과 요약

| 항목 | 값 |
|---|---|
| ctest `send\|submit\|dontwait\|writable\|backpressure\|monitor\|close\|stream\|router\|dealer\|pair` | 71/71 통과 × **5회** (실패 0) |
| dev 빌드(JOBS=4) | 경고·에러 없음 |
| 축소 callgrind STREAM 셀 Ir/msg | **9,545** (full 902,646,189 − idle 3,786,587) / 94,167 msg |
| 기준 | S-5 before 10,117 → **−5.7 %**. 목표선 9,474 대비 **+0.75 %(미달)** |
| 순수 계층 비용 합계 | **≈532 Ir/msg** (S-5 537) |

## 2. 변경 내용

### 묶음 A (기계적)
1. `routed_submit_target.hpp` — 죽은 `typedef uint64_t zlink_send_op_id_t;` 삭제(참조 0 재확인).
2. `socket_close_ops.{hpp,cpp}` 삭제 + `core/CMakeLists.txt` 항목 삭제. 유일 호출부 `monitor_api.cpp:200`을 `stop(); close(0);` 3줄로 인라인. 클래스가 하던 NULL 검사는 호출부의 기존 `if (raw_source_monitor_socket)` 와 중복이었고, 포인터 NULL화는 함수 끝 지역 변수라 관측 불가 — 동작 동일.
3. `socket_base_monitor.cpp` — 스칼라 1개짜리 모니터 이벤트 10개(`event_connect_delayed`/`event_connect_retried`/`event_listening`/`event_bind_failed`/`event_accepted`/`event_accept_failed`/`event_closed`/`event_close_failed`/`event_handshake_failed_no_detail`/`event_handshake_failed_protocol`)를 private `event_scalar (uri, type, value)` 하나로 병합. 각 함수는 1줄. 페이로드는 이전과 **바이트 단위로 동일**(values[1], routing_id NULL/0, internal_flags 0, lane=application, pair id/gen 0 — 전부 `event()`의 기존 기본값). 이름 있는 10개 진입점은 외부 호출자(transports/ws·tls·tcp·ipc, session_base, asio 엔진, unittest)가 많아 그대로 둔다.

### 묶음 B (S-13)
4. **`record_context_admission_` 제거는 하지 않았다 — 인벤토리 #3(a)는 오탐이다.** 인벤토리는 이름 grep만 했으나, 이 인자는 **위치 인자로 false가 넘어온다**: `socket_send_submit.cpp:409`의 `try_admit_send_parts_scoped(...)` 14번째 인자가 `!logical_wait_registered`. 즉 blocking wait가 이미 등록된 뒤의 wake 재시도에서는 false다("등록된 wait가 이 제출의 첫 admission을 이미 소유한다 — wake가 metric attempt를 하나 더 더하면 안 된다"). 제거하면 `_auto_hwm_send_attempts`/`_auto_hwm_send_blocked_attempts`가 wake마다 중복 증가하여 auto-HWM 정책(S-A)의 입력이 달라진다 → **동작 변경**이므로 유지하고, 헤더에 그 이유를 주석으로 남겼다.
5. **죽은 인자 3종 제거**(`send_direct_with_retry` 4개 호출부 전수 확인 결과 모두 상수): `connection_id_out_`(항상 NULL), `expected_connection_id_`(항상 0), `attempt_identity_out_`(항상 NULL). 함수 본문의 `if (connection_id_out_) *connection_id_out_ = 0;` 와 `if (attempt_identity_out_) attempt_identity_out_->reset ();` 도 삭제, 하위 `xsend_routed` 3개 호출부에는 `NULL, 0, ... NULL` 을 그대로 전달(=이전과 동일한 값). 18 → 15 인자.
6. **`send_routed_scoped` 15 → 5 인자.** 두 호출자(`send_routed`, `send_routed_complete_record`)가 나머지 10개를 전부 기본값으로만 넘기고 있었다. 이제 `(target_rid_, msg_, flags_, scope, manage_public_send_recovery_)` 만 받고 나머지는 `send_direct_with_retry` 호출에서 리터럴로 고정된다.

### 각 프레임이 소유한 규칙 (헤더에 명시)
| 프레임 | 소유 규칙(정확히 하나) |
|---|---|
| `send()` / `send_routed()` | 일반 public send scope 생성 |
| `send_complete_record()` / `send_routed_complete_record()` | complete-record admission scope 생성 |
| `send_scoped()` | routed target 없는 진입(target=NULL) |
| `send_routed_scoped()` | routed 진입은 routing id를 반드시 갖는다(`!target_rid_` → EFAULT) |
| `send_direct_with_retry()` | 물리 admission 1회 + submit-retry/blocking 정책 |

admission 규칙은 어느 것도 두 프레임에 중복되지 않는다. `_ctx_terminated`→ETERM, `msg_->check()`→EFAULT, EMSGSIZE, `process_commands`, auto-HWM 계수는 모두 `send_direct_with_retry` 한 곳에만 있다.

### 각 경계의 errno/result 매핑 — 전부 동일
| 경계 | 매핑 | 변경 |
|---|---|---|
| `send_routed_scoped` 진입 | `!target_rid_` → `errno=EFAULT`, ret −1 | 동일(코드 그대로 이동 없음) |
| `send_direct_with_retry` 진입 | `_ctx_terminated`→ETERM / `!msg_ \|\| !check()`→EFAULT / 비STREAM size>UINT32_MAX→EMSGSIZE / `process_commands!=0`→−1 | 동일 |
| `xsend_routed`/`xsend_pipe` 반환 | rc 0 성공 / rc −2 → `finish_multipart_abort()` / rc −1 → errno 보존 | 동일 |
| request-full | `errno=EAGAIN`, ret −1 (플래그 무관) | 동일 |
| staged-pair-intent 재분류 | `errno != EAGAIN && is_submit_retry_errno` + DEALER/ROUTER → `errno=EAGAIN` | 동일 |
| manual-connect 대기 재분류 | `errno=EAGAIN` | 동일 |
| submit-retry 루프 종료 | `errno=last_errno` | 동일 |
| 최종 비-EAGAIN | `errno=failure_errno`, ret −1 (+recovery arm/clear 분기) | 동일 |
| DONTWAIT/sndtimeo==0 EAGAIN | `arm_send_recovery_after_backpressure()`, ret −1 | 동일 |
| blocking 타임아웃 | `errno=EAGAIN`, ret −1 | 동일 |
| `try_admit_send_parts_scoped` 진입 | `!parts_ \|\| count==0 \|\| !scope.acquired()` → EFAULT | 동일 |

삭제한 3개 인자는 어느 매핑에도 참여하지 않았다(출력 포인터 초기화뿐).

## 3. 설계 비교와 선택 이유

- **(A3) 모니터 이벤트**: (i) 10개 함수를 지우고 호출자가 `event()`를 직접 호출 vs (ii) 파라미터화 헬퍼 1개 + 1줄 래퍼 유지. (i)은 외부 호출자 30여 곳에 `NULL, 0, values, 1` 마샬링을 복제한다(중복 금지 위반). (ii) 선택.
- **(B) 프레임 병합**: (i) `send_scoped`/`send_routed_scoped`를 삭제하고 4개 호출부가 `send_direct_with_retry`를 직접 호출 vs (ii) 프레임은 남기되 **통과만 하던 인자를 전부 제거**해 각 프레임이 자기가 판단하는 값만 받게 한다. (i)은 프레임 2개를 없애지만 각 호출부에 기본값 상수 10개씩을 복제한다 — 규칙 수는 그대로인데 중복만 늘어난다. POSDDD(중복 금지 우선)에 따라 (ii) 선택. 결과적으로 "각 프레임이 정확히 하나의 규칙을 소유"라는 목표는 (ii)로 달성되고, 인자 마샬링(=S-5가 지목한 계층 비용의 본체)은 실제로 줄었다.

## 4. 성능 표 (축소 callgrind 셀, 1024 B, CCU 20, 15 s, `--cache-sim=no`, `flock PERF_LOCK`, load 0.65)

| 프레임 | self Ir/msg (R4) | S-5 before | 차 |
|---|---|---|---|
| `wait_for_completion_submit_admission` | 144.0 | 144 | 0 |
| `try_admit_send_parts_scoped` | 127.0 | 127 | 0 |
| `send_direct_with_retry` | 113.0 | 113 | 0 |
| `enter_public_send` | 48.0 | 56 | −8 |
| `leave_public_send` | 40.0 | 47 | −7 |
| `~socket_public_send_scope_t` | 21.0 | 21 | 0 |
| `process_submit_commands` | 16.2 | 16 | +0.2 |
| `msg_t::check` | 19.2 | 9 | +10 |
| `clear_send_recovery_pending` | 4.0 | 4 | 0 |
| **합계** | **≈532** | **537** | **−5** |
| **전체 Ir/msg** | **9,545** | 10,117 | −5.7 % |

**해석(정직하게)**: 죽은 인자 제거는 **살아남은 프레임의 self 비용을 줄이지 못했다** — `send_direct_with_retry` 113, `try_admit_send_parts_scoped` 127, `wait_for_completion_submit_admission` 144로 S-5 표와 자릿수까지 같다. LTO가 이미 NULL/0 상수 인자를 상수 전파(호출 심볼도 `.constprop.1`)로 없애고 있었다는 뜻이고, 이는 S-5 §3("LTO가 이미 최적 배치를 고르고 있다")과 일치한다. 전체 9,545가 S-5의 10,117보다 낮은 것은 이 diff의 효과라고 주장할 수 없다: S-5는 동일 코드 3회 런에서 10,101/10,117/10,216(±1 %)을 관측했고, 그 사이 G-0b idle 재측정으로 기준선 자체가 갱신되었다. **목표 9,474는 달성하지 못했고(+0.75 %), "계층 제거가 수치로 드러난다"는 가설은 이 셀에서 확인되지 않는다.**

## 5. 재확인한 스펙 절 — 어느 문장도 다른 동작이 되지 않았다

- **08-stream.en.md 122–135**: `NONE FINAL`은 SNDTIMEO 스냅샷 후 같은 RID의 로컬 큐 admission·재연결을 기다리고 ID 0·completion 없음; `DONTWAIT FINAL`은 admission 시도 정확히 1회, 즉시 admit이면 ID 0·completion 없음, HWM/바이트 크레딧으로 막히면 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`+그 RID에 묶인 nonzero wait token, 이후 쓰기 크레딧이 생기면 그 토큰에 대해 `ZLINK_COMPLETION_WRITABLE` 레코드 **정확히 1개**.
- **README.en.md "Part send and completion" 1527–1535**: 위와 동일한 문장.
- **왜 불변인가**: (i) 토큰 생성 지점 `register_send_writable_wait_after_failure`(`socket_message_send_api.cpp`)와 등록 시점 불변식("거절을 관측한 그 lifecycle sync 안에서 등록", `wait_for_completion_submit_admission`의 `logical_wait.acquire()` 위치)을 이 job은 **한 줄도 건드리지 않았다**. (ii) 삭제한 인자 3종은 출력 포인터·기대 connection id로, admission 성패나 errno에 관여하지 않는다. (iii) `record_context_admission_`를 유지했으므로 wake 재시도의 auto-HWM 계수도 이전과 같다. (iv) 모니터 이벤트는 페이로드 필드가 동일하고 발행 순서·조건이 그대로다. (v) POLLIN/POLLOUT level, READY/DISCONNECTED, completion 순서 코드는 미변경.

## 6. 실행한 테스트

- dev 빌드(JOBS=4) 1회 — 경고·에러 없음.
- `ctest --test-dir core/build-dev -R 'send|submit|dontwait|writable|backpressure|monitor|close|stream|router|dealer|pair' -j4` **5회 전부 71/71 통과**. 남은 실패 없음.
- pipe/engine/mailbox/mutex는 만지지 않았으므로 TSan 트리 미실행.
- Release `--lib-only` 빌드 1회 + with_stream 벤치 바이너리 빌드(측정용) — 성공.

## 7. 변경 분류

- 묶음 A: **B(기존 결함 — 죽은 코드/중복)**.
- 묶음 B의 인자 제거: **B(죽은 인자)**.
- 묶음 B의 `record_context_admission_`: **D(spec/인벤토리 gap)** — 인벤토리 #3(a)의 "죽은 매개변수" 판정이 틀렸다. 위치 인자 전달을 grep이 놓쳤다. 다른 인벤토리 항목의 "호출부 전수 확인"도 같은 방식이면 재검증이 필요하다.

## 8. 멈춘 지점

- S-13 본제안(#3(b), 가운데 3개 프레임 실제 병합)은 **착수하지 않았다**. 근거: 위 측정에서 프레임 self 비용이 S-5와 동일하게 나와, 병합의 기대 이득(≈537 Ir/msg의 일부)이 이미 LTO에 흡수되어 있음을 시사한다. 반면 병합은 "거절을 관측한 lifecycle sync 안에서 wait 등록" 불변식을 한 함수 안으로 옮기는 작업이라 계약 위험이 집중된다. **이득이 측정으로 뒷받침되기 전에는 하지 않는 것이 맞다** — 이는 S-5가 (b') 항목에서 내린 판단과 같다.
- 인벤토리 #7(errno 판정 대조)은 시간 상한과 D 후보(동작 변경 가능) 성격 때문에 손대지 않았다.
