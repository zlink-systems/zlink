2026-09-05 시작: detached main 기준, 기존 사용자 변경은 core/build·core/build-dev symlink뿐임을 확인. bindings/rust/**만 수정하며 공식 Rust 테스트 경로와 submit 매핑을 조사 중.
2026-09-05 구현 1차: native submit rc 0–13 전 값 보존 helper, errno fallback 보완(ENOBUFS→Backpressured 포함), SEND/REQUEST async rc 우선 분기, terminal 전용 정규화를 적용함.
2026-09-05 테스트 보강: NOT_CONNECTED·NOT_ADMITTED·WRITABLE retry·명시 제거 NotFound·close Terminated를 raw errno assertion 없이 구분하도록 기존 contract를 강화하고 ROUTER→DEALER request 회귀 테스트를 추가; 신규 테스트 1회 통과.
2026-09-05 검증 중: NOT_ADMITTED 신규 회귀 총 5/5 통과, send_failure 9/9 및 routed_async 18/18 통과. enum 0–13 전 값·ENOBUFS·terminal narrow mapping unit test 13/13 통과.
2026-09-05 최종: 공식 Rust 러너 14/14 PASS(samples 포함), clippy all-targets 경고 0, cargo fmt 및 git diff check 통과. Core 재빌드·spec 수정 없음, BLOCKERS 없음.
EXIT:0
