# progress R6R8-A (완료)

2026-09-07 완료. worktree ~/project/zlink-work/r6r8 detached from main 482d7bca80. 커밋 안 함.
적용: R6 bundle A(#7 주석삭제, #3 find_locked 헬퍼 7곳) + R8 bundle A(dist has_pipe->contains) + R8 bundle B(xsend_routed 디버그 6블록 추출).
보류: R6 #2 policy_class — socket_base_monitor.cpp:117에서 공개 모니터 스냅샷으로 노출 중(readers 존재), 인벤토리 오탐 확인, 미적용.
검증: dev 빌드 성공, ctest -R 'registry|hwm|auto|dist|router|pubsub|dealer|physical|queue' 5회 38/38 통과, hotpath_gate(build-gate 트리, hotpath_bench만) dealer_dealer_inproc 0.9877 PASS / router_router_tcp 1.0031 PASS.
남은 이슈: xsend_routed 331행(목표 250행 미달, 순수 추출 범위 내 한계 — 보고서 참고).
보고서: doc/plan/c016-worklog/core-rf-R6R8-A-summary.md
