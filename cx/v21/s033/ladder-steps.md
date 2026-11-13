# 사다리 단계 정의 (PM 11-13 18:05 확정, 네 언어 공통)

| 단계 | 더하는 것 (production 경로 그대로) | 송신 | 수신 | 응답 | completion |
|---|---|---|---|---|---|
| L0 | binding과 같은 raw 경로 + 고정 handler 직접 호출 | raw | raw, binding과 같은 수신 방식 | raw | raw |
| L1 | application job queue permit (수신 전 획득, handler 직전 반환) | — | permit | — | — |
| L2 | envelope header: 요청·응답 모두 | 요청 envelope encode | header parse 1회 | 응답 envelope encode | 응답 header parse 1회 |
| L3 | service wire multipart framing | frame pack | frame unpack | frame pack | frame unpack |
| L4 | typed payload codec | serialize | deserialize | serialize | deserialize |
| L5~L7 | owner queue·worker, handler 활성화·DI scope, operation registry·completion·deadline (나눌 수 없는 언어는 한 단계, 예산 21%) | operation 등록 | owner queue → worker → 활성화 | completion dispatcher | operation 완료 |
| Node L0R/L4R | 해당 단계 모듈을 require만 함(모듈 로드 비용 분리) | — | — | — | — |

| 언어 | L2 응답 envelope | L5~L7 |
|---|---|---|
| C++ | 있음 fbdcb16e26 | 묶음 62e766f88d |
| .NET | 있음 69891f4446 | 묶음 2b6da5af64 (L4 JSON 수정 뒤 다시 쌓음) |
| Java | 있음 e8721a0776 | 묶음 f3e3933567 |
| Node | 있음 2bef18aa94 | 묶음 7d1afa98a9 |
