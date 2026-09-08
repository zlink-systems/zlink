# review-CCU-2 진행

- 2026-09-08 18:06 KST: 시작. 공통 규칙, 브랜치(분리 HEAD), 미커밋 9개 파일 상태를 확인했다.
- 현재 단계: 입력 보고서·스펙·diff·테스트를 대조하는 정적 차단 검증.
- 변경 제한: 소스·스펙·테스트 무수정, 커밋 금지. 결과 보고서와 이 진행 파일만 작성한다.
- 2026-09-08 18:14 KST: 증분식과 전체 water-filling을 대조했다. 비포화 확장은 aggregate 숫자만 새 식으로 바꾸고 기존 방향의 실제 `planned_hwm`은 유지해 스펙의 현재 목표 합계와 어긋나는 차단 후보를 확인했다. `plan_member` 및 plan aggregate 등 새 상태가 추가된 사실도 확인했다.
- 검증 대기: 다른 작업의 `ninja` 1개가 실행 중이고 load 5.50이라 TSan 구성 및 hotpath 측정을 시작하지 않았다. 메모리 available 8833 MiB는 충족한다.
- 2026-09-08 18:25 KST: `core/build-tsan`을 GCC 13.3, `-fsanitize=thread`, LTO off로 새 구성했다. `setarch x86_64 -R`, 억제 파일 없이 auto-HWM/ctx/attach 관련 8개 CTest가 8/8 통과했고 TSan report 0건이다. 저장소에는 지시의 `core/tests/scale_test_*`가 존재하지 않는다.
- 2026-09-08 18:34 KST: Release+LTO `core/build`을 JOBS=4로 빌드해 성공했다. 다른 worktree의 `ninja` 1개와 load1 4.23이 남아 있어 성능 측정은 아직 시작하지 않았다(available 8822 MiB).
- 현재 단계: `ninja=0`, load1 < 1.0을 기다려 `hotpath_gate` 5셀을 `PERF_LOCK` 아래 실행하고 최종 보고서를 작성한다.
- 2026-09-08 18:42 KST: 감독관의 18:25 D-H1 채택과 한·영 스펙 갱신을 확인해 최신 계약으로 재판정했다. 정적 검토는 attach allocation 예외와 중복 plan 상태/POSDDD 위반 2건을 차단으로 확정했다. 다른 worktree의 make 빌드로 load1 2.43이어서 hotpath 시작 조건만 대기 중이다.
- 2026-09-08 18:50 KST: semantic publication race를 추가 차단으로 확정했다. hotpath를 load1 0.76, ninja 0, available 10668 MiB와 `PERF_LOCK` 아래 1회 실행했다. 4/5 PASS이고 `dealer_router_reqrep_inproc`이 ratio 0.9448(-5.52%)로 대칭 gate를 0.52%p 벗어나 FAIL했다. 반복하지 않고 최종 보고서에 기록했다.
- 현재 단계: 보고서 근거·마지막 판정 검산 및 변경 제한 확인.
- 2026-09-08 18:52 KST: 최종 검산 완료. artifact 785줄/9파일을 기준으로 리뷰했고, `git diff --check` 이상 없음. 리뷰어의 소스·스펙·테스트 수정 및 커밋은 없다. 최종 판정은 차단 3건, 채택 불가다.
