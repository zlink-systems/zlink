# ln3pr2 진행 기록

- 범위: PR #4415 pre-merge 비교 측정만 수행한다. production source 수정 없음. 측정은 셀별 A-L2, B-L2, A-L3, B-L3 순서이며 한 셀의 네 실행을 같은 exclusive hold에 둔다. 셀당 warmup 1초, active 5초, 실행 사이 3초, discarded warm run 없음.
- A: `HEAD=6350275facc547759c9e143cc2515f3f528c776e`, merge-base 동일. B: `HEAD=ec6aa8ea5c4e6f59f8b8fc494febc8d928b9dba8`, merge-base `6350275facc547759c9e143cc2515f3f528c776e`.
- A/B 빌드 모두 종료 코드 0. 각 worktree의 Core binding 1.25.0 package: `/root/.cache/zlink/packages/local-1.25.0/node/npm/zlink-systems-zlink-1.25.0.tgz`; Framework `framework/languages/node`와 bench `build.sh`를 각 worktree에서 실행했다. 빌드 세부 해시와 bench-only diff는 `report.md`에 기록.
- PR 대상 dist JS 3개는 A/B 해시가 다르고 각 `.ts` source보다 새롭다. A/B dist 파일 수는 각각 1,059개이며, 이 외 차이는 `report.md`에 모두 기록했다.
- build 완료 후 측정 잠금 대기 전 확인: load average `7.32`; 다른 빌드 프로세스가 R/S 상태로 실행 중이어서 규칙에 따라 종료를 기다린다(최대 10분). 다른 worktree는 건드리지 않았다.

## 셀 측정

아직 측정값 없음. 첫 측정 전에 해당 셀의 binding/L-step receive·reply 호출 위치와 per-message waits/allocations/atomics/copies를 각 실행마다 아래 기록에 덧붙인다.
- 2026-11-14T01:04:28Z build-process wait started under BUILDS BETWEEN HOLDS; at the 10-minute limit, take the blocking exclusive measure lock even if unrelated builds remain. No lock is held by this job.
- 2026-11-14T01:14:28Z ten-minute build wait completed; unrelated build processes remain. Beginning the required blocking exclusive lock request.
- 2026-11-14T01:15:44Z first blocking hold acquired, but measurement did not start: helper exited 126 (execute permission missing). No raw result exists. Corrected helper executable bits; restarting pre-hold build check.
- 2026-11-14T01:16:05Z second pre-hold build wait started; unrelated R/S builds remain.
