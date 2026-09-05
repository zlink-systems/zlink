2026-09-05 시작: detached @ main 확인, 기존 변경은 금지된 core/build·core/build-dev symlink만 존재; 별도 core/build-monid를 사용한다.
2026-09-05 조사: monitoring §3.1은 physical attempt identity를 규정하고 socket §zlink_connect는 inproc 포함 자동 재연결을 규정; CLOSED 발생 조건은 명시가 없어 inproc CLOSED는 spec gap 후보다.
2026-09-05 빌드: build-core.sh dev와 동일 옵션(RelWithDebInfo, LTO OFF, tests ON)으로 core/build-monid configure 완료.
2026-09-05 빌드 진행: core/build-monid 초기 전체 빌드 -j3 실행 중(46%); 금지된 symlink build tree는 사용하지 않았다.
2026-09-05 빌드 진행: 초기 전체 빌드 -j3 75%; Core와 기존 monitoring 테스트 바이너리 링크 완료, 나머지 테스트 빌드 중.
2026-09-05 빌드 완료: core/build-monid 초기 전체 빌드 -j3 성공. 원인 경로를 좁혀 inproc pipe 양쪽 endpoint metadata가 shared transport id와 어긋나고, TCP/IPC connecter가 한 connect attempt의 DELAYED/CLOSED에 매번 새 id를 발급함을 확인했다.
2026-09-05 테스트 추가: 공개 C API 신규 test_monitor_connection_identity(5 Unity cases), CTest TIMEOUT 30 및 integration;serial 자동 label 등록. 최초 실행은 lifecycle 대기에서 실패하여 이벤트 단계별 관찰 중.
2026-09-05 수정 진행: inproc 양 pipe endpoint에 같은 id를 배정하고 peer detach 시 self-command로 connect intent를 재등록하도록 구현; TCP/IPC connecter는 attempt endpoint identity를 DELAYED/CONNECTED/CLOSED에 재사용. 대상 재빌드 -j3 진행 중.
2026-09-05 재현 결과: TCP DELAYED/READY/DISCONNECTED id가 수정 후 동일(9), 재연결 READY는 새 id(17); inproc READY/DISCONNECTED 동일(34), 서버 재bind READY 새 id(37) 확인. 추가 lifecycle 종료/명시 disconnect 관찰의 테스트 정리 중.
2026-09-05 테스트 결과: 신규 8-case binary 0.51s green. tcp/ipc 명시 disconnect는 즉시 같은 id DISCONNECTED; inproc은 peer 공개 poll로 command progress 후 같은 id DISCONNECTED이며, peer 무호출 시 5s 무이벤트를 별도 blocker로 기록한다.
2026-09-05 반복 게이트: 신규 test_monitor_connection_identity 5/5 green(총 2.48s). 전체 테스트 재링크를 위한 core/build-monid -j3 빌드 진행 중(88%).
2026-09-05 게이트: 전체 재빌드 -j3 성공; monitor 이름 suite 13/13 green(9.86s). integration label -j2 실행 중 79/94 green.
2026-09-05 게이트: integration 94/94 green(162.85s). 전체 ctest -j2 실행 중이며 hotpath_gate만 감독자 별도 대상답게 기준 대비 1.2553~1.3167로 실패, 나머지 진행 중.
2026-09-05 게이트: 전체 ctest 144/145 green(196.64s), 유일 실패 hotpath_gate; 지시대로 hotpath_gate 제외 전체 ctest -j2를 별도 실행 중.
2026-09-05 client close 축을 peer ROUTER monitor의 공개 C API READY→DISCONNECTED identity 3종(tcp/ipc/inproc)으로 추가; 재빌드·반복 검증 시작.
2026-09-05 최종 재검증: 신규 11-case 5/5 green(3.37s), monitor 이름 suite 13/13 green(9.93s), integration 94/94 green(163.33s), git diff --check PASS.
2026-09-05 요약 완료: B 기존 Core 결함 수정, inproc CLOSED trigger는 spec gap, peer 무호출 command progress와 WS/TLS parity 및 hotpath_gate는 BLOCKERS/후속으로 기록.
EXIT:0
