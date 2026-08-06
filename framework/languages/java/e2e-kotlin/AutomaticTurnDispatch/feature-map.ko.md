# Kotlin AutomaticTurnDispatch feature map

Kotlin counterpart는 Java fixture와 별도로 현재 Java Framework public API를 기준으로
구성한다. 이번 migration에서 Delay, Shared, Client, Server/Play, Server/Session의
source compile을 현재 API에 맞췄다. Runtime scenario의 동작 검증 상태와 source compile
상태는 분리해 기록한다.

| 범위 | 상태 | 근거 또는 blocker |
|---|---|---|
| `Server/Delay`, `Shared`, `Client`, `Server/Play`, `Server/Session` | compile-verified | `:Client:compileKotlin`, `:Client:compileJava`, `:Shared:compileJava`, `:Server:Delay:compileJava`, `:Server:Play:compileJava`, `:Server:Session:compileJava`가 성공했다. |
| `Server/Play`, `Server/Session` join flow | runtime follow-up | 현재 join contract인 `joinSpot(...).defer()`와 `ZLinkActorJoinCompletion` lifecycle을 사용하도록 migration했다. 각 scenario의 실제 process-matrix 결과는 별도 E2E 실행으로 확인해야 한다. |

이 migration은 삭제된 request context, 구형 `spotRid()` context accessor, generic join result,
구형 actor factory/getOrCreate/channel 호출을 제거했다. Framework 내부 API나 reflection/raw
우회는 추가하지 않았다. `run_e2e.sh`의 주석에 남은 항목은 compile blocker가 아니라 아직
process-matrix runtime 검증이 필요한 scenario 목록이다.
