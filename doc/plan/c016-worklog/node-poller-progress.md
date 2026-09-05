2026-09-05T09:48:47+09:00 START detached @ main 확인; bindings/node 범위 조사 시작 (core build symlink 미접촉).
2026-09-05T09:51:00+09:00 DIAG Core polling §3/C/C++ 대조 완료; Node completionOwnerOf 강제 경로를 A 계약 적응으로 분리 예정.
2026-09-05T09:53:47+09:00 IMPLEMENT public Pollable/monitor overload와 source identity, runtime 등록 분리, sample poller drain, 회귀 테스트 작성.
2026-09-05T09:54:42+09:00 BUILD npm ci 후 공식 npm run build 통과; generated dist-tools 포함 상태 정상화.
2026-09-05T09:55:34+09:00 TEST native addon을 local Core 0.17.0으로 재빌드; 신규 monitor poller 3개 테스트 통과, source_layout 기대값 갱신 중.
2026-09-05T09:56:55+09:00 GATE 공식 npm test 전체 및 sample 7개 통과; 신규 테스트 5회 반복 통과; git diff --check 통과.
2026-09-05T09:57:53+09:00 REVIEW bindings/node 외 변경 없음(core build symlink 기존 상태); 요약 작성 완료. EXIT:0
