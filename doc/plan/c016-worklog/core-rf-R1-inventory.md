# R1 인벤토리 — `core/src/runtime/sockets/stream/`

입력: stream.cpp(1298행)/stream.hpp/stream_batch_policy.hpp/stream_dispatch_lifecycle.cpp(15행) + pipe_stream_packet_state.hpp(STREAM 전용 부분) + socket_base_dispatch.cpp/socket_base.hpp 중 stream.cpp가 오버라이드·호출하는 훅.
기준 HEAD: 5a5b111139(브리프 커밋). 지시된 2529709db6은 그 2커밋 전(문서 전용 커밋만 차이) — 코드는 동일, 재검증 불필요.
방법: 전량 정독 + `grep -rn` 참조 카운트. 빌드/실행 없음.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1 dead | stream.cpp:88-127 (packet_record_t 이동 생성자/대입) | `_packet_receive_queue`는 `std::deque`이고 실사용은 `emplace_back()`(기본 생성) + `front/back/pop_front/clear`뿐. deque는 끝에서의 push/pop 시 기존 원소를 이동시키지 않는다는 표준 보장이 있어, 손수 만든 move ctor/operator=(각 4회 msg init/close+2회 move)는 호출되지 않는 것으로 보임(참조 grep: 정의부 외 호출 없음) | 삭제하고 `= delete` 또는 컴파일러 기본값 확인 후 제거. 확신 낮음 → **확인 필요**: 실제로 `-Wunused` 또는 컴파일러가 이 함수들을 인스턴스화하는지 바이너리 심벌로 확인, 혹은 잠깐 `= delete`로 바꿔 빌드해서 에러 위치 확인(측정 job 종료 후) | 파일 1, 행 ~50 감소 | 없음(내부 구현) | 이득(불필요 msg init/close 경로 제거 자체는 no-op이지만 코드 크기·이해 비용 감소) |
| 2 | 1 dead | stream.cpp:13-17 (`#include <chrono>`, `<new>`, `<thread>`) | 세 헤더 모두 파일 내 실사용 없음(`std::chrono`, `std::this_thread`, placement `new` grep 0건) | 세 include 제거 | 파일 1, 행 3 | 없음 | 없음 |
| 3 | 1 dead(정도 약함) | stream.hpp:120, stream.cpp:1275-1292 (`maybe_emit_connect_event`의 `routing_id_value_` 기본인자) | 호출부가 stream.cpp:231 단 한 곳, 항상 인자 생략(기본값 0) → "호출자가 넘긴 값을 우선 쓰는" 분기(1279-1280행)가 항상 미사용 경로 | 매개변수 제거하고 함수 본문에서 `pipe_->get_server_socket_routing_id()`만 사용하도록 단순화 | 파일 2(hpp/cpp), 행 ~6 | 없음 | 없음 |
| 4 | 1 dead(무해한 게이트) | stream_dispatch_lifecycle.cpp:7-15 | 브리프가 "항상 0 반환"이라 했으나 실제로는 `stream_recv_mode != RAW`일 때 -1/ENOTSUP도 반환하는 모드 게이트. 죽은 코드는 아님(socket_message_api.cpp:178에서 실사용) | 항목 아님(브리프 리드 기각). 다만 아래 #12(파일 배치)로 재분류 | — | — | — |
| 5 | 2 dup | stream.cpp:295-299, 798-802, 843-846, 948-951, 1006-1009, 1108-1111, 1131-1134, 1149-1152 (총 8곳) | `route_shard_t &shard = route_shard_for(id); scoped_lock_t shard_lock(shard.sync); shard.routes.find(id)` 패턴이 8개 함수에 그대로 복제됨 | `pipe_t *find_route_locked(uint32_t routing_id_, route_shard_t **shard_out_ = NULL)` 같은 private 헬퍼로 통합(락은 호출자가 이미 원하는 시점까지 들고 있어야 하는 곳도 있어 시그니처 설계 주의) | 파일 1, 행 ~40 순감소 | 없음(동작 보존 리팩터) | 없음(락 스코프 동일 유지 시) |
| 6 | 2 dup | stream.cpp:456-505 (fast path) vs 508-605 (state machine path) | prefix+header+body가 한 버퍼에 다 있을 때의 "빠른 경로"가 상태머신 경로(508행부터)와 같은 헤더/바디 파싱·enqueue 로직을 별도로 다시 구현 | `decode_packet_bytes` 분해와 함께(#7) 리팩터링. 이득이 크지만 핫패스라 성능 회귀 위험 — **확인 필요**: S-B가 측정한 64B급 트래픽 벤치로 전/후 비교 필수 | 파일 1, 행 ~60 | 없음(리팩터가 동일 바이트 프로토콜을 보존해야 함 — D 불필요, 회귀 테스트 필수) | 위험(리팩터 실수 시 핫패스 회귀 가능) |
| 7 | 3 shallow | stream.cpp:360-610 (`decode_packet_bytes`) | 250행 단일 함수. 로컬 람다 4개(packet_total_size/fail_packet/enqueue_parts/enqueue_completed_state)로 부분 분해는 되어 있으나 함수 자체는 여전히 250행 | 람다들을 private 멤버 함수로 승격(캡처 필요한 것은 `state`/`source_pipe_`/`source_rid_` 인자화), fast-path(#6)와 state-machine 본체를 별도 함수로 분리 | 파일 1(hpp에 멤버 선언 추가), 행 이동 위주(순감소는 작음) | 없음 | 위험(#6과 동일 사유, 함께 진행) |
| 8 | 3 shallow(인터페이스 과다) | stream.cpp:918-995 (`xsend_routed`), 5개 `LIBZLINK_UNUSED` 매개변수(928-932) + 매개변수 총 13개 | ROUTER용으로 설계된 범용 routed-send 가상 인터페이스(관측 파이프 재사용 검증·observer·request_only·route incarnation)를 STREAM은 대부분 무시 — 정보 누출이 아니라 "STREAM엔 과한 인터페이스"(ISP 위반형 얕은 구현) | 리팩터 범위상 base 가상함수 시그니처 축소는 R1 밖(ROUTER 쪽 동시 변경 필요) → 이번 job에서는 항목만 기록, 실제 조치는 base 인터페이스를 다루는 별도 phase 브리프로 이관 제안 | 파일 3+(stream.hpp/cpp, socket_base.hpp, router 쪽) | 있음→D 필요(가상 인터페이스 변경은 ROUTER 계약도 건드림) | 없음 |
| 9 | 4 owner | pipe_stream_packet_state.hpp 전체, pipe.hpp:96/200/269, pipe.cpp:213-237/1002-1017 | STREAM 전용 패킷 프레이밍 상태(`pipe_stream_packet_state_t`)가 Core 공용 계층인 `pipe_t`(`_transport_lifetime`) 멤버로 박혀 있고, 사용자는 stream.cpp 단 한 곳(`source_pipe_->stream_packet_state()`, stream.cpp:371)뿐 | STREAM 쪽에서 `map<pipe_t*, state>` 등으로 소유권을 옮기는 안이 이상적이나, per-pipe 수명과 얽혀 있어 위험도 높음. 이번 job 범위 밖 — **확인 필요**: pipe_t 확장포인트(사용자 데이터 슬롯) 존재 여부를 Core 담당자에게 문의 후 D로 제안 | 파일 3+(pipe.hpp/pipe.cpp/stream.cpp) | 있음→D 필요 | 위험(각 pipe당 상태 접근 경로 변경) |
| 10 | 2 dup(약한 신뢰도) | stream.hpp:150-155 (`_route_shards`/`_route_publication_mutex`) vs socket_base.hpp:1517-1523 (`routing_socket_base_t::_out_pipes`) | STREAM이 라우팅용으로 자체 `_route_shards`(송신 실경로)를 쓰면서, 상속받은 `_out_pipes`(add/has/erase_out_pipe, stream.cpp:277,1270-1271)도 병행 유지. `_out_pipes`는 STREAM 자신은 읽지 않지만 base의 `xsubmit_retry_allowed`(socket_base_routing.cpp:267)가 간접적으로 `lookup_out_pipe`를 통해 읽음 — 완전한 dead mirror는 아님 | 통합은 base 클래스(ROUTER 공유) 영향까지 검토해야 함 — **확인 필요**: 실제로 STREAM에서 submit-retry 경로(target_rid_ 지정 blocking send)가 쓰이는지 통합테스트로 확인 후 착수 여부 결정 | 파일 2+(stream.cpp, socket_base_routing.cpp) | 있음→D 필요 | 없음/이득(락 두 개 유지 비용 감소 가능) |
| 11 | 3 shallow(항상 같은 값) | stream.cpp:1096-1099 (`xhas_out`) | 항상 `true` 반환 — 실제 쓰기 가능 여부는 매 호출 시 `check_write_admission`/route lookup으로 별도 판정. 의도적 설계(폴러가 라우트 준비 여부까지 볼 필요 없음)로 보이나 문서화 없음 | 주석 추가만으로 충분해 보임(로직 변경 불필요) — **확인 필요**: 05-polling 문서와 대조해 의도 확인 | 파일 1, 행 1(주석) | 없음 | 없음 |
| 12 | 3 shallow(파일 배치) | stream_dispatch_lifecycle.cpp(전체 15행) | `sockets/` 트리 전체에서 `*_dispatch_lifecycle.cpp` 패턴은 이 파일이 유일(다른 소켓 타입에 대응 파일 없음) — 명명 관례가 아니라 단일 함수용 별도 파일 | stream.cpp로 병합, 파일 삭제 | 파일 2 → 1 | 없음 | 없음 |

집계: 분류1(dead) 4건, 분류2(dup) 3건, 분류3(shallow) 4건, 분류4(owner) 1건, 분류5(이름 불일치) 0건 — 이름-개념 불일치는 못 찾음(전수 확인: `_api_mutex`=recursive_mutex로 이름과 일치, `route_lifecycle_mutex()` 오버라이드 없음도 주석과 일치). 총 12건.

## 적용 job 묶음 제안 (파일 겹침 없음, 각 1.5h 이내)

- **묶음 A (안전, 저위험)**: #2 → #12 → #3 → #1 순서. 전부 stream.cpp/.hpp/stream_dispatch_lifecycle.cpp만 건드리고 서로 겹치는 로직 없음(#1은 확인 필요 표시 있으므로 먼저 컴파일러/심벌 확인 후 진행). 계약 영향 없음, 빌드만 통과하면 됨.
- **묶음 B (락-패턴 통합, 중위험)**: #5 단독. stream.cpp의 8개 호출부를 헬퍼로 교체 + 동작 동일성 회귀 테스트. 파일은 A와 같은 stream.cpp이므로 A와 동시 진행 금지(순차 실행), A 완료 후 별도 job으로.
- **묶음 C (핫패스 리팩터, 고위험, 별도 게이트 필요)**: #6+#7 묶어서 진행(같은 함수를 다루므로 분리 무의미). 반드시 S-B 벤치마크 재측정을 게이트로 걸 것. 측정 job 종료 후 착수.
- **보류(이번 phase 범위 밖, D 필요)**: #8, #9, #10, #11 — 각각 base 인터페이스/Core 계층/의도 확인이 선행돼야 함. 별도 브리프로 승격 제안.

보고 경로: `doc/plan/c016-worklog/core-rf-R1-inventory.md`
