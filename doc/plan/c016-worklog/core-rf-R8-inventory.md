# Phase 3 인벤토리 — R8 (dealer / router / pubsub / pair / proxy / internal)

기준: doc/principal/dev/posddd.ko.md, core/doc/spec/core/systems/08-posd-module-structure.ko.md, 07-core-source-layout.ko.md. 참고 실측: S-A, S-B, S-5. Repo HEAD 3586f0eb17. 읽기 전용 조사, 빌드/수정 없음.

범위: `core/src/runtime/sockets/{dealer,router,pubsub,pair,proxy,internal}` (common/, stream/ 제외). 대상 6,946행, 22개 파일.

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 2(중복) | `internal/dist.cpp:43` `dist_t::has_pipe` | `array_t::contains()`(`utils/array.hpp:74`)와 동일한 멤버십 판정을 손으로 재구현(`_pipes.index()` + 범위 비교). `lb_t::pipe_terminated`는 이미 `_pipes.contains()`를 직접 쓴다(`internal/lb.cpp:68`) — 같은 클래스 계열에서 관용구가 갈라짐. 두 호출부(`xpub.cpp:447`, `dist.cpp:105`) 모두 non-null 보장. | `has_pipe`의 본문을 `return _pipes.contains (pipe_);`로 교체(시그니처 유지, 헤더 불변) | 파일 1개, ~8행 | 없음 | 없음(동일 로직, 인라인 가능성 ↑) |
| 2 | 확인 필요(2 중복 후보) | `internal/fq.cpp:56` `try_get_pipe_index` vs `array_t::contains`/`dist_t::has_pipe` | fq_t는 index-out 파라미터가 필요해 `array_t::contains`를 그대로 못 쓰고 별도 헬퍼를 유지 — 이건 정당한 차이일 수 있음(단순 bool 아님). dist/lb/fq 세 컨테이너가 f57c73b040 이후 "attach된 것만 terminate" 불변식을 각자 다른 표현(has_pipe / _pipes.contains / try_get_pipe_index)으로 구현. 통합 헬퍼(예: array_t에 index-out 오버로드 추가)는 이득이 작고 3개 클래스 동시 변경이라 반경이 커짐. | 항목 1만 적용하고 이 항목은 보류 권장. 확인 방법: `try_get_pipe_index`를 `contains`+`index()` 조합으로 대체 가능한지, 오버헤드/가독성 비교 후 별도 job에서 재검토 | 파일 3개(fq/dist/lb), ~30행 | 없음 | 없음 |
| 3 | D(계약 영향, 이미 R3에서 발견) | `router/router_recv_path.cpp:36-72` `router_t::copy_router_pipe_source_rid` | standby RID 조회가 `_out_pipes_sync` 락 하에 `_standby_pipes` map을 매 수신마다 조회(콜드 핸드오버 폴백). R3가 지목한 route-binding-cache BLOCKER와 동일 — pipe-owned snapshot(`try_copy_router_route_binding`)이 실패했을 때만 타는 폴백이라 상시 hot-path는 아니지만, 잠금 하에 map 조회를 유지하는 구조 자체가 캐시/락 재설계 대상. 이번 조사에서 재확인만 하고 손대지 않음. | (설계 결정 필요 — 이번 job 범위 아님, D로 표시) | — | 있음 → D | 위험(폴백 경로 빈도·락 경합에 따라 다름) |
| 4 | 3(깊은 모듈, ≥250행 함수) | `router/router_send_path.cpp:237-590`(약 354행) `router_t::xsend_routed` | 진입 검증, standby 배수, 파이프 선택, 쓰기 재시도, 디버그 로깅(`router_debug::enabled()` 분기 다수)이 한 함수에 있음. `router_debug` fprintf 블록만 추려도 6곳. | 디버그 트레이스 블록을 별도 헬퍼로 뽑거나 `#ifdef`/함수 추출로 200행 이하로 축소 검토 | 파일 1개(+router_debug.hpp 참조), ~350행 재배치 | 없음(리팩터링만, 동작 불변 조건) | 없음(동일 코드, 순서 보존 시) |
| 5 | 3(깊은 모듈, ≥250행 함수) | `internal/lb.cpp` `lb_t::sendpipe`(약 265행) | 가중치 선택 + 멀티파트 상태 + admission 콜백이 한 함수에 혼재. R8 파일 중 lb.cpp(766행)가 가장 크고 이 함수가 그 1/3을 차지. | 가중치 선택과 admission/observer 처리를 분리하는 리팩터링 후보로 등록(이번 job에서는 관찰만) | 파일 1개, ~270행 | 없음(예상) | 위험(핫패스 함수 — S-5 layering 비용과 겹치므로 실측 job과 조율 필요) |
| 6 | 확인 필요(1 dead-code 후보) | `dealer/dealer.cpp:168-169` vs `stream/stream.cpp:893-894` | `xsend_routed`의 `expected_route_incarnation_id_`/`request_only_` 두 파라미터가 dealer_t와 stream_t 양쪽에서 `LIBZLINK_UNUSED`. S-B는 이를 "stream만의 특이점"으로 기록했으나, dealer_t도 동일하게 미사용 — router_t만 두 값을 실제로 씀(`router_send_path.cpp:295,318,353,440,444`). 즉 이 시그니처는 socket_base_t의 공용 가상 인터페이스이고, ROUTER 계약(요청 슬롯/라우트 세대 검증)에만 의미가 있는 파라미터를 DEALER·STREAM이 함께 흡수하는 구조 — 개별 소켓의 dead param이 아니라 인터페이스 설계상 계약 비대칭. | 코드 변경 제안 없음(인터페이스 통합 설계 이슈, apply job 대상 아님) — S-B 보고서에 "stream만"이 아니라 "dealer도 동일"로 정정 기록 권장 | — | 있음(가상 인터페이스 변경 시) → 이번엔 변경 안 함 | 없음 |

