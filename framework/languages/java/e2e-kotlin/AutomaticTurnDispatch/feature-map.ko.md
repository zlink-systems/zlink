# Kotlin AutomaticTurnDispatch feature map

Kotlin counterpart는 Java fixture와 별도로 현재 Java Framework public API를 기준으로
구성한다. 이번 migration에서 Delay, Shared, Client, Server/Play, Server/Session의
source compile을 현재 API에 맞췄다. Runtime scenario의 동작 검증 상태와 source compile
상태는 분리해 기록한다.

| 범위 | 상태 | 근거 또는 blocker |
|---|---|---|
| `Server/Delay`, `Shared`, `Client`, `Server/Play`, `Server/Session` | compile-verified | `:Client:compileKotlin`, `:Client:compileJava`, `:Shared:compileJava`, `:Server:Delay:compileJava`, `:Server:Play:compileJava`, `:Server:Session:compileJava`가 성공했다. |
| `Server/Play`, `Server/Session` join flow | process-verified | 현재 join contract인 `joinSpot(...).defer()`와 `ZLinkActorJoinCompletion` lifecycle을 사용한다. `bash run_e2e.sh all`에서 ATD-B1/B3 actor 경로와 ATD-E3 재시작 경로를 포함한 전체 ATD process aggregate가 성공했다. |
| AutomaticTurnDispatch process scenarios | process-verified | `bash run_e2e.sh all`이 `STATUS=0`으로 완료됐다. ATD-A1..A4, B1..B3, C1..C3, D1..D4, E1..E3가 실제 Delay/Play/Session 프로세스와 stream connector를 사용해 통과했다. 최신 증거는 `logs/20260807-205559-2682286/`에 있다. |
| common execution-turn matrix outside this fixture | gap remains | `run_e2e.sh`의 inventory에 기록한 TD-A3, TD-A5, TD-B3, TD-B4, TD-C1..C5, TD-D1..D6, TD-E1..E2A, TD-F1..F6, TD-G1은 이 Kotlin fixture의 public surface에 아직 없다. ATD aggregate 성공을 이 전체 공통 matrix 완료로 해석하지 않는다. |

이 migration은 삭제된 request context, 구형 `spotRid()` context accessor, generic join result,
구형 actor factory/getOrCreate/channel 호출을 제거했다. Framework 내부 API나 reflection/raw
우회는 추가하지 않았다. `run_e2e.sh`의 주석에 남은 항목은 compile blocker가 아니라 아직
process-matrix runtime 검증이 필요한 scenario 목록이다.
