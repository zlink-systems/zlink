# Framework 구현 차이

이 문서는 정식 Framework 계약과 현재 언어별 구현의 차이를 기록한다. 여기에 적힌 구현 상태는 공개 계약을
축소하지 않는다. 공개 동작은 각 공통 spec과 언어별 exact interface가 정의한다.

## Session Actor binding 교체

같은 Actor를 새 session에 bind하면 새 binding이 즉시 current가 되고, 이전 session의 ACK·callback·close를
기다리지 않는다. Framework는 이전 exact session에 command 51을 one-way로 보낸다. 이전 session은 closing으로
전이한 뒤 application callback을 실행한다. Callback terminal에서는 non-blocking timer를 예약하고 turn을 즉시
반환하며, exact retired identity를 다시 확인한 뒤 100 ms에 connection을 닫는다. `sleep`, blocking wait 또는
session serial lane·worker 점유로 지연 시간을 만들지 않는다.

| 언어 | 현재 구현 차이 | 종결 조건 |
|---|---|---|
| C++ | 없음. Expected binding generation을 포함한 정식 command 36/38, one-way command 51, public callback과 non-blocking 100 ms timer를 구현한다. | 종결 |
| Node.js | 없음. 새 binding은 ACK를 기다리지 않고 current가 되며 rollback하지 않는다. One-way command 51, public callback과 non-blocking 100 ms timer를 구현한다. | 종결 |
| .NET | 차이 없음 — 새 binding을 먼저 current로 publish하고 command 51 one-way 통지, public callback, exact retired fence와 non-blocking 100 ms timer를 구현한다. | 종결 — command 51 canonical schema·golden, owner regression, package와 process 검증 통과 |
| Java/Kotlin | 없음. Java callback과 Kotlin suspending bridge, one-way command 51, non-blocking 100 ms timer를 구현하며 bind는 이전 cleanup을 기다리지 않는다. | 종결 |

모든 언어는 `runtime/protocol/golden/bound-session-replaced-v1.json`의 정상·malformed bytes와 재시작 전
session owner lifecycle 거부를 같은 결과로 검증해야 한다. 같은 physical session의 idempotent bind는 자신에게
교체 통지를 보내거나 connection을 닫지 않아야 한다.

## Session relocation route high-water 검증

Spec 20 §5 7단계는 session owner가 command 44의 ObjectGeneration, 이전·target
AuthorityOwnerGeneration, binding generation, session owner lease와 함께 high-water가 현재
binding에 기록된 값과 **같은지** 확인하도록 요구한다. 이 등식은 `sessionRelocationSeal(42)` /
`sessionRelocationSealed(43)` 핸드셰이크가 source가 포착한 high-water와 owner가 기록한
high-water를 같은 값으로 고정해 주기 때문에 성립한다. Owner 변경 뒤 이전 route로 도착한
message의 전달은 이 검증이 아니라 Message Follow가 보장한다.

| 언어 | 현재 구현 차이 | 종결 조건 |
|---|---|---|
| C++ | 없음. Command 42/43을 service wire로 주고받는다. | 종결 |
| Node.js | 없음. Seal 의미를 in-process binding registry로 구현하며 route publish를 seal과 대응시킨다. | 종결 |
| .NET | 없음. Seal 의미를 내부 relay로 구현하고 commit 시 exact fence와 idempotent 재수신을 구분한다. | 종결 |
| Java/Kotlin | Source 쪽 seal은 구현한다 — relocation barrier가 실행 lane을 봉인하고 그 시점의 accepted session sequence를 기록해 relocation에 싣는다. 빠진 것은 session owner 쪽 seal(command 42/43)과 seal 뒤 도착한 message의 ingress hold다. Owner는 봉인 시점을 통보받지 못하므로 비교할 기준값이 없고, 그래서 high-water gate는 등식이 아니라 monotonic으로 둔다 — 이미 적용된 값보다 역행한 high-water는 superseded relocation으로 거부한다. Seal 뒤 이전 route로 도착한 message는 Message Follow가 target Actor queue로 전달한다. | Command 42/43과 ingress hold를 구현해 owner가 봉인 경계를 기록하게 하고 등식 비교로 전환 |

Java/Kotlin의 monotonic gate는 이 항목 앞의 ObjectGeneration·AuthorityOwnerGeneration·binding
generation 검증을 모두 통과한 요청에만 적용된다. 지연 도착한 이전 relocation의 route 요청은
그 fence들과 monotonic gate에서 거부된다.
