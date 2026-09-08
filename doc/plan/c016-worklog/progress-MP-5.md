# MP-5 진행

- 2026-09-07 KST: 공통 규칙, detached worktree와 기존 MP-3+MP-4 변경을 확인했다. 변경 전 diff를 지정된 `mp4-before-mp5.patch`에 보존했다. MP-4 네 파일의 single-part 추가 비용과 MP-3/current callgrind 비교를 조사 중이다.
- 2026-09-07 KST: MP-4 경계를 네 파일로 재구성했다. helper가 존재할 때의 counter load가 `prepare_send_step_locked()`와 single REQUEST lookup에 추가됐다. 보존 patch를 이용해 worktree를 MP-3 상태로 전환했고 Release+LTO lib 및 `hotpath_bench` 빌드를 완료했다. load가 1.5 아래로 내려가면 `pair_inproc` callgrind 기준을 수집한다.
- 2026-09-07 KST: 동일 `build-gate`를 MP-3 patch와 MP-4 current로 각각 다시 링크해 `pair_inproc` 20,000회를 측정했다. MP-3 49,034,133 Ir(2451.707/msg), MP-4 49,015,670 Ir(2450.784/msg)이고 `prepare_send_step_locked()` self cost는 둘 다 820,000 Ir다. 보고서의 MP-3 2341.905는 재현되지 않아 과거 static runner 링크 상태를 조사 중이다.
- 2026-09-07 KST: 최종 MP-3 patch의 실제 추가 비용은 helper 확인보다 먼저 single `FINAL`에도 여는 staging public-API scope였다. helper 미생성 SEND/SEND_RID는 complete-record 경로로 즉시 보내고, REQUEST는 pinned socket의 borrowed helper pointer로 존재를 먼저 확인하도록 수정했다. Dev build와 직접 영향 5 target은 통과했고 관련 정규식 `until-fail:2`를 실행 중이다.
- 2026-09-07 KST: 관련 정규식 40 target × 2(80회)가 모두 통과했다(257.94초). ASan+LSan 6/6, GCC TSan 직접 8/8도 통과했다. Release+LTO lib와 `hotpath_bench`를 최종 갱신한 뒤 load<1.5 조건에서 5셀 및 동일 dev runner 2셀을 측정할 예정이다.
- 2026-09-07 KST: counter-zero 판정을 `borrow_send_sequence_state()` 한 곳으로 통합해 helper가 없거나 caller slot 수가 0이면 staging scope 전에 direct path로 보냈다. 동일 dev runner의 최종 single 두 표본은 20.983/23.431ms(평균 22.207ms, main 대비 -1.26%), 4-thread는 98.513ms다. 최종 ASan 6/6·TSan 8/8을 재확인했고 관련 정규식 `until-fail:2`를 마지막으로 재실행 중이다.
- 2026-09-07 KST: 최종 코드 기준 관련 정규식 40 target × 2(80회)도 모두 통과했다(258.04초). 이제 다른 ninja가 없고 load가 1.5 미만인 상태에서 Release+LTO를 한 번 갱신해 최종 5셀을 측정한다.
- 2026-09-07 KST: 최종 Release+LTO 5셀은 모두 PASS다. MP-3 대비 증가했던 dealer/dealer, pair, router/router는 각각 -0.46%, -0.64%, -0.31%이고, 최종 pair callgrind는 MP-4보다 2,492,215 Ir(124.611 Ir/msg) 적다. 보고서와 변경 범위 검사를 마무리한다.
- 2026-09-07 KST: `core-rf-MP-5-report.md`에 원인·함수별 callgrind 차이·수정·검증·성능·소유권/분류를 기록했다. `git diff --check`, public API/ABI 무변경과 미커밋 상태를 최종 확인한다.
