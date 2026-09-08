# MP-3 진행

- 2026-09-07 시작: 공통 규칙 확인, MP-2 diff를 `mp2-before-mp3.patch`로 보존.
- 현재: detached worktree의 기존 24개 파일 변경 범위와 리뷰·계약·설계 근거를 재검증 중.
- 다음: B01~B06/W01~W07/S01~S02를 소유 상태·수명 경계별로 수정하고 작은 테스트부터 실행.
- 1차 수정: token checkout 상태 결과 분리(B01), revoke 즉시 capacity 반환(B05), close의 sequence store 무할당 swap(B04), TLS identity ENOMEM 경계와 strong API pin(B03/W06), validation 동안 public-handle pin(B06), DONTWAIT complete scope 밖 만료 해제(B02), MORE ETERM 검사(W05)를 반영. 현재 컴파일 검증 전.
- 2차: production 컴파일 성공. B01 회귀가 기존 MP-2 기대값(NOT_FOUND)을 정확히 실패시켜 EBUSY로 복구했고, 서로 다른 token 동시 성공·completion ID/context/payload 전수 대조·staging/context/TLS OOM·revoke counter 테스트를 추가 중. 테스트 target include 경계 1건을 unittest로 이동 후 재빌드 중.
- 3차: 최신 production 빌드와 직접 관련 4개 target 통과. dev 전체 suite를 지시대로 한 번만 실행해 `hotpath_gate` 제외 207/207 통과(244.12초). 다음은 W07 경계 보강, 관련 regex 반복, lost-wake 반복, sanitizer 순서.
- 4차: W07의 같은 thread/두 socket, 5-part spill, 네 caller slot 동시 close+zero-copy exact-once 테스트를 추가해 직접 target 통과. 관련 regex 80개를 `until-fail:5`로 포그라운드 실행 중이며 현재 실패 없음.
- 5차: 관련 regex 80개 × 5회=400회 전부 통과(678.73초). CMake의 `wake-invariant` 라벨 4개를 `until-fail:20`으로 실행 중이며 장시간 public matrix 연속 통과 중.
- 6차: lost-wake 4개 × 20회=80회 전부 통과(620.10초). ASan+LSan 434 target 재빌드 성공, close/abandoned/OOM 포함 직접 변경 6개 target 통과(16.37초, sanitizer/leak 오류 없음). 다음 GCC TSan.
- 7차: GCC TSan 계측 빌드 성공(`__tsan_read*` 확인), 직접 변경 executable 8/8 통과. 관련 80개는 75/80 통과; 실패 5개는 MP-2와 동일한 monitor/ctx lock-order 3개, 10ms timing 1개, flow-state `lb.cpp:159` race 1개로 multipart 변경 경로 밖 기존 debt.
- 완료: Release+LTO hotpath 5/5 PASS. PERF_LOCK dev 비교는 single-after-multipart main 22.491ms/MP-3 27.821ms, 4-thread 2-part main 20초 timeout/MP-3 110.396ms(724.7k record/s). 서로 다른 RID와 MORE-close race 보강 target도 dev·ASan·TSan 통과. 보고서 `core-rf-MP-3-report.md` 작성, patch 미커밋 유지.
- 최종 보강: staged REQUEST FINAL이 실제로 block된 상태에서 context shutdown이 `TERMINATED/ETERM`, ID 0, part 소비로 깨우는 W05/W07 test 추가. dev·ASan+LSan·GCC TSan에서 해당 target 통과.
