# gate s14-r10b 진행

- 2026-09-07 KST: main/origin-main 일치 확인. 기존 worklog 변경으로 pull --rebase는 불가했으나 core/bindings/scripts는 clean. S-14 및 R10-B-redo patch 적용과 게이트를 시작한다.
- 2026-09-07 KST +3분: 두 patch를 3-way clean 적용했고 ABI/export diff 없음. dev build(JOBS=4) 진행 중.
- 2026-09-07 KST +6분: dev build 성공. 전체 ctest -j2 실행 중이며 현재 35/208까지 모두 통과.
- 2026-09-07 KST +11분: 전체 ctest 완료: 207/208 통과. 유일한 실패 hotpath_gate는 5개 셀이 기준 대비 1.0742~1.2556으로 개선되어 gate의 상한(+5%)에 걸린 측정 FAIL; 기능 테스트 실패 없음.
- 2026-09-07 KST +14분: 지정 74-test suite의 5회 반복 진행 중. 1/5는 74/74 통과, 2/5는 실행 중.
- 2026-09-07 KST +17분: 지정 suite 3/5까지 각 74/74 통과. 4/5 실행 중.
- 2026-09-07 KST +25분: 74-test suite 5/5 및 stream|pipe 10/10 통과. S-14 solo repeat은 1 PASS 뒤 1 FAIL, 독립 재실행 3/3 PASS로 간헐 확인(30/30 실패). release, hotpath 5/5, with_stream 및 perf/c 3셀까지 완료; gate summary 작성.
