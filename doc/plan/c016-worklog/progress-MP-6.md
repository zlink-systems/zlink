# MP-6 진행

- 상태: 진행 중
- 범위: D-BP15의 2번 시나리오를 공개 C API 통합 테스트로 추가한다.
- 제약: `core/src`·스펙 수정 없음, commit·stash 없음, 빌드 `JOBS=4`, `ninja` 한 개 이하, foreground 실행.
- 2026-09-07: MP-5 이전 diff를 지정된 scratchpad의 `mp5-before-mp6.patch`로 보존했다(199,545 bytes).
- 2026-09-07: D-BP15·D-B207과 버그 문서의 "추가 발현"을 확인했다. REQUEST와 SEND 각각 inproc/tcp에서 열린 A sequence와 B의 WRITABLE 재제출이 독립적으로 완주하는지를 검증한다.
- 다음: 기존 fixture·poller/token 대기 helper를 재사용할 수 있는 최소 테스트 구조를 확정하고 구현한다.
- 2026-09-07: 독립 split CTest 2개를 추가했다. A는 `MORE` 뒤 조건변수에서 대기하고, B는 HWM 거절 token을 공개 poller로 받은 뒤 prefix부터 재제출하며, receiver가 B 완료 뒤 A를 해제한다.
- 2026-09-07: 첫 dev 빌드는 새 파일의 enum 기본 초기화 3건이 정수형이라 컴파일 실패했다. 명시적 공개 enum 값으로 수정했으며 기존 소스 실패는 없었다.
- 2026-09-07: 초기 HWM=1 fixture에서는 filler drain 뒤에도 2-part B record 자체의 frame charge가 HWM을 넘어서 재제출이 다시 거절됐다. 제품 결함 판정이 아니라 fixture 전제 오류다. B record가 들어갈 4 KiB HWM과 빈 pipe oversize 예외로 들어가는 64 KiB filler 조합으로 정정했다.
- 다음: until-fail:10, 관련 정규식 1회, 기존 ASan/TSan 구성의 새 테스트 1회를 실행하고 실패를 보고서에 고정한다.
- 2026-09-07: 최종 fixture는 SEND filler를 실제 pending WRITABLE token까지 채우고, accepted filler 전부를 receiver가 drain한다. tcp의 transient local backpressure token은 poller로 닫고 계속 채운다.
- 2026-09-07: 최종 결과 — SEND inproc/tcp는 until-fail 10/10 PASS. REQUEST inproc/tcp는 B 재제출·2-part 수신·reply까지 PASS지만, A sequence가 열린 동안 B REQUEST completion이 `NO_DATA/EAGAIN`으로 남아 FAIL.
- 2026-09-07: 관련 정규식 45/45 PASS. ASan/LSan은 같은 REQUEST 기능 실패 외 sanitizer/leak 없음. TSan은 `setarch x86_64 -R` 재실행에서 같은 기능 실패 외 race 보고 없음.
- 2026-09-07: MP-5 전 source diff와 현재 source diff SHA-256이 `74831e8c...538197`로 동일해 MP-6 source delta 0을 확인했다. 보고서 `core-rf-MP-6-report.md` 작성, patch 미커밋.
- 상태: 완료 — 테스트 추가 완료, REQUEST 회귀 RED를 보고하고 source 수정 없이 멈춤.