## 요약 개수
- dead code: 0건 확정(1건은 인터페이스 설계 이슈로 재분류, 항목 6)
- 중복: 1건 확정 적용 가능(항목 1), 1건 보류(항목 2)
- 얕은 모듈(≥250행 함수): 2건(항목 4, 5) — 둘 다 router_send_path.cpp / lb.cpp, R8에서 가장 큰 두 파일과 일치
- 잘못된 소유: 0건 발견(헤더 노출은 socket_base.hpp가 이미 R-계열 공통 범위, 본 R8 개별 파일에서는 신규 항목 없음)
- 이름-개념 불일치: 0건 발견
- D(계약 영향, 재확인만): 1건(항목 3, R3와 동일 블로커)

## 적용 job 묶음 제안
1. **묶음 A** (안전, ~10분): 항목 1 — `internal/dist.cpp`의 `has_pipe`를 `_pipes.contains()` 호출로 교체. 단독 파일, 순수 리팩터링, 회귀 위험 최소.
2. **묶음 B** (선택, ~1.5h 이내 가능하나 성능 job과 충돌 가능성 있어 조율 필요): 항목 4 — `router_send_path.cpp::xsend_routed`의 디버그 트레이스 블록 추출. 락/핫패스 코드는 그대로 두고 `router_debug::enabled()` 분기만 헬퍼로 이동.
3. **묶음 C** (별도 조율, 성능 실측과 겹침 — S-5/S-B 담당 job과 순서 조정 필요): 항목 5 — `lb.cpp::sendpipe` 분리는 핫패스이므로 측정 job이 끝난 뒤 단독으로 진행 권장, 이번 라운드에는 넣지 않음.
- 항목 2, 3, 6은 코드 변경 없음(보류/D/설계 이슈 기록만).

보고서 경로: `doc/plan/c016-worklog/core-rf-R8-inventory.md`
