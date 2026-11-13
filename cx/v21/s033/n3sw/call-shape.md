# Node L2/L3 프로파일 호출 대조

Scratch `20598f57a2`, `B=framework/bench/grpc/node`, `R=framework/languages/node/packages/framework/src/runtime`. 측정 worktree를 사용하지 않는다. 아래 실행은 6–7,14–15 CPU에서 최대 20초 프로파일이며 공식 throughput 판정이 아니다. 사용자 지시로 measure.lock을 사용하지 않는다.

| 단계 | binding 호출 | L2/L3 호출 | 동일 여부·복원 기능 |
|---|---|---|---|
| source payload | `B/client/main.js:257,295` protobuf serializer → native Message | 같은 함수 | 동일; source body를 재사용하지 않음 |
| source envelope | `B/client/main.js:261,298` 상수 envelope | `:259-264,296-300` production channel envelope; L3 service header + frame | L2 header, L3 framing |
| source submit | `B/client/main.js:267-268,306` 2-part submit | `:266-268,301-304` 2-part submit | 동일 builder·admitted/completion |
| target receive | `B/zlink-raw-server/main.js:47-53,64-75` DontWait/NoData + bounded loop | `B/zlink-framework-server/main.js:209-239` 같은 recv/NoData + bounded loop | L1 permit acquire가 추가; wait/drain 정책 동일 |
| target decode | `B/zlink-raw-server/main.js:78-81` body data → protobuf → metrics | `B/zlink-framework-server/main.js:244-258,268-275`; L2 channel header, L3 `B/shared/raw-wire.js:151-177` service header + multipart views | L2/L3 외 protobuf·metrics 동일 |
| target reply 생성 | `B/zlink-raw-server/main.js:82` 새 protobuf encode | `B/zlink-framework-server/main.js:109-120`; production reply serializer가 새 protobuf encode | L2 response envelope, L3 service reply pack; request parts 재사용 없음 |
| target reply submit | `B/zlink-raw-server/main.js:83-86` 2-part reply submit | `B/zlink-framework-server/main.js:120,123` 2-part reply submit | 동일 |
| source completion | `B/client/main.js:238-253,280-288` PollCompletion + protobuf + close | `:272-278` L3 reply header/correlation + multipart, L2 response header | 추가 worker·wait 없음 |

| 정상 message/leg 비용 | binding | L2 | L3 추가 |
|---|---|---|---|
| source encode | 새 protobuf/native Message; 기존 binding 전송 소유권 | 새 channel header | service header Buffer + application frame Buffer; 각 inner part를 frame에 1회 copy/write; frame Buffer를 native submit에 넘기면 binding copy 1회 |
| target receive | binding data()의 managed copy, protobuf decode, 같은 metrics | channel header 1회 parse; L1 permit | application bytes copy 0; intermediate multipart view 1 + final Buffer views 2 + wrapper 2 + 배열 1; profile object 1; service header parse |
| target reply | 새 body encode, 2-part reply | 새 envelope | source encode와 같은 framing; request 재사용 없음 |
| source completion | binding completion + body decode | response header 1회 parse | receive와 같은 service framing decode; correlation validation |

Framework production receive는 `R/backend/node/node-raw-mesh-backend.ts:2635`의 direct `decodeMultipartApplicationParts`를 사용하므로 bench보다 intermediate view/profile object가 각각 1개 적다. Production outbound는 `:2951-2958` → `R/foundation/service-wire-m6a-codec.ts:216-229`. Native submit/receive의 복사는 binding 비용으로 별도 계수한다.
