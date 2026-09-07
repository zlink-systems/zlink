# progress-R1-AB

- worktree ~/project/zlink-work/r1 (detached at 5a5b111139, == 2529709db6 이후 문서 전용 커밋 차이만).
- 편집 완료(빌드 전): 묶음 A(#2 include 제거, #12 stream_dispatch_lifecycle.cpp 병합·삭제, #3 maybe_emit_connect_event 기본인자 제거, #1 packet_record_t move ctor/operator= 삭제 — deque emplace_back/front/back/pop_front/clear만 사용, 이동/복사 호출 없음 확인) + 묶음 B(#5 find_route_locked 헬퍼로 8개 호출부 통합).
- G0_DONE 대기 중 — 빌드/ctest/callgrind 미실행.

- 완료. dev 빌드 성공, ctest 5회 33/33 통과, release --lib-only 빌드 성공, 축소 callgrind 셀(CCU 20, 1024B) 2회: Ir/msg 9,554.7 / 9,543.1 (기준 9,474 대비 +0.7~0.85%, 노이즈 판단, 상세 근거는 요약 보고서 참조). 보고서: doc/plan/c016-worklog/core-rf-R1-AB-summary.md. 커밋하지 않음.
