# Node.js RegistrationCodec E2E feature map

| Scenario | 상태 | 근거 |
|----------|------|------|
| RC-A1 | 구현 | provider discovery 기반 request/send marker를 검증한다. |
| RC-A2 | 구현 | Node decorator 기반 request/send marker를 검증한다. .NET method attribute와 언어별 표면은 다르다. |
| RC-A3 | 구현 | builder 명시 등록 request/send marker를 검증한다. |
| RC-A4 | 구현 | handler dispatch가 Nest context별 `resolve()`를 사용해 singleton id는 유지되고 scoped id는 request마다 달라지는지 검증한다. Node/Nest public `ModuleRef`는 dispatch 뒤 context dispose API를 제공하지 않으므로 dispose counter를 완료 조건에 넣지 않는다. 내부 wrapper 저장소 삭제나 테스트 전용 adapter를 쓰지 않는다. 최신 확인 로그: `logs/20260702-065333-61321` |
| RC-A5 | 구현 | `ZLinkHandlerFilter`를 구현한 두 Nest provider type을 public `filters` registration option에 등록하고 filter before/after 순서를 검증한다. |
| RC-A6 | 구현 | duplicate registration startup failure를 검증한다. |
| RC-B1 | 구현 | JSON codec content-type과 evidence를 검증한다. |
| RC-B2 | 구현 | Main role의 전역 codec registry에서 공통 content-type `application/x-protobuf`와 request/send/reply evidence를 검증한다. |
| RC-B3 | 구현 | Main role의 전역 codec registry에서 MessagePack content-type과 request/send/reply evidence를 검증한다. |
| RC-B4 | 구현 | serializer별 `canSerialize` public predicate로 한 host에 JSON/Protobuf/MessagePack serializer를 함께 등록하고 payload class에 맞는 content-type을 검증한다. |
| RC-B5 | 구현 | Protobuf requester가 JSON-only peer로 보낸 request가 거부되고, JSON request가 이후 정상 처리되는지 검증한다. |

검증 결과: `framework/languages/node/e2e/RegistrationCodec/run_e2e.sh` 실행에서 `RC-A1`, `RC-A2`,
`RC-A3`, `RC-A4`, `RC-A5`, `RC-A6`, `RC-B1`, `RC-B2`, `RC-B3`, `RC-B4`, `RC-B5`가 통과하고
`registration-codec e2e result=passed`를 출력했다. 최신 확인 로그 디렉터리는
`logs/20260702-065333-61321`이다.

## 후속 계약 판정

| Scenario | 판정 | 다음 작업 |
|----------|------|-----------|
| 후속 계약 | 구현 | `RC-A4`의 per-dispatch scope evidence는 public dispatch 경로로 통과했다. 현재 설치된 Nest public export는 `ContextIdFactory`, `ModuleRef.resolve(...)`, `registerRequestByContextId(...)`를 제공하지만, `ContextIdFactory.create()`로 만든 request context를 dispatch 뒤 명시 해제하는 API는 제공하지 않는다. 따라서 Node 완료 조건은 scoped id 분리와 singleton 안정성이다. Nest 내부 저장소 삭제나 테스트 전용 adapter는 쓰지 않는다. |
