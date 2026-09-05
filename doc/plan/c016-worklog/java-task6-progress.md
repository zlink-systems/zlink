2026-09-05 09:12: 조사 시작 — detached worktree 및 기존 Java 변경 3건 확인, 해당 변경 보존
2026-09-05 09:15: spec error/submit/completion 조항 확인 — public SubmitResult 5종과 async CompletionOwner 재분류 경계 조사
2026-09-05 09:16: 신규 4-case×2-transport 최초 실행 7/8 — TCP wait 제거가 NOT_CONNECTED, 양방향 route-ready 미확정 가능성 분리 중
2026-09-05 09:18: 양방향 ready 후 동일 실패(errno 113) 재현 — CompletionOwner retry와 disconnectRid 락 경쟁을 원인으로 특정
2026-09-05 09:21: disconnectRid 성공 시 matching Java pending state를 typed NOT_FOUND로 종결하도록 수정, targeted inproc+tcp 2/2 통과
2026-09-05 09:23: 전체 8-case 재실행에서 test blocking recv 정지 1회 — 무한 대기 제거를 위해 deadline DONT_WAIT receive로 보강
2026-09-05 09:26: 신규 계약 5회 반복 모두 PASS(회당 8 cases, 총 40), inproc/tcp 결과 안정화 확인
2026-09-05 09:28: 전체 gate 4 subsystem 중 integration만 기존 raw errno assertion 1건 실패; samples 7/7 포함 나머지 통과
2026-09-05 09:30: integration 재실행 close hang stack 확인 — 즉시 reject로 native completion 미drain; pending 표식 후 정상 drain 방식으로 변경
2026-09-05 09:33: 최종 구현으로 기존 terminal test PASS, 신규 8-case 5회 반복 총 40/40 PASS
2026-09-05 09:37: 최종 run_tests.sh green — JUnit 129(실패 0, error 0, skipped 3), samples 7/7, diff check PASS
2026-09-05 09:38: summary 작성 및 ./gradlew --stop 완료
EXIT:0
