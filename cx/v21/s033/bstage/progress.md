# Progress

- 시작: 지침 확인. 저장소 HEAD는 detached `168c679590`; 기존 미추적 `.psmeas-build/`는 사용자 자산으로 유지.
- 다음: harness/runner 차이 확인, base stage timing 1회, 22k A/B/A/B 측정.
- Stage harness: 첫 두 시도는 `BENCH_COLLECTOR` 지정/복사 의존 파일 문제로 stage trace가 생성되지 않아 raw를 보존하고 재실행. 유효 실행에서는 `perf` stage trace 1,701 events, PM probe 250 samples, 완료/수신 수 일치.
- 22k A/B/A/B 유효 실행 완료. 첫 B 시도는 supervisor 환경 미설정으로 measurement 전에 종료; 이후 올바른 psched8 outer supervisor로 B1/A2/B2 완료. 기존 analyzer 기준 app-worker 집계는 캡처에 동시에 존재하는 TID 수에 따라 변동하여, launch 방식 단독 원인 여부를 추가로 raw에서 확인 중.
- 유효 base stage trace 분석 완료: 243 matched requests, 1,701 events; chained raw와 같은 analyzer로 4개 span 산출. `bstage:*` perf probe 제거 확인.
- 22k raw 재분석 완료: 이전 두 세션의 app-worker 차이는 로그에 찍힌 3 TID 중 before/after 양쪽 snapshot에 존재한 수(기존 psmeas 2, psched8 1)에 비례. 새 A/B/A/B에서도 app-worker 집계가 A1=2, B1=2, A2=2, B2=1로 나와 launch 방식 단독 차이는 재현되지 않음.
- 완료 보고 전 raw captures, summaries, stage-summary 두 파일을 열어 수치를 교차 확인함.
- 최종 runner diff 확인에서 `runner-common.sh`의 기본 collector 경로와 종료 신호 차이도 확인. 이번 실행은 collector 경로를 명시했고 종료 신호 차이는 capture 종료 후라 결과 차이 원인으로 보지 않도록 보고서에 범위를 반영.
