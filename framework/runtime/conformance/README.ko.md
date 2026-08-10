# Framework runtime conformance fixture

이 디렉터리는 service wire와 무관한 Framework 내부 불변식을 언어 중립 JSON으로 고정한다.
각 언어 runtime은 같은 fixture를 직접 읽는 white-box test를 두며, 구현 언어의 thread,
executor, lock과 event-loop 선택은 fixture에 넣지 않는다.

`serial-execution-v1.json`은 owner별 serial execution의 admission, lane arbitration과
same-owner 호출 규칙을 정의한다. 수치나 trace를 바꾸려면 C++/.NET/JVM/Node 소비 test와
common internals를 같은 변경 단위에서 갱신해야 한다.

`runtime-observation-v1.json`은 subscriber별 source-latest intermediate, bounded terminal
FIFO, source key 수명과 서로 분리된 loss counter를 정의한다. Source 사이의 공개 sequence
비교를 만들지 않으며, fixture의 terminal 배열은 terminal FIFO 안의 순서만 고정한다.

`message-follow-suppression-v1.json`은 exact source·target route fence별 single-flight와
route lifetime에 묶인 suppression 상태를 정의한다. Payload, reply completion과 별도 timer는
이 registry의 책임에 넣지 않는다.

`completion-terminal-v1.json`은 128-bit `OperationId`, 별도 `ReplyRouteId`, pending table
상한과 reply·timeout·cancellation·close 사이의 단일 terminal winner를 정의한다.

`payload-ownership-v1.json`은 binding에서 handler까지의 소유권 전이, copy·deserialize
상한과 terminal 경로의 release 횟수를 정의한다.

`codec-selection-v1.json`은 선언된 송신 타입과 wire content-type을 서로 다른 입력으로
사용하는 codec 선택, 등록 우선순위와 bounded send-type cache를 정의한다. Codec 등록
content-type은 startup에서 parameter가 없는 ASCII media type으로 정규화하며, wire는 이미
정규화된 값을 사용해야 한다.

`runtime-state-v1.json`은 public 7-state 값, readiness authority, work admission과
maintenance·discovery bounded context로 보내는 단방향 projection을 정의한다.

검증 명령은 다음과 같다.

```bash
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
```
