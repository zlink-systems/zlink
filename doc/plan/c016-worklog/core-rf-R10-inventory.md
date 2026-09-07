# Phase 3 인벤토리 — R10 (`core/src/runtime/core/`, ctx*/options*/object/own/io_*/mailbox/signaler/socket_poller/poller_base/reaper/msg/multipart_send_txn/recv_internal/send_internal/control_runtime/address/endpoint/command/i_mailbox/i_poll_events/thread/scoped_msg/recv_tls_view)

기준: HEAD 482d7bca80. 읽기 전용 조사, 빌드·수정 없음.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1 dead code | `options.hpp:237-238`, `options.cpp:119-120`, `options_core_socket.cpp:120-134,291-305`, `options_owner.cpp:29-30,96-97` | D-B85로 `PENDING_MAX_MSGS/BYTES`가 ABI-only no-op이 됨. `send_pending_max_msgs`/`send_pending_max_bytes`는 여전히 `options_t` 멤버로 저장되고 set/getsockopt 경로가 값을 왕복시키지만, 저장값을 **읽어 쓰는 곳이 get/set 자신 외에 전무**함 (`grep -rn` 전 트리, R10 밖 포함 0건). | 코드가 아니라 의도 확인 필요: (a) getsockopt ABI 안정성 위해 저장을 유지하는 것이 D-B85의 의도라면 그 사실을 주석으로 박제 — "값은 저장/왕복만 하며 강제(enforce)하지 않음"; (b) 저장 자체도 걷어내려면 getsockopt 반환값 계약이 깨지므로 별도 D 필요. | 1개 파일(주석만) 또는 0(현행 유지) | 있음→D (getsockopt 반환값이 ABI) | 없음 |
| 2 | 1 dead code | `msg.hpp:216-234`(`type_join`/`type_leave`), `msg.hpp:146-147`(`is_join`/`is_leave`), `msg.cpp:389-410`(`init_join`/`init_leave`), `msg.cpp:618-627`(`is_join`/`is_leave` 구현) | radio/dish 소켓이 저장소 전체에서 이미 제거됨(`grep -rln ZLINK_RADIO\|ZLINK_DISH core/` 0건). `init_join`/`init_leave`/`type_join`/`type_leave`는 msg.cpp 자기 자신 외 **호출자 0건**(R10 밖 포함). `is_join()`/`is_leave()`만 `pipe.cpp`(R3, 이번 모듈 제외) 여러 곳에서 검사되지만 그 값을 만드는 `init_join/init_leave`가 아무도 호출하지 않으므로 **항상 false로만 평가되는 죽은 분기**다. | msg.hpp/cpp에서 join/leave 타입·생성자·판별자 제거. `pipe.cpp`의 `is_join()/is_leave()` 검사식도 함께 정리 필요 — R3(pipe) 담당과 조율. | 2개 파일(msg.cpp/hpp, ~25행) + pipe.cpp(R3, ~8곳 조건식) | 없음(내부 enum, 공개 zlink.h 미노출 — 확인 필요: zlink.h에 ZLINK_GROUP류 공개 상수 없는지 재확인) | 이득(죽은 분기 제거, 미미) |
| 3 | 2 중복 | `ctx_socket_registry.cpp:114-142`(`wait_for_socket_removal`) / `:145-163`(`wait_for_socket_count_at_most`) | 두 함수가 "데드라인 계산 → 조건 확인 → timeout 처리 → cv.wait" 폴링 루프를 술어(predicate)만 다르게 그대로 복붙. | 공통 `wait_until (mutex_t*, predicate, timeout_ms)` 헬퍼로 추출, 두 함수는 술어만 전달. | 1개 파일, ~50행 | 없음 | 없음 |
| 4 | 3 얕은 모듈(확인 필요) | `socket_poller.cpp:244-491`(`rebuild()`, 247행) | 단일 함수가 250행 임계에 근접. 다만 대부분이 `#if ZLINK_HAVE_WINDOWS` / `#elif ZLINK_POLL_BASED_ON_POLL` / `#else`(select) 플랫폼별 3-way 분기로, 실제 한 빌드에서 살아있는 코드 경로는 훨씬 짧음(전처리기 배제). | 백엔드별 `rebuild_poll()`/`rebuild_select()`/`rebuild_windows()`로 분리하면 가독성은 좋아지나 이득이 크지 않을 수 있음 — 우선순위 낮음, 리팩터 전 실제 활성 경로 길이(플랫폼별 전처리 후) 재확인 필요. | 1개 파일, ~250행 | 없음 | 없음 |
| 5 | 5 이름-개념 불일치(확인 필요, 현재는 안전) | `ctx.hpp:179`(`recursive_mutex_t _slot_sync`), `ctx.cpp:284-291,308-315`(`wait_for_socket_removal`/`wait_for_socket_count_at_most`), `ctx_socket_registry.hpp:38-39`(`mutex_t *sync_` 매개변수) | `ctx_socket_registry_t::wait_for_socket_*`는 `mutex_t*`를 받아 `condition_variable_t::wait`(pthread_cond_wait/`condition_variable_any`)에 그대로 넘김 — 이는 락이 **정확히 1단계**로 잠겨 있음을 전제. 호출자는 `recursive_mutex_t& _slot_sync`(`mutex_t`에서 파생, `utils/mutex.hpp` 설계상 의도적 겸용)를 넘기는데, 두 진입점(`ctx.cpp:284`,`:308`) 모두 자체 `scoped_lock_t`로 갓 잠근 뒤 바로 호출하므로 현재 콜그래프 상 재진입(lock count>1) 경로는 발견되지 않음(재확인: `grep -rln wait_for_socket_removal\|wait_for_socket_count_at_most core/` → `ctx.cpp` 외 호출자 없음). glibc pthread 구현은 recursive mutex의 재진입 카운트를 cond_wait 시 보존/복원하지만 이는 POSIX 보장이 아닌 구현 세부사항. | 현재 수정 불필요 — 다만 `_slot_sync`를 재귀적으로 미리 잠근 상태에서 이 두 함수에 진입하는 경로가 생기지 않도록 주석/어서션으로 계약을 명시 제안. | 주석 추가 시 1개 파일, ~4행 | 없음 | 없음 |

## 적용 job 묶음 제안

- **묶음 A** (msg 정리, ~1 h): 항목 2 — `msg.cpp`/`msg.hpp`에서 join/leave 흔적 제거. `pipe.cpp` 쪽 조건식 정리는 R3(pipe) 담당과 사전 조율 후 같은 job에서 함께 처리하거나 별도 후속 커밋으로 순서 조정.
- **묶음 B** (ctx_socket_registry 중복 제거 + 문서화, ~0.5 h): 항목 3(폴링 루프 통합) + 항목 5(주석으로 락 계약 명시) — 같은 파일(`ctx_socket_registry.cpp`/`.hpp`, `ctx.hpp`)이라 함께 처리해도 반경이 작음. 순서: 3 → 5.
- **묶음 C** (문서화 전용, ~0.3 h, 선택): 항목 1 — `options.hpp`/`options_core_socket.cpp`에 PENDING_MAX 저장이 D-B85 이후 왕복 전용(no-op)임을 주석으로 명시. 코드 변경 없음, 감독관 확인 후 진행.
- 항목 4는 우선순위 낮음 — 별도 job으로 묶지 않고 보류 권장(리팩터 이득 대비 위험 낮지 않음, "확인 필요"만 기록).

보고서 경로: `doc/plan/c016-worklog/core-rf-R10-inventory.md`
