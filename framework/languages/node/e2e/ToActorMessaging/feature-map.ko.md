# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging` 문서의 Node 구현 위치다.

| 공통 ID | 상태 | Node 구현 | Runner 증거 |
|---------|------|-----------|-------------|
| TA-A1 | implemented | `Client/main.ts`의 `runTaA1()`이 session stream으로 actor를 bind하고 stream relay push를 확인한 뒤, caller 서버의 `/send`, `/request`로 no-bind 전달이 기존 session bind를 오염시키지 않는지 검증한다. | `run_e2e.sh TA-A1` 또는 전체 실행에서 `scenario TA-A1 passed`와 `to-actor-messaging e2e result=passed`를 출력한다. |
| TA-A2 | implemented | `runTaA2()`가 session binding 없이 actor ref 기반 send/request를 검증한다. | `run_e2e.sh TA-A2` 또는 전체 실행에서 `scenario TA-A2 passed`를 출력한다. |
| TA-A3 | implemented | `runTaA3()`가 session bind 전 no-bind send/request를 먼저 확인하고, 이후 stream bind와 relay push가 같은 actor에서 성공하는지 확인한다. | `run_e2e.sh TA-A3` 또는 전체 실행에서 `scenario TA-A3 passed`를 출력한다. |
| TA-A4 | implemented | `runTaA4()`가 stream bind를 닫은 뒤 actor row가 유지되어 no-bind send/request가 성공하는지 확인한다. | `run_e2e.sh TA-A4` 또는 전체 실행에서 `scenario TA-A4 passed`를 출력한다. |
| TA-B1 | implemented | `runTaB1()`이 없는 actor에 대한 send의 local submit을 확인한 뒤 handler·location evidence가 없음을 검증하고, request는 `actorRouteNotFound`로 실패하는지 확인한다. | `run_e2e.sh TA-B1` 또는 전체 실행에서 `scenario TA-B1 passed`를 출력한다. |
| TA-B2 | implemented | 실제 Actor를 destroy한 뒤 같은 id로 다시 만들어 `ObjectGeneration`을 바꾼다. 이전 `ActorRef`의 send/request가 handler에 도달하지 않고 request는 `actorLocationStale`로 실패하며, 새 `ActorRef`의 request는 성공한다. | `run_e2e.sh TA-B2` 또는 전체 실행에서 `scenario TA-B2 passed`를 출력한다. |
| TA-B3 | implemented | runner가 실제 owner MeshNode descriptor를 제거해 route를 끊고 다시 게시한다. 같은 actor ref로 단절 중 `routeNotConnected`와 handler evidence 부재를 확인하고, 복구 뒤 request 성공을 확인한다. | `run_e2e.sh TA-B3` 또는 전체 실행에서 `scenario TA-B3 passed`를 출력한다. |

`run_e2e.sh`는 Redis, actor owner 서버, session stream 서버, caller 서버, client runner를 모두 시작한다.
서버 역할은 `E2E_START_ORDER=reverse`와 고정 seed `shuffle:20260715`로도 시작할 수 있으며, 두
변형의 `TA-A1` runner가 통과했다.
인자를 주지 않으면 전체 scenario를 실행하고, `TA-B2`처럼 공통 ID를 첫 인자로 주면 해당 scenario만
실행한다. location store는 실행마다 Docker로 전용 Redis container를 만들며 다른 실행이나 host의
Redis 인스턴스를 공유하지 않는다.
