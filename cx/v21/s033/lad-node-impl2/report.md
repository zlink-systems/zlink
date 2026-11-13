# Node ladder L2–L3 구현

측정·benchmark 실행은 하지 않았다. 각 단계는 지정된 실험 브랜치에 별도 로컬 커밋으로 남겼다.

| 단계 | 복원 기능과 production 경로 | Bench 변경과 커밋 | 검증 및 call-shape |
|---|---|---|---|
| L2 (#4352) | Channel envelope header encode: `channel-outbound-operations.ts:79-90,213-224` → `channel-envelope.ts:124-147,471-533,560-575`. Receive decode: `channel-dispatchers.ts:143-145` → `channel-envelope.ts:392-410`. | `client/main.js:256-268,289-303`, `zlink-framework-server/main.js:126-202`, `shared/raw-wire.js:50-88`. L2R 모듈 load 전용 스위치 추가. 커밋 `1708b92580244f126d6ac63d4d3bedd7055f9ad8`. | `build.sh` 통과; `npm test` 19/19 통과; 무부하 `/bench/stats` 응답 확인. `channel-envelope.js` dist가 TS source보다 최신. [L2 call-shape](call-shape-L2.md): source request/command header encode와 target header decode만 추가, body·reply·completion은 기존 경로 유지. |
| L3 (#4353) | Service header/frame encode: `service-wire-m6a-codec.ts:93-115,216-229`; decode/materialize: `:337-362,631-638`. Production 호출부 `node-raw-mesh-backend.ts:2632-2639,2951-2958`; reply header encode와 completion correlation 검증 `raw-service-mesh-runtime.ts:981-985,1851-1868`. | `client/main.js:256-303`, `zlink-framework-server/main.js:94-118,126-202,231-266`, `shared/raw-wire.js:92-171`. L3R service-wire load 전용 스위치 추가. 커밋 `e97dd9a5e4629410b29f4f8e74ab07c0c3f179a0`. | `build.sh` 통과; `npm test` 20/20 통과; L3 스위치로 무부하 `/bench/stats` 응답 확인. `service-wire-m6a-codec.js`와 `channel-envelope.js` dist가 TS source보다 최신. [L3 call-shape](call-shape-L3.md): inner L2 parts를 service frame으로 묶고 reply correlation을 검증; receive 방식·drain·metrics 유지. |

## 측정 재개 시

- L2: 이전 L1 대 신규 L2, 각 6셀 1회. `send-saturation`, `request-serial`, `request-backpressure` 각각 1024/4096 bytes.
- L3: 이전 L2 대 신규 L3, 같은 6셀 1회.
- binding 기준 및 측정 조건은 재개 시점의 공통 규칙에 따라 새로 확인한다.

주의: 첫 L2 무부하 readiness 시작은 concurrent 측정 hold를 확인하기 전에 실행되어 잠깐 겹쳤다(요청/부하는 전송하지 않음). 이를 인지한 뒤 build/test와 L3 readiness는 hold 종료 후 수행했다.
