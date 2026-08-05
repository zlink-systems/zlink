# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging` 문서의 Java 구현 위치다.

| 공통 항목 | Java 구현 |
|-----------|-----------|
| TA-A1 bind된 actor send/request | session-a의 실제 stream connection에 actor를 bind한다. 같은 connection에서 `Before`·`After` push를 받고, 그 사이에 외부 caller의 send/request와 actor handler evidence를 확인한다. |
| TA-A2 bind 안 된 actor send/request | 어느 session에도 bind하지 않은 actor의 send/request는 성공하고 bound-session push는 `REQUEST_FAILED`로 끝나는지 확인한다. 두 gateway의 evidence에도 해당 actor bind가 없어야 한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | bind 전 send/request 성공과 push 실패를 확인한 뒤 session-b에 bind한다. 이후 send/request와 `LateBind` push가 모두 성공하는지 확인한다. |
| TA-A4 unbind/disconnect 후 | session-a에 bind해 push를 확인한 다음 공개 bound-session API로 정상 unbind한다. actor가 유지되는 동안 send/request가 성공하고 push는 실패해야 한다. 명시적 destroy 뒤 같은 actor 호출은 `ACTOR_ROUTE_NOT_FOUND`이며 handler evidence가 추가되지 않아야 한다. |
| TA-B1 row 없음 | public `ActorRef` request가 framework의 `ACTOR_ROUTE_NOT_FOUND`로 실패하고 actor handler evidence가 추가되지 않는지 확인한다. one-way send는 local submit 의미를 유지한다. |
| TA-B2 stale location | actor 서버가 public `ActorRef` 값을 wire DTO로 넘기고, caller 서버의 `/request-ref`가 같은 public ref 호출 경로에서 generation을 바꾼 stale ref를 호출해 `ACTOR_LOCATION_STALE`을 확인한다. 이후 actor id 기반 재조회 호출이 성공하는지도 확인한다. |
| TA-B3 route not connected | caller 서버의 `/request-ref`가 존재하지 않는 node RID를 가진 ref를 호출해 `ROUTE_NOT_CONNECTED`를 확인한다. 이후 정상 actor id 기반 호출이 성공하는지도 확인한다. |

`run_e2e.sh`는 Redis, actor owner 서버, caller 서버, session gateway 두 개와 client runner를 모두
실행한다. 각 gateway는 actor owner와 분리된 프로세스에서 MeshNode endpoint와 stream endpoint를
사용한다. 따라서 원격 actor bind와 호출은 같은 MeshName의 MeshNode와 location store
발견만으로 연결된다. Runner는 실행마다 전용 Docker Redis container를 만들며, container를
만들지 못하면 즉시 실행을 실패로 종료한다.

client runner는 caller 응답만 보지 않고 actor 서버의 `/evidence`도 읽는다. 이 확인은 성공 scenario가
실제 actor handler까지 도달했는지, 그리고 TA-B1 missing actor 호출이 actor handler에 도달하지 않았는지
같이 검증한다.

최근 검증: `./run_e2e.sh TA-A1`부터 `TA-A4`까지 각각 실행해
`logs/20260715-030607-1313313`, `logs/20260715-030622-1314553`,
`logs/20260715-030635-1315721`, `logs/20260715-030651-1317573`에서
`to-actor-messaging e2e result=passed`를 확인했다. Track B 회귀는
`logs/20260715-030710-1319569`와 `logs/20260715-030725-1320790`에서 TA-B2·TA-B3가 통과했다.
