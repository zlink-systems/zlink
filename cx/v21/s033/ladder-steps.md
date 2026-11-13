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
| C++ | 있음 fbdcb16e26 | 미구현 |
| .NET | 추가 중 lad-dotnet-l2r | 묶음 de0507ae48 (재적재 예정) |
| Java | L5~L7 job 뒤에 추가 | 묶음 구현 중 lad-java-impl5 |
| Node | 추가 중 lad-node-l2r | 묶음 3c50a02908 (재적재 예정) |
