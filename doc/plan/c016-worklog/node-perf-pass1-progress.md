2026-09-05T14:01:56+0900 시작: Node pass1 경로/규칙 조사
2026-09-05T14:02:58+0900 분석: REQREP 직렬 settlement 없음, uv_poll completion owner 확인; baseline profile 준비
2026-09-05T14:04:38+0900 baseline CPU/heap profile 완료(DD/DR 64B,8clients,1s); native submit/recv 지배 확인
2026-09-05T14:05:55+0900 native 상세 프로파일: perf 미설치, V8 --prof와 N-API 호출 계수/GC 포함 allocation sampling 사용
2026-09-05T14:07:30+0900 REQREP 25160요청/82815 FD wake 관측; DD epoll_ctl 호출 스택 조사
2026-09-05T14:14:28+0900 후보 구현: DD inline receive, routed multipart native frame, SEND inline-success singleton; 관련 gate 실행
2026-09-05T14:16:52+0900 후보 mini 비교 진행, SUB receive도 기존 inline storage로 통일; 중복 vector collect helper 제거
2026-09-05T14:17:39+0900 mini에서 routed frame 큰 payload 회귀 신호; CPU시간 포함 교대 확인 중, 정식 측정 아님
2026-09-05T14:19:12+0900 Buffer multipart 정상 경로 변환배열/진단문자열 할당 제거, public signature 유지
2026-09-05T14:22:17+0900 신규 17-part/1MB 테스트 첫 실패 격리: 변경 전 addon 대조 실행
2026-09-05T14:23:28+0900 테스트 원인: async 17-part 최초 BACKPRESSURED, blocking recv가 JS 재제출 막음; public PollCompletion+POLLIN 진행으로 테스트 호출 경로 교정
2026-09-05T14:25:46+0900 범위외 Core 기존 oversize REQUEST 실패: 17parts 1,048,696B > 적용 HWM1MiB, baseline 재현 보존(reports/node-oversize-request-repro.test.ts); 성능 후보 gate와 분리
2026-09-05T14:26:18+0900 정정: oversize 재현은 Core 결함 아님. Core auto-HWM spec admission: 중간 MORE는 HWM 초과 허용 안 함. 신규 테스트의 잘못된 수락 가정 교정(17part count/64KiB body 유지, 중간MORE 총량 HWM내); 기존 assertion/fixture 변경 없음
2026-09-05T14:28:46+0900 routed multipart native-frame 후보 기각:64B REQREP profile 25.3k→15.5k, 재계측30.9k→13.0k; copy 감소보다 native boundary/finalizer 증가. 원래 경로 복귀, 러너 변경 없음
2026-09-05T14:30:17+0900 전체 npm test PASS samples7/7; measure_chain.done 확인; 최종 관련5회 및 src-review gate
2026-09-05T14:30:40+0900 공식 after 시작: done 존재 및 1분load<=3 검증, 4patterns/5sizes/100clients/5s/1run
2026-09-05T14:31:07+0900 공식 after 진행 중, before/C paired 원본 reports 복사; optional src-review 기존 unused field TS6133 분리(필수typecheck/source_layout PASS)
2026-09-05T14:32:41+0900 after 진행, public d.ts 31파일 byte-identical 확인
2026-09-05T14:34:02+0900 공식 after REQREP/PUBSUB 진행, npmtest126개/26파일 및 samples7/7 PASS, 관련24개×5회 PASS
2026-09-05T14:34:29+0900 공식 after COMPLETE20/20, report143333 복사 및 Core SHA256불변 검증
2026-09-05T14:37:30+0900 after aggregate: DD41.20 DR20.31 RR18.05 PUB31.03%; 최종 계수/GC 포함 allocation 자료 보완(공식수치와 분리)
2026-09-05T14:45:31+0900 요약 작성 완료, 최종 파일범위/수치/잔여실패 대조 중
2026-09-05T14:48:58+0900 공유 Core mtime14:22 감지(after 이전/before 이후). paired 비교의 동일artifact 여부 확인 요청; 효과 확정 유보
2026-09-05T14:50:34+0900 before 기록은 이전 Release build 명시. 현재Core paired 재확인 가능성 조사(기존 C binary reuse만, Core build 금지 유지)
