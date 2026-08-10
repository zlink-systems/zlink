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
| Java/Kotlin | 없음. Command 42가 Session owner의 accepted high-water를 고정하는 시점에 ingress barrier도 함께 설치한다. Command 43은 그 값을 source에 반환한다. 봉인 뒤 ingress는 이전 route에서 실행되지 않고, command 44 또는 45가 종결될 때까지 순서를 유지한 채 보관된다. Command 44는 relocation id에 연결된 seal 값과 정확히 같은 high-water만 적용한다. 재시작 복구도 durable root에 보관된 exact seal과 route를 사용하며 monotonic 비교로 대체하지 않는다. | 종결 |

Java/Kotlin도 ObjectGeneration·AuthorityOwnerGeneration·binding generation과 exact
high-water를 함께 검증한다. 대응하는 seal이 없는 command 44는 route를 변경하지 않고
`stale` 또는 `sessionOrBindingClosed`와 high-water 0을 반환한다. 재시작 뒤 exact seal을
복원할 수 없으면 relocation을 명시적으로 실패시키며, 다른 high-water 값으로 추정하지 않는다.

Command 45는 `session-relocation-route-result`(applied / alreadyApplied / stale /
sessionOrBindingClosed) 필드를 싣는다. Spec 20 §5는 target이 이 네 결과 중 하나를 받으면
재전송을 멈추도록 요구하므로, 거부된 command 44도 무응답으로 두지 않고 거부 사유를 담아
답한다. C++와 Java/Kotlin 모두 이 필드를 encode/decode하며 refusal 경로에서 `stale` 또는
`sessionOrBindingClosed`를 답한다.
