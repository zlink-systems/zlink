2026-09-05T11:15:29+09:00 시작: 상태·지침·C++ 개선 결과 조사
2026-09-05T11:16:56+09:00 조사: 즉시 SEND entry/Task/GCHandle 및 CloneParts, 러너 PERF_PART_COUNT 매회 조회 확인; GC 미니벤치 준비
2026-09-05T11:20:40+09:00 프로파일: DD binding 856 B/helper 992 B, REQ binding 781 B/runner 901 B; 공통 native 12 calls/2-part submit. 내부 clone wrapper·즉시 entry 제거 설계
2026-09-05T11:23:26+09:00 구현: noncancelable 즉시 SEND 지연 등록, 고유 context counter, native clone scratch, struct submitter; 러너 env cache 별도. 관련 gate 실행
2026-09-05T11:26:28+09:00 수신 비용 확인: 2-part recv native 임시 배열 매회 생성. 내부 ArrayPool 전환; 공개 wrapper identity 유지. 소유권 테스트 추가 및 관련 28건 통과 확인 중
2026-09-05T11:30:05+09:00 계측 종료: DD minibench 1320→184B/msg, REQ 1920→1024B/op; 임시 계측 제거. 전체 tests+samples gate 시작
2026-09-05T11:31:13+09:00 gate PASS: 전체 tests 208/208, samples 7/7, 관련 contracts 31건×5회. 공식 after 준비
2026-09-05T11:31:39+09:00 공식 after 시작: runtime=/home/hep7hep7/project/zlink-wt-dotnet-perf/core/build/lib/libzlink.so.0.17.0 load=0.43 0.89 10.05 2/431 2864139
2026-09-05T11:31:39+09:00 공식 after 측정 중 load=0.43 0.89 10.05 2/434 2864144
2026-09-05T11:32:09+09:00 공식 after 측정 중 load=1.89 1.19 9.86 7/552 2865711
2026-09-05T11:32:39+09:00 공식 after 측정 중 load=4.06 1.78 9.78 3/549 2866315
2026-09-05T11:33:09+09:00 공식 after 측정 중 load=3.10 1.78 9.52 12/540 2867028
2026-09-05T11:33:39+09:00 공식 after 측정 중 load=3.47 1.99 9.35 8/546 2867693
2026-09-05T11:34:09+09:00 공식 after 측정 중 load=3.69 2.20 9.18 8/524 2868555
2026-09-05T11:34:39+09:00 공식 after 종료 rc=0 load=2.94 2.17 8.95 2/456 2869622
2026-09-05T11:36:04+09:00 공식 after 20/20 complete, DD latency 86.49x C 미달. 클라이언트 분포 임시 진단 시작(공식 after에 합산 안 함)
2026-09-05T11:40:32+09:00 진단 완료: DD 64B 활성 1/100, 첫 client 2485161건·await 대기0; 1024B 첫 client24.58%. 임시 계측 제거, 최종 artifact 재빌드·요약 검증
2026-09-05T11:42:02+09:00 완료: summary 작성, after20/20, tests208/208, samples7/7, contracts31×5, diff-check PASS. 성능 미달·DD fairness·로그주기 초과는 summary BLOCKERS에 기록
EXIT:0
