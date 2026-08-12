# Framework runtime conformance fixture

이 디렉터리는 언어별 runtime이 함께 지켜야 하는 규칙을 언어 중립 JSON으로 고정한다.
내부 자료구조와 실행 규칙은 white-box test가 읽는 내부 불변식 fixture로 관리한다. 공개
spec의 동작은 application과 runtime port에서 관찰할 수 있는 사건, 결과와 partial order로
표현한 behavioral fixture로 관리한다. 어느 범주에도 구현 언어의 thread, executor, lock,
event-loop, private phase와 함수 이름을 넣지 않는다.

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

`relocation-behavior-v1.json`은 Actor와 Spot relocation에서 application과 runtime port가
관찰하는 lifecycle, traffic identity, terminal settlement를 정의한다. `actorJoin`,
`actorHostHandoff`, `perActorUserSpot`, `spotWideUserSpot`, `instanceSpot` profile은 공통 사건을
공유하면서 callback과 ownership 경계가 다른 부분만 분리한다. Adapter는 runtime의 private
phase를 trace로 바꾸지 않고 Ready authority, lifecycle callback, handler 실행, source cleanup,
authority completion과 route convergence처럼 실제 관찰한 결과를 제출해야 한다.
같은 ObjectGeneration으로 relocation이 연속해서 일어날 때는 이전 relocation의 늦은
terminal이 successor relocation의 보관 payload를 전달하거나 정리하지 않는지도 확인한다.
이 경계는 relocation ID, binding generation과 Session identity가 모두 일치할 때만 열린다.
Actor Join의 public terminal은 `publicJoinCompleted`, Host relocation terminal은
`relocationTerminalDelivered`, Session route terminal은 `sessionRouteTerminalDelivered`로 서로
구분한다. Bound Session이 있는 profile만 optional route branch를 관찰하며, 이 branch가 없어도
successful relocation의 공통 필수 사건을 생략할 수 없다.

`bound-session-relocation-v1.json`은 공통 relocation 동작 중 bound Session seal, 보관한 traffic,
Message Follow와 route convergence만 Actor capability로 projection한다. Wire command와 byte
encoding은 이 디렉터리가 아니라
`framework/runtime/protocol/golden/session-relocation-barrier-v1.json`이 소유한다.

`relocation-conformance-adapters-v1.json`은 C++, .NET, Java, Kotlin, Node adapter inventory와
focused test evidence를 관리한다. `direct`는 해당 언어 test가 behavioral fixture를 읽고 실제
runtime observation을 대조할 때만 사용한다. 기존 test와 fixture 항목을 연결한 상태는
`mapped`로 기록한다. 동작 일부만 실제로 관찰하면 `partial`, 아직 검증 주체를 정하지 못했으면
`pending`, process 경계가 있어 focused test로 확인할 수 없으면 `e2eRequired`로 기록한다. Evidence의
test identifier는 source에 존재하는 exact literal이어야 하며, focused command도 해당 source나 test를
실제로 선택해야 한다. `direct` adapter는 fixture를 읽는 test source와 그 source 안의 fixture
reference도 별도로 제출해야 한다. Focused command에는 sample이나 E2E runner를 넣지 않는다.

`relocation-e2e-scenarios-v1.json`은 실제 Redis CAS, process 종료와 재시작, physical STREAM,
이전 node를 거치는 Message Follow와 route ACK 유실처럼 process 내부 deterministic test로
확인할 수 없는 항목을 분리한다. Focused conformance runner는 이 inventory의 구조만 검증하며
E2E를 실행하거나 통과한 것으로 처리하지 않는다. 실행 상태는 언어 하나의 통합 상태가 아니라
5개 언어와 각 scenario의 matrix cell마다 `notRun`, `pass`, `fail`, `deferred`로 기록한다.

검증 명령은 다음과 같다.

```bash
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
```

Relocation adapter inventory와 focused test는 repository root에서 다음과 같이 검증한다. 인자가
없으면 다섯 언어의 등록된 focused test를 순서대로 실행한다. 한 언어만 실행하려면 `--language`를,
현재 adapter 상태만 확인하려면 `--list`를 사용한다.

```bash
node scripts/run-framework-relocation-conformance.mjs --list
node scripts/run-framework-relocation-conformance.mjs
node scripts/run-framework-relocation-conformance.mjs --language node
node scripts/run-framework-relocation-conformance.mjs --require-complete
```

인자가 없는 실행과 `--language`는 `mapped` adapter의 기존 focused evidence도 실행할 수 있다. 이 성공은 현재
mapping의 test가 통과했다는 뜻이며 complete conformance를 뜻하지 않는다. 각 focused command는
10분 안에 끝나야 한다. 시간 초과가 발생하면 runner는 process group에 종료를 요청하고 짧은
유예 시간 뒤에도 남은 process tree를 강제 종료한다. `--require-complete`는 먼저 모든
언어가 fixture를 직접 소비하고 모든 profile, policy, failure와 idempotency group을 actual runtime
observation으로 검증하는지 확인한다. 하나라도 빠지면 test를 실행하지 않고 실패한다. 모두
`direct/covered`이면 5개 언어의 focused command를 순서대로 실제 실행한 뒤에만 성공한다. E2E
inventory는 이 조건과 별도로 유지한다.
