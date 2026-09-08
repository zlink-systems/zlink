# core-rf-SD-5 진행

- 2026-09-09 KST: 작업 시작. 공통 규칙, branch와 SD-4 누적 diff, 설계 원칙, ZMP spec §9를 확인했다. `wip/sd-1`의 기존 변경은 보존하며 B-SD4-1의 실제 encoder/transport 호출 경로를 조사 중이다.
- 2026-09-09 KST: `async_writev`를 buffer array/count 계약으로 일반화하고, bounded gather가 첫 pointer body의 소유권을 completion에 유지한 채 뒤 frame을 target까지 encoder buffer에 계속 모으도록 구현했다. 영속 제어 상태는 split offset 하나만 추가했다. 기존 memory transport fake에 제출 횟수/buffer 수 관찰을 연결하고 네 반례를 실제 ZMP encoder 경로에 추가했다.
- 2026-09-09 KST: ASan으로 초기 test fixture의 큰 msg reference 해제 오류를 분리해 수정했다. 128 KiB target을 test-only로 고정한 실제 encoder 반례 4건이 모두 통과했다: `64 KiB→small` 3-buffer 1회, `small→64 KiB→small` 3-buffer 1회, max 초과 encoder 2회, pointer body 2개 2회.
- 2026-09-09 KST: 최종 dev 증분 빌드 성공. `test_zmp_ws_wss`, `test_asio_ws`, policy/transport/encoder unit 5/5 성공. 관련 `stream|asio|decoder|ws` 27-suite는 27/27을 3회 연속 통과(14.91/14.92/15.00초). `git diff --check` 성공.
- 2026-09-09 KST: 전체 `ctest -E hotpath_gate`는 pending 판별 결함을 split offset 예약 비트로 교정한 뒤 211/211 성공(246.16초). TSan 관련 27-suite도 27/27, 보고 0건이다.
- 2026-09-09 KST: Release WS/WSS 18-cell 단일 report는 run 1·2가 각각 ws/wss 0.967902/0.832113, 0.830596/1.358819로 PASS했다. run 3은 추가 RR-WS-64KiB endpoint 오류로 17/18 partial이라 gate FAIL로 보존했다. hotpath는 지시된 reqrep reference 15609.7872 적용 시 5/5 PASS다. 보고서와 patch를 확정한다.
- 2026-09-09 KST: 보고서와 base `84d25131a6` 누적 `all-artifacts/SD-5.patch`를 생성했다. report 3개를 포함하고 supervisor note·hotpath reference 파일은 제외했으며 reverse apply check를 통과했다. SHA-256은 `07a9bbc9b48c515b772368797a01f49cfb50507df7875a55fc7759376faa61d7`다.
