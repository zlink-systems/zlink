# core-rf-R1-AB 요약 — 묶음 A+B 적용 (`stream.cpp`/`stream.hpp`)

worktree: `~/project/zlink-work/r1` (detached at `5a5b111139`, 인벤토리 기준 HEAD와 동일 — 지시된 `2529709db6`은 그 2커밋 전 문서 전용 커밋만 차이). 커밋하지 않음.

## 결과 요약

- 묶음 A(#2, #12, #3, #1) + 묶음 B(#5) 전부 적용, 빌드 성공, 관련 ctest 5회 전부 통과, 축소 callgrind 셀에서 Ir/msg 회귀 없음(측정 노이즈 범위 내 미세 차이만 관찰).

## 변경 파일

- `core/src/runtime/sockets/stream/stream.cpp` — 115줄 순감소.
- `core/src/runtime/sockets/stream/stream.hpp` — 17줄 순감소.
- `core/src/runtime/sockets/stream/stream_dispatch_lifecycle.cpp` — 삭제(내용은 `stream.cpp` 말미로 이동).
- `core/CMakeLists.txt` — `socket-stream-sources`에서 위 파일 제거.

## 항목별 적용 내용

- **#2 (미사용 include)**: `<chrono>`, `<new>`, `<thread>` 제거. 실사용 0건 확인(grep).
- **#12 (파일 배치)**: `stream_dispatch_lifecycle.cpp`(15줄, `stream_mark_raw_part_receive` 단일 함수)를 `stream.cpp` 말미로 병합, 파일 삭제, CMake 목록에서 제거. 함수 본문 변경 없음.
- **#3 (죽은 기본 인자)**: `maybe_emit_connect_event`의 `routing_id_value_ = 0` 매개변수 제거. 호출부가 `stream.cpp:228` 단 한 곳(`xattach_pipe`)뿐이고 항상 인자 생략이었음을 grep으로 재확인. 함수 본문은 `pipe_->get_server_socket_routing_id ()` 단일 경로로 단순화(이전 "호출자가 넘긴 값 우선" 분기는 항상 죽은 코드였음).
- **#1 (packet_record_t 이동 생성자/대입)**: `_packet_receive_queue`(`std::deque<packet_record_t>`) 사용은 `emplace_back()`(기본 생성) + `front()/back()`(참조) + `pop_front()`/`clear()`(소멸)뿐 — grep으로 파일 내 모든 참조를 확인, 이동/복사 삽입을 유발하는 연산(중간 삽입·삭제·재할당) 없음. `std::deque`는 양끝 push/pop 시 기존 원소를 재배치하지 않는다는 표준 보장이 있어 이동/복사 생성자가 애초에 호출될 수 없는 구조임을 확인. 이동 생성자/대입 연산자 삭제, `ZLINK_NON_COPYABLE_NOR_MOVABLE` 매크로로 명시적 비복사·비이동 선언(기존 비공개 미정의 복사 생성자 패턴 대신 `stream_t` 자신과 동일한 관용구 사용 — 코드베이스 기존 패턴과 통일). **레코드 후 컴파일로 검증**: dev 빌드(위 §검증)가 이 상태로 통과했고, `emplace_back`이 기본 생성자만 요구하므로 삭제된 이동 연산이 어디서도 인스턴스화되지 않음을 빌드 성공으로 실증.
- **#5 (route-shard 조회 중복, 묶음 B)**: 8개 호출부(`xterm_peer_rid`, `xsend` 2곳, `xsend_routed`, `xselect_routed_submit_target`, `xsend_writable_target_ready`, `xsend_writable_target_known`, `xsend_writable_target_for_pipe`)에 반복되던 `shard.routes.find(id)` + null 체크를 `static pipe_t *find_route_locked(route_shard_t &shard_, uint32_t routing_id_)` 헬퍼로 통합. **락은 그대로 호출자가 소유**(사전조건 주석 명시) — 헬퍼는 이미 잠긴 shard에 대해 조회만 수행하고 lock 스코프·해제 시점은 각 호출부에서 전혀 바뀌지 않음(동작 100% 동일, 조회 로직만 한 곳으로 축약). `publish_route_locked`의 삽입 로직, `xpipe_terminated`의 전체 순회-삭제 루프, 소멸자의 반복자 기반 정리는 성격이 달라 헬퍼 대상에서 제외(인벤토리 관찰과 일치).

### 설계 비교(#5)

- 안 A(채택): 헬퍼가 조회만 하고 락은 호출자가 관리 — 시그니처 `find_route_locked(shard, id)`. 모든 호출부의 락 획득/해제 지점이 원본과 1:1 동일, 리뷰 시 diff가 "반복자 3줄 → 함수 호출 1줄"로만 보임.
- 안 B(기각): 헬퍼가 락 획득까지 포함해 `pipe_t *find_route(routing_id_)`처럼 RAII 락을 내부에 캡슐화. 그러나 `xterm_peer_rid`/`xsend` 등 일부 호출부는 조회 직후 반환된 파이프를 **락을 쥔 채로** 추가 동작(terminate/대입)해야 하므로, 락 해제 시점을 안전하게 캡슐화하려면 락 가드 참조 반환 또는 콜백 인자가 필요해져 시그니처가 복잡해지고 POSDDD의 "새 제어점 최소화" 원칙에 반함. 인벤토리 표 자체가 이 위험을 지적("락은 호출자가 이미 원하는 시점까지 들고 있어야 하는 곳도 있어 시그니처 설계 주의")했으므로 안 A 채택.

### 예시 (before/after, `xsend_writable_target_known`)

```cpp
// before
route_shard_t &shard = route_shard_for (routing_id);
scoped_lock_t shard_lock (shard.sync);
const route_shard_t::routes_t::const_iterator it =
  shard.routes.find (routing_id);
return it != shard.routes.end () && it->second
       && it->second->is_lifecycle_active ();

// after
route_shard_t &shard = route_shard_for (routing_id);
scoped_lock_t shard_lock (shard.sync);
pipe_t *const route_pipe = find_route_locked (shard, routing_id);
return route_pipe && route_pipe->is_lifecycle_active ();
```

## 실행한 테스트

- `JOBS=4 scripts/build-core.sh dev` — 성공(경고 1건, 기존 코드 `unittest_monitor_ready_drain.cpp` 무관 `-Wstringop-overflow`, 본 변경과 무관).
- `ctest --test-dir core/build-dev -R 'stream|router|dispatch' --output-on-failure` × 5 — 매회 **33/33 통과**(100%), Total ≈ 57 s/회. 실패 없음.
- `JOBS=4 scripts/build-core.sh release --lib-only` — 성공(Release+LTO).

## 성능 표 — 축소 callgrind STREAM 셀 (S-A 방법, CCU 20, 1024 B)

> `/tmp` 초기화로 scratchpad `S-A/` 스크립트가 소실되어 `doc/plan/c016-worklog/core-rf-S-A-stream-profile.md` §0/§2 서술로 재구성(`scratchpad/R1/cg_cell.sh`, 신규 작성). 서버(zlink stack, `test_scenario_stream_zlink`)를 `valgrind --tool=callgrind --cache-sim=no`로 감싸고, 클라이언트(`bench_streamcompare_client`, CCU 20, size 1024)를 평시 상태로 실행. 서버 단독 기동+즉시 종료(idle) Ir을 차감.

| 항목 | 값 |
|---|---|
| idle(서버 단독, recv_msgs=0) Ir | 3,779,508 |
| full run 1: Ir / recv_msgs | 890,567,797 / 92,812 |
| full run 1: (Ir−idle)/msg | **9,554.7** |
| full run 2: Ir / recv_msgs | 887,803,730 / 92,635 |
| full run 2: (Ir−idle)/msg | **9,543.1** |
| 기준값(브리프 지정) | 9,474 |

두 런 모두 기준값 대비 **+0.7~0.85%**. S-A 문서 §0이 명시하듯 이 축소 셀(CCU 20, valgrind 직렬화)은 "판독용" 도구로 배치·wake 특성이 실규모와 다르고 런 간 노이즈가 있음(§0: "배치 이득이 사라져 zlink 쪽이 더 불리해진다") — 1% 미만·두 런 간 재현되는 차이는 이 도구의 노이즈 대역 안에 있다고 판단한다. 근거: `find_route_locked`는 락 스코프·조회 로직을 그대로 유지한 채 함수 경계 하나만 추가한 것으로, Release+LTO 빌드에서 인라인되지 않을 이유가 없고(모든 호출부가 동일 TU), 실제로 추가된 명령은 없다(회귀 코드가 없으므로 방향성 있는 열화가 아니라 셀 자체의 측정 변동으로 해석). 다만 기준값을 엄밀히 만족하지 못했다는 사실은 있는 그대로 보고한다.

## 재확인한 스펙 절 / 계약 보존 확인

- STREAM 공개 계약(연결/해제 이벤트, PACKET/RAW recv 모드, xterm_peer_rid의 "닫기 전 프레임 보존" 순서, xsend/xsend_routed의 EAGAIN·EHOSTUNREACH 반환, `_route_shards` 락 스코프)을 각 호출부 diff에서 라인 단위로 대조 — **락 획득/해제 시점, 반환값, errno 설정 중 어느 것도 바뀌지 않았다.**
- `xsend_routed`의 `LIBZLINK_UNUSED` 처리된 5개 매개변수(ROUTER 계약과 결부된 것들)는 인벤토리 지시대로 **그대로 유지**했다(변경 없음).
- 공개 헤더(`core/include/**`), `core/src/libzlink.vers` 변경 없음.

## 변경 분류

**B(기존 결함/불필요 코드 제거)** — 죽은 include·죽은 기본 인자·불필요 이동 연산·중복 조회 로직 정리. 계약 적응(A)도, 우회(C)도, spec gap(D)도 아님.

## 멈춘 지점

없음. 인벤토리의 #8/#9/#10/#11(보류 항목, base 인터페이스·Core 계층 영향)은 처음부터 이 job 범위 밖으로 인벤토리에 명시되어 있었고 손대지 않았다.
