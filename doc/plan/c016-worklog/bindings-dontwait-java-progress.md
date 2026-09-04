# Java DONTWAIT 계약 B 진행 기록

- 2026-09-04: detached worktree와 기존 Core/타 바인딩 변경을 확인했다. Java 작업 트리는 깨끗하며 수정 범위를 `bindings/java/**`로 고정했다.
- 2026-09-04: completion ID, pending, POLLCOMPLETION, PENDING_MAX, DONTWAIT/BACKPRESSURED 참조에 대한 소스·테스트·문서 정찰을 시작했다.
- 2026-09-04: Core dev 빌드는 Java와 무관한 스냅샷 선언 누락(`fail_all_send_writable`)으로 29%에서 실패했다. Core는 범위 밖이라 수정하지 않았다.
- 2026-09-04: async SEND 상태를 즉시 성공(ID 0) 또는 BACKPRESSURED/EAGAIN 대기 토큰으로 분리하고, 바인딩 소유 패킷 snapshot·POLLOUT event loop·WRITABLE token/context/RID 검증·동일 패킷 재시도를 구현했다. REQUEST completion 경로는 유지했다.
- 2026-09-04: public `CompletionKind.WRITABLE`, REQUEST-only PENDING_MAX 설명, DONTWAIT/completion Javadoc·README 및 public HWM backpressure 계약 테스트를 추가했다. Java main/test 컴파일은 native resource 준비를 제외한 gate에서 성공했다.
