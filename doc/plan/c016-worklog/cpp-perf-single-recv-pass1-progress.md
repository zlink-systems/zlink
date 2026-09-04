2026-09-05 07:17:25 +0900 START detached HEAD 확인, 범위/지침 조사 시작
2026-09-05 07:18:20 +0900 PROFILING 준비: C++/C PAIR tcp 64B 1초 바이너리 빌드·기준 실행 시작
2026-09-05 07:19:15 +0900 C runner가 timestamp stale 오판으로 core build 호출(ninja no-op) 후 실패; 직접 C benchmark 빌드 경로로 전환
2026-09-05 07:20:10 +0900 CALLGRIND C++ PAIR tcp 64B 1초 시작(separate threads)
2026-09-05 07:20:22 +0900 CALLGRIND C++ 완료; C 동일 조건 시작
2026-09-05 07:24:03 +0900 러너 PERF_PART_COUNT 1회 캐시 수정, cpp_perf_pair 재빌드 완료; after callgrind 시작
2026-09-05 07:24:55 +0900 AFTER 측정 시작: C++ PAIR,PUBSUB,DEALER_DEALER tcp,ws,inproc 6 sizes 5초 1run
2026-09-05 07:31:56 +0900 AFTER 측정 COMPLETE 54/54; report perf_cpp_single_linux_20260905_072455.txt
2026-09-05 07:32:13 +0900 GATE 전체 C++ contract + samples 시작(JOBS=4)
2026-09-05 07:32:47 +0900 GATE 전체 contract 16/16 + samples 7/7 PASS; 관련 optimization_guard 5회 시작
2026-09-05 07:32:56 +0900 GATE optimization_guard 직접 실행 5/5 PASS(ctest는 samples 재configure 뒤 등록 해제), git diff --check PASS
2026-09-05 07:34:41 +0900 SUMMARY 작성 및 최종 상태 확인 완료
2026-09-05 07:34:41 +0900 EXIT:0
