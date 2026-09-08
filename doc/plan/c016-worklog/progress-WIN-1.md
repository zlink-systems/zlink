# WIN-1 진행

- 09-08 시작. CI workflow 확인 완료. Windows 체크아웃 상태 확인 시작.
- 12:5x: D:\project\zlink 신규 클론(origin/main b5cebd30d2), CI와 동일 cmake configure 성공. 빌드 시작.
- 실패 지점: test_zmp_metadata.cpp:720 = assert_incomplete_pair_lane_hits_fence 내부 첫 wait_for_transport_pair_admission (모니터 STATE_READY 5s 폴링).
- 13:2x: **Windows에서 1회차 즉시 재현 성공** (test_zmp_metadata.cpp:720 FAIL). 원인 분석 진행 중 — pair admission(monitor READY) 미발생.
- 13:5x: **근본원인 확정**. 계측 로그로 pair id가 두 번 연속 동일값(27206470336553/1)으로 생성됨.
  core/src/runtime/utils/random.cpp: Windows에는 getrandom//dev/urandom 경로가 #if로 제외되어 rand() 폴백만 남음.
  MSVC rand()는 15비트 + 스레드별 시드(스레드마다 seed=1) → I/O 스레드마다 같은 수열 → transport pair id 충돌.
  충돌한 id 때문에 stale lane의 늦은 attach가 새 pair의 lane 0을 점유충돌시켜 reject → pair 미admission.
- 수정 방향: Windows에 RtlGenRandom(advapi32 SystemFunction036) 엔트로피 경로 추가.
- 완료. 수정 2건(random.cpp Windows 엔트로피, ctx_termination.cpp EAGAIN 재대기).
  Windows ctest 10회 100% 통과, Linux 23/23 x3 통과. 보고서 core-rf-WIN-1-report.md 작성.
