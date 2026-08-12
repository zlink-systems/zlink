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

## Session relocation route와 target-only cutover

Spec 30과 Spec 20 §5는 모든 언어가 같은 단순한 경계를 사용하도록 요구한다. Session owner는
command 42/43으로 exact binding을 seal하고 이후 Session message를 보관한다. Source와 target의
message 순서는 같은 TCP connection의 relay와 one-way cutover가 제공하며 numeric high-water를
사용하지 않는다. Target만 Location Store CAS를 수행하고 queue를 연 뒤 command 44를 Session
owner에게 `[send]`로 전달한다. Command 44에는 reply가 없고 정상 경로에서 command 45를 사용하지
않는다. Exact update가 `SessionRelocationSealTimeout` 안에 없으면 Session owner가 physical
Session과 관련 state를 정리한다.

| 언어 | 현재 구현 차이 | 종결 조건 |
|---|---|---|
| C++ | 기존 command 45 ACK와 high-water 기반 route 적용을 제거하고 target one-way command 44 및 Session timeout 정리로 수렴해야 한다. | 미종결 |
| Node.js | 기존 aggregate의 high-water·route ACK 상태를 제거하고 ordered relay, target one-way command 44와 Session timeout 정리로 수렴해야 한다. | 미종결 |
| .NET | 기존 exact high-water·ACK 재전송 경로를 제거하고 target-only CAS 뒤 one-way command 44를 적용해야 한다. | 미종결 |
| Java/Kotlin | Command 42/43은 binding seal만 유지하고 high-water와 command 45 terminal을 제거해야 한다. Target one-way command 44와 Session timeout 정리가 필요하다. | 미종결 |

종결 조건은 네 언어 모두 같다. Command 42/43은 current binding seal만 설치하고, command 44는
target CAS와 queue 개방 뒤 one-way로 한 번 전달한다. Session owner는 exact Session, binding과
relocation identity만 확인하며 Store나 Actor authority를 다시 읽지 않는다. Timeout과 route
update를 직렬화하여 먼저 처리한 하나만 유효하게 만들고, late·duplicate update는 Warning만
기록한다. Command 45와 relocation 전용 high-water가 production, codec expectation과 contract
test의 정상 경로에서 모두 제거되어야 종결이다.
