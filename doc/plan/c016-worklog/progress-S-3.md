# S-3 진행 (decoder 버퍼 재사용 확대)

- [x] 브리프·S-A §0/§3/§4#5·S-B 3b#3 정독
- [x] worktree ~/project/zlink-work/s3 (detached 430abce139)
- [x] 현행 구조 파악: spare 1칸 CAS, raw_decoder max_messages_=1, allocate() 2회/msg
- [ ] 설계 A(다중 spare 슬롯) 구현
- [ ] dev 빌드 + ctest 5회
- [ ] 축소 callgrind 셀 (after)
- [ ] with_stream 성능 셀
- [ ] ASan 1회
- [ ] 보고서

## 2026-09-07
- 설계 A 구현 완료: `shared_message_memory_allocator_state_t`의 spare 1칸 → 4칸 링.
  - `recycle_or_destroy_buffer`가 빈 슬롯을 순차 CAS, 다 차면 기존대로 즉시 free.
  - `take_exact_spare`가 슬롯을 순차 xchg, 크기 불일치 블록은 그대로 파기(링이 target 변경 시 자연 배수).
  - `stop_recycling`이 전 슬롯에 CLOSED sentinel을 넣고 회수분을 소멸자가 파기.
- unittest_zmp_decoder.cpp의 spare 정책 테스트 2건 갱신(1칸 전제 → 링 상한 4 / 경쟁 케이스 1~2 허용).
- dev 빌드 진행 중.

## 측정 결과 — S-3 전제가 반증됨
- 축소 callgrind 셀(zlink 1024 B, CCU 20, 15 s, worktree release lib): 4칸 spare 링 적용 후
  `shared_message_memory_allocator::allocate()`가 부른 malloc = **41회 / 80,155 msg (0.0005/msg)**.
- **baseline(S-A `cg_zlink.out`)에서도 같은 값 = 41회 / 57,663 msg (0.0007/msg)**.
  → decoder spare 1칸은 이미 정상 상태에서 100% 재사용 중이었고, S-A §4#5의
  "수신 버퍼 malloc/free 1.31+1.47/msg" 귀속은 오귀속이다.
- 실제 메시지당 malloc/free의 정체(baseline, per msg):
  `operator new` 1.026 ← `asio_engine_t::start_async_read()`, `operator delete` 1.039,
  `aligned_alloc` 0.263 ← `boost::asio::detail::thread_info_base::allocate`.
- 원인: `engine/asio/handler_allocator.hpp`의 인라인 블록이 **1칸**이고, 다음 read는 이전
  read의 완료 핸들러 **안에서** 재무장되는데 ASIO는 핸들러가 반환된 뒤에야 op를 해제한다.
  따라서 재무장 시점에는 항상 `_in_use == true` → 매 메시지 heap fallback.
- 조치: decoder 링 변경은 **되돌림**(이득 0, 보유 바이트만 4배). 대신 `handler_allocator`를 2칸으로.

- [x] handler_allocator 1칸 → 2칸 구현, dev 빌드, ctest 8회(7회 clean, 1회 stream ready regression flake)
- [x] 보고서 doc/plan/c016-worklog/core-rf-S-3-summary.md
- [ ] (미실행, 상한 초과) ASan 1회, with_stream 성능 셀
