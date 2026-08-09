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
