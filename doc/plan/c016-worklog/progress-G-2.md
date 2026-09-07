# G-2 진행

- 09:41 시작. worktree ~/project/zlink-work/g2 (detached f67625990a) 생성.
- 브리프·공통규칙·G-A·R10 읽음. 다음: msg.{hpp,cpp} 및 호출자 조사, join/leave 독자 0건 확인.
- 10:20 msg join/leave 제거 + init 인라인 + report_invalid 아웃라인. dev 빌드 성공. ctest 시작.
- 10:26 ctest 5회 통과 (50/50). release lib 빌드 시작(perf 측정용).
- 10:14 always_inline 추가, base vs after 측정 완료(−153~−323 Ir/msg). dev 재빌드+ctest 중.
- 10:33 hotpath 5셀 before/after 확보(−1.5~−6.3%). hotpath_gate FAIL은 pristine HEAD에서도 동일(기존 실패). 보고서 작성.
- 10:47 완료. 보고서 core-rf-G-2-summary.md 작성. 커밋하지 않음.
- 주의: git stash 공유 스택 충돌로 G-1 워크트리에 내 msg 변경이 섞였을 수 있음(보고서 §9).
