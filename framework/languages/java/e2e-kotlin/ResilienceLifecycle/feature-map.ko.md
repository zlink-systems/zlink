# Kotlin ResilienceLifecycle E2E feature map

이 문서는 Config 5 Resilience/Lifecycle 공통 시나리오 중 Kotlin E2E가 현재 검증하는 항목과,
public API 또는 harness 제어가 더 필요한 항목을 구분한다. Client process는 HTTP/process-control
driver이며 framework runtime에 참여하지 않는다. Consumer role은 public Spring starter, `ZLinkClient`,
Redis location store와 public MeshNode runtime snapshot을 사용한다. Provider role은
`ZLinkChannelRuntimeOptions.clientServerChannel(channelName).weight(...)`로 runtime weight를 변경하고,
admin/evidence endpoint로 결과를 노출한다.

현재 runner는 `Shared`, `Client`, `Server/Provider`, `Server/Consumer` Gradle
project에서 만든 role별 binary를 시작한다. client scenario는 공통 scenario ID별 Kotlin file로
분리했다. Consumer role application/HTTP endpoint, Provider role application,
evidence/admin endpoint, state, dispatch-error observer, request/send handler를 Kotlin code path로
옮겼다. Shared message type도 Kotlin source다. location discovery는 Redis location store가 담당한다.

## 현재 상태

- `RL-A1` (차단): 같은 client process의 종료·재시작 흐름은 있지만 shared Java runtime이 down 구간
  오류를 정식 `RouteNotConnected`로 정규화하지 못하고 `ConnectionReady` identity도 확인하지 못한다.
  runtime 수정 뒤 terminal `Drained`, row 제거와 새 generation을 함께 검증해야 한다.
- `RL-A2`: provider-a를 같은 routing id의 다른 endpoint로 재기동하고, 같은 client 프로세스가 MeshNode descriptor의 endpoint 갱신과 follow-up request 성공을 확인한다.
- `RL-A3`: 동시에 여러 client 프로세스를 두 차례 띄워 server에 재접속 폭주를 만들고, 각 client의 public request가 정상 reply를 받는지 확인한다.
- `RL-A4`: 같은 routing id의 provider-b를 drain한 뒤 기존 프로세스를 내리고 green endpoint를 같은 routing id로 올린다. client가 public topology에서 green endpoint를 관측하고 green provider evidence까지 확인한 뒤, green을 내리고 원래 endpoint로 복구되는지 검증한다. Java Redis location store는 같은 routing id의 동시 blue/green claim을 거절하므로, 이 scenario는 공통 문서가 허용하는 rolling 전환으로 닫는다.
- `RL-A5`: provider-a가 짧은 간격으로 down/up을 반복하는 동안 같은 client 프로세스가 지속 request를 보내고, 정상 provider-b로 수렴해 timeout 없이 follow-up까지 성공하는지 확인한다.
- `RL-B1`: 처리 중인 request를 client timeout으로 끝낸 뒤 같은 client의 후속 request가 정상 reply를 받아 late reply가 pending을 오염시키지 않는지 확인한다.
- `RL-B2`: provider A를 drain해 slow request를 provider B에 고정하고, provider B가 처리 중이라는 evidence를 확인한 뒤 runner가 provider B를 `SIGKILL`한다. pending request가 public failure로 끝나고, provider A follow-up이 정상 동작하며, provider B 재시작 뒤 다시 traffic을 받는지 확인한다.
- `RL-B3`: provider 정상 종료 뒤 MeshNode descriptor에서 빠지고 같은 client의 후속 request가 남은 provider로만 가는지 확인한다.
- `RL-B4` (전환 필요): provider admin 경로가 현재
  `ZLinkChannelRuntimeOptions.clientServerChannel(channelName).weight(0/100)`으로 channel weight를
  변경해 신규 부하 제외와 복원을 검증한다. Java 10.0.0 exact interface의
  `ZLinkRouteMeshRuntimeOptions.channel(meshName, channelName)`을 구현한 뒤 같은 의미를 다시
  검증해야 한다. 이 동작을 graceful drain으로 판정하지 않는다.
- `RL-B5`: 느린 handler가 이미 받은 request는 drain 뒤에도 정상 reply하고, drain 이후 새 request는 다른 provider로 가는지 검증한다.
- `RL-B6`: provider-a에 public admin fault를 주입해 일부 request가 public 실패로 끝나는 동안, provider-b의 정상 reply가 계속 유지되고 follow-up request가 성공하는지 확인한다.
- `RL-C1`: 다량의 request와 send를 Consumer role을 통해 처리한 뒤 client driver가 정상 종료하고 runner가 프로세스 종료를 확인해 public 경로의 cleanup을 관측한다.
- `RL-C2`: runner가 provider-b를 `SIGKILL`해 owner lease가 갱신되지 않는 stale descriptor를 만들고 consumer를 새 discovery host로 재시작한다. public MeshNode runtime snapshot에서 provider-b가 live topology에서 빠지는지, 이후 request가 provider-a로만 가는지, provider-b 재시작 뒤 다시 traffic을 받는지 확인한다.
- `RL-C3`: provider-a 정지 구간의 public 실패와 재기동 후 topology 회복, 후속 request 성공을 같은 restart orchestration에서 확인한다.
- `RL-C4`: runner-owned Redis location store를 pause/unpause해 store outage를 만들고, 이미 연결된 channel request가 계속 성공하는지, public topology read가 outage 중 infrastructure error로 실패하는지, store 복구 뒤 topology read와 follow-up request가 정상화되는지 확인한다.
- `RL-D1`: 다수 client 프로세스가 동시에 request를 보내는 high fanout burst에서 정상 reply를 유지하는지 확인한다.
- `RL-D2`: provider dispatch-error observer가 실패해도 후속 request가 정상 처리되는지 확인한다. Observer 오류는 application callback이 아니라 structured log로 기록한다.
- `RL-D3`: 명시 `ZLinkMessageFlowObserver`가 미등록 request의 `outcome=failed`, `reason=no_handler`, `action=reply_error`, `packet_name` marker를 evidence에 남기고, 이후 request가 정상 동작하는지 확인한다.
- `RL-D4`: 미등록 request handler가 public HTTP 실패로 노출되고, provider dispatch-error evidence에 `no_handler`/`reply_error`가 남으며, 이후 request가 정상 동작하는지 확인한다. 공통 문서의 code round-trip 검증은 server-side dispatch evidence 경로로 닫는다.
- `RL-D5`: 같은 실행 안에서 request와 send를 섞어 여러 window로 지속 주입하고, 처리 성공과 단순 latency drift 한계를 관측한다.

## public API/harness 대기

- 위 `RL-A1`에는 Java runtime 오류 정규화와 RuntimeMonitoring socket source identity 수정이 필요하다.
- 위 `RL-B4`에는 Java 10.0.0 route-mesh runtime options 구현이 필요하다.

## 검증 방법

`run_e2e.sh all`은 RL-A1부터 RL-D5까지 selector를 실행한다. 차단 행은 표에 적은 10.0.0 단언을
충족한 뒤에만 완료로 바꾼다.
