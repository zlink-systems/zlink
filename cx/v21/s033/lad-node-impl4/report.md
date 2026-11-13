# Node ladder L4 구현

측정은 실행하지 않았다. 실험 브랜치에 L4를 로컬 커밋으로 기록한다.

| 단계 | 복원 기능 및 production 경로 | Bench 코드 | 커밋 | 빌드·검증 | 호출 형태 및 재개 후 측정 |
|---|---|---|---|---|---|
| L4 (#4354) | 기본 typed JSON payload encode/decode. 송신 `channel-outbound-operations.ts:79-90,213-224` → `channel-envelope.ts:471-532`; 수신 `channel-dispatch-pipeline.ts:349-356` → `channel-envelope.ts:413-436`; reply encode/decode `channel-dispatchers.ts:243`, `channel-outbound-operations.ts:243-249` → `channel-envelope.ts:217-237,321-356`. | L4 client/server switch `ZLINK_LADDER_L4_CLIENT` / `ZLINK_LADDER_L4_SERVER`; `client/main.js:263-330`, `zlink-framework-server/main.js:98-128,150-164,250-269`, `shared/raw-wire.js:96-136`; production channel envelope의 기본 JSON 경로 사용. channel envelope 모듈은 L2부터 로드되므로 별도 L4R은 추가하지 않음. | `COMMIT` | `framework/bench/grpc/node/build.sh` 성공; `npm test` 21/21; `node --check` 3개 JS 성공; dist의 `channel-envelope.js`가 TS source보다 최신; L4 server dry start `/ready` 및 `/bench/stats` 응답, errors=0/received=0. 측정·measure.lock 미사용. | [L4 call-shape](call-shape-L4.md): source send·target receive·target reply·source completion의 의도된 차이는 L4 JSON payload codec이며, L1-L3는 이전 단계. 측정 재개 시 **L3 대 L4**, send-saturation/request-serial/request-backpressure 각 1K·4K의 6 cells. |
