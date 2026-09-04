2026-09-04 18:49:07 KST START detached HEAD 70a999899813 확인. bindings/rust 작업 트리는 clean이며 core/build, core/build-dev symlink는 보존한다.
2026-09-04 18:49:07 KST SCOPE bindings/rust/** 구현·테스트·문서와 지정된 r2 진행/요약 파일만 변경. core 빌드/clean, local-package, git commit/push/checkout 금지.
2026-09-04 18:49:07 KST AUDIT Rust API, 테스트/gate, 확정 Core 계약 및 선행 포팅 예시를 병렬 조사 시작.
2026-09-04 18:52 KST BASELINE 첫 targeted cargo 명령은 시스템 Cargo 1.75가 edition 2024를 지원하지 않아 실행 전 실패. 표준 스크립트처럼 /home/hep7hep7/.cargo/env를 적용해 Cargo 1.97.1로 재실행한다.
2026-09-04 19:03 KST IMPLEMENT async SEND를 binding-owned packet + DONTWAIT single-attempt + WRITABLE token 재시도 상태기로 전환. SEND용 private reactor는 executor turn timeout-0 probe이며 OS thread/timer 없음; REQUEST fallback thread는 유지.
2026-09-04 19:03 KST API CompletionKind::{Send=1,Request=2,Writable=3} 공개, FFI kind=3 추가, PENDING_MAX private ABI에 REQUEST-only 주석 추가, poller가 POLLOUT|POLLCOMPLETION에서 queue drain하도록 수정.
2026-09-04 19:03 KST TEST targeted lib 4/4, 즉시 admission, 기존 HWM 재개, 신규 sleep-free public Poller HWM→POLLOUT→retry, enum surface 통과. REQUEST pollcompletion은 첫 단독 실행에서 연결 race로 timeout 후 즉시 재실행 통과; 전체 gate에서 재확인 예정.
2026-09-04 19:24 KST HARDEN completion drain 직렬화, public/private owner handoff wake, live-token tombstone, submit/close 직렬화, 반복 token/RID/terminal 검증을 반영. SEND 전환 시 기존 async/sync REQUEST waiter가 passive reactor를 이어받도록 progress handshake 추가.
2026-09-04 19:24 KST TEST 신규 public Poller multipart retry, close terminal, missing ROUTER route 즉시 NOT_CONNECTED, REQUEST public completion 및 관련 routed/behavior/contract suite targeted green. 연결 race가 있던 REQUEST tests는 blocking data handshake로 결정화.
2026-09-04 19:29 KST GATE 최종 신규 HWM→WRITABLE→동일 multipart retry 5/5 green. 표준 bindings/rust/tests/run_tests.sh 14/14 PASS(lib, integration 12 suites, samples), cargo fmt check PASS, cargo doc PASS.
2026-09-04 19:29 KST VERIFY git diff --check PASS, bindings/rust/include 8개 mirror 전부 core/include와 byte-identical. 변경은 bindings/rust/**와 지정된 r2 로그/요약만이며 남은 실패 없음.
2026-09-04 19:29 KST COMPLETE 요약: /home/hep7hep7/project/zlink-work/c016/bindings-dontwait-rust-r2-summary.md
EXIT:0
