# G-3 진행

- 10:37 브리프·G-A·S-1·G-2 정독 완료. worktree ~/project/zlink-work/g3 (detached a12ef5c515) 생성.
- 다음: pipe.cpp/pipe.hpp/ypipe.hpp 정독, 인라인 예산 확인, before hotpath 5셀 측정.
- 11:12 pipe.cpp의 1규칙 술어 14개를 pipe.hpp inline으로 이동(+publish_session_outbound_accounting 냉·열 분리). hotpath 5셀 전부 감소(-1.1%~-3.4%). 다음: ctest 5회, TSan.
- 11:45 완료. ctest 5회 71/71 통과, lost-wake until-fail:10 통과, TSan 신규 경고 0, hotpath 5셀 전부 감소. 보고서 core-rf-G-3-summary.md 작성. 커밋하지 않음.
