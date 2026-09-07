# Progress R9-ABC

시작: 2026-09-07, worktree ~/project/zlink-work/r9 (detached 80871f34f3)

## 계획
- 묶음 A: ssl_context_helper from_pem 체인(4개) + tcp.cpp tcp_write/tcp_read/tcp_open_socket 삭제
- 묶음 B: listener process_term/on_accept 공통 헬퍼 (tcp/ipc/tls/ws)
- 묶음 C: wss_address::to_string 중복 제거

## 상태 (완료)
- [x] 죽은코드 재확인(전체 repo grep) — 호출자 0 확인
- [x] 묶음 A 적용 (tcp.cpp/hpp, ssl_context_helper.cpp/hpp)
- [x] 묶음 B 적용 (asio_listener_accept_policy.hpp 헬퍼 2개 + 4개 리스너)
- [x] 묶음 C 적용 (ws_address::format_url, wss_address 재사용)
- [x] dev 빌드 성공
- [x] ctest 5x — 53/53 통과 x5
- [x] test_endpoint_release 10x — 10/10 rc=0
- [x] c binding smoke — dev lib 대상 수동 configure, 10/10 통과

커밋 안 함. 요약: doc/plan/c016-worklog/core-rf-R9-ABC-summary.md
