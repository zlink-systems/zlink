# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging` 문서의 Java 구현 위치다.

| 공통 항목 | Java 구현 |
|-----------|-----------|
| TA-A1 bind된 actor send/request | session-a의 실제 stream connection에 actor를 bind한다. 같은 connection에서 `Before`·`After` push를 받고, 그 사이에 외부 caller의 send/request와 actor handler evidence를 확인한다. |
| TA-A2 bind 안 된 actor send/request | 어느 session에도 bind하지 않은 actor의 send/request는 성공하고 bound-session push는 현재 Java public error kind `NOT_CONFIGURED`로 거부되는지 확인한다. 두 gateway의 evidence에도 해당 actor bind가 없어야 한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | bind 전 send/request 성공과 push 실패를 확인한 뒤 session-b에 bind한다. 이후 send/request와 `LateBind` push가 모두 성공하는지 확인한다. |
| TA-A4 unbind/disconnect 후 | session-a에 bind해 push를 확인한 다음 공개 bound-session API로 정상 unbind한다. actor가 유지되는 동안 send/request가 성공하고 push는 실패해야 한다. 명시적 destroy 뒤 같은 actor 호출은 현재 Java public error kind `NOT_FOUND`이며 handler evidence가 추가되지 않아야 한다. |
| TA-B1 row 없음 | 한 번도 생성하지 않은 ActorId에 public ID-only send/request를 보내 현재 Java public error kind `NOT_FOUND`를 확인하고 actor handler evidence가 추가되지 않는지 확인한다. |
| TA-B2 stale location | Actor node의 public `ZLinkActorManager.destroy(ActorRef)`로 첫 incarnation을 제거하고 같은 ActorId로 새 incarnation을 만든다. Caller의 ID-only send/request가 새 Actor evidence에 기록되는지 확인한 뒤 보관한 이전 `ActorRef` destroy가 `INVALID_OPERATION`으로 거부되고 새 Actor가 계속 request를 처리하는지 검증한다. |
| TA-B3 route not connected | 별도 actor-b owner와 caller의 public route status를 사용한다. Runner가 actor-b advertised endpoint 앞의 TCP proxy를 중단해 실제 route를 차단하고, caller request가 `UNAVAILABLE`이며 actor evidence가 추가되지 않는지 확인한 뒤 proxy를 복구하고 같은 Actor의 새 request가 처리되는지 검증한다. |

`run_e2e.sh`는 Redis, actor owner 서버, caller 서버, session gateway 두 개와 client runner를 모두
실행한다. 각 gateway는 actor owner와 분리된 프로세스에서 MeshNode endpoint와 stream endpoint를
사용한다. 따라서 원격 actor bind와 호출은 같은 MeshName의 MeshNode와 location store
발견만으로 연결된다. Runner는 실행마다 전용 Docker Redis container를 만들며, container를
만들지 못하면 즉시 실행을 실패로 종료한다.

client runner는 caller 응답만 보지 않고 actor 서버의 `/evidence`도 읽는다. 이 확인은 성공 scenario가
실제 actor handler까지 도달했는지, 그리고 TA-B1 missing actor 호출이 actor handler에 도달하지 않았는지
같이 검증한다.

최근 검증: `bash -n run_e2e.sh`와 Gradle `installDist`가 통과했다.
`bash run_e2e.sh TA-A1`은 `logs/20260806-025516-3831180`에서 통과했다.
`bash run_e2e.sh TA-B2`는 `logs/20260806-033426-279679`에서 통과했고,
`bash run_e2e.sh TA-B3`는 `logs/20260806-034745-543597`에서 route 단절 중
`UNAVAILABLE`, 복구 뒤 request 처리와 actor evidence를 확인했다.
