# Java ResilienceLifecycle E2E feature map

이 문서는 Config 5 Resilience/Lifecycle 공통 시나리오 중 Java framework E2E가 현재 검증하는
항목과, 추가 public API 또는 harness 제어가 필요한 항목이 남았는지를 구분한다. Client는 HTTP driver이고, 실행
시나리오의 framework 참여는 `Server/Consumer` role이 맡는다. provider/consumer process lifecycle은
Client support가 제어한다. Provider와 Consumer role은 같은 Redis location store endpoint와 실행별
key prefix를 공유한다. Consumer role은 public Spring starter, `ZLinkClient`,
현재 구현의 `ZLinkChannelRuntimeOptions.clientServerChannel(channelName)`과 public MeshNode runtime
snapshot을 사용한다. 10.0.0에서는 `ZLinkRouteMeshRuntimeOptions.channel(meshName, channelName)`으로
전환해야 한다.

## 시나리오 상태

- `RL-A1` (차단): 같은 client process의 종료·재시작 흐름은 있지만, down 구간 오류가 정식
  `RouteNotConnected`로 정규화되지 않고 `ConnectionReady` identity도 확인하지 못한다. runtime 오류
  정규화와 monitoring source가 수정된 뒤 terminal `Drained`, row 제거와 새 generation을 함께
  검증해야 한다.
- `RL-A2`: old provider의 slow handler 시작 뒤 process를 강제 종료하고, owner lease 만료로 old row가
  제외된 뒤에만 같은 routing id의 다른 endpoint에서 replacement를 시작한다. 같은 client 프로세스가
  새 owner generation·endpoint와 새 provider에만 기록된 반복 request 20개를 확인한다.
- `RL-A3`: 동시에 여러 client 프로세스를 두 차례 띄워 server에 재접속 폭주를 만들고, 각 client의
  public request가 정상 reply를 받는지 확인한다.
- `RL-A4`: provider-b를 public runtime drain으로 신규 request 대상에서 제외한 뒤 종료하고, 같은
  routing id의 green endpoint로 교체해 request가 green provider evidence에 기록되는지 확인한다.
  이후 green endpoint를 내리고 원래 provider-b endpoint를 복구해 신규 request가 복구된 provider로
  다시 가는지 확인한다.
- `RL-A5`: provider-a가 짧은 간격으로 down/up을 반복하는 동안 같은 client 프로세스가 지속 request를
  보내고, 정상 provider-b로 수렴해 timeout 없이 follow-up까지 성공하는지 확인한다.
- `RL-B1`: 처리 중인 request를 client timeout으로 끝낸 뒤 같은 client의 후속 request가 정상 reply를
  받아 late reply가 pending을 오염시키지 않는지 확인한다.
- `RL-B2`: provider-b가 slow request를 받은 상태에서 강제 종료되면 in-flight request가 public 실패로
  끝나고, owner lease TTL 뒤 stale topology가 제거되어 provider-a로 수렴하는지 확인한다. 이후 같은
  routing id의 provider-b를 다시 올려 topology와 신규 request가 복구되는지도 확인한다.
- `RL-B3`: provider의 slow handler가 이미 받은 request를 정상 reply한 뒤 runtime drain이 terminal
  `Drained`로 끝나는지 확인한다. 이어서 MeshNode descriptor 제거와 남은 provider의 후속 request 성공을
  검증한다.
- `RL-B4` (전환 필요): provider admin 경로가 현재
  `ZLinkChannelRuntimeOptions.clientServerChannel(channelName).weight(0/100)`으로 channel weight를
  변경해 신규 부하 제외와 복원을 검증한다. 10.0.0 exact interface를 구현한 뒤
  `ZLinkRouteMeshRuntimeOptions.channel(meshName, channelName)`으로 같은 의미를 다시 검증해야 한다.
  이 동작을 graceful drain으로 판정하지 않는다.
- `RL-B5`: 느린 handler가 이미 받은 request는 weight 0 변경 뒤에도 정상 reply하고, 전파 완료 뒤의
  새 request는 다른 provider로 가는지 검증한다. `Draining`이나 actor handoff는 단언하지 않는다.
- `RL-B6`: provider-a에 public admin fault를 주입해 일부 request가 public 실패로 끝나는 동안,
  provider-b의 정상 reply가 계속 유지되고 follow-up request가 성공하는지 확인한다.
- `RL-C1`: 같은 Consumer role이 반복 request 뒤 follow-up request를 보내 public 경로의 client
  lifecycle cleanup을 관측한다.
- `RL-C2`: provider-b 강제 종료 뒤 public location topology에서 stale descriptor가 제외되고, 정상
  provider-a로 request가 수렴하는지 확인한다. provider-b 재기동 뒤 같은 endpoint row와 traffic
  복구도 함께 검증한다.
- `RL-C3`: provider-a 정지 구간의 public 실패와 재기동 후 topology 회복, 후속 request 성공을
  같은 restart orchestration에서 확인한다.
- `RL-C4`: runner가 실행별 Redis location store를 시작하고, Client support가 store를 일시 중지한
  동안 이미 연결된 provider channel의 request가 계속 성공하는지 확인한다. store 복구 뒤에는 public
  location topology와 후속 request가 다시 정상 동작하는지도 확인한다.
- `RL-D1`: 다수 client 프로세스가 동시에 request를 보내는 high fanout burst에서 정상 reply를
  유지하는지 확인한다.
- `RL-D2`: dispatch-error observer가 예외를 던지도록 fault를 켠 뒤 handler 없는 request를 보내고,
  observer failure가 provider process와 messaging 경로를 멈추지 않는지 후속 request와 evidence로
  확인한다. 또한 structured logger가 `zlink.runtime_error`/`observer_failed`/
  `message_flow_observer` event를 한 번 받는지 확인해야 한다.
- `RL-D3`: 명시 `ZLinkMessageFlowObserver`가 미등록 request의 `outcome=failed`,
  `reason=no_handler`, `action=reply_error`, `packet_name` marker를 evidence에 남기고, 이후
  request가 정상 동작하는지 확인한다.
- `RL-D4`: 같은 버전 provider/consumer 사이에서 handler 없는 request가 public error reply로
  실패하고, provider message-flow observer evidence에 미등록 packet marker가 남는지 확인한다. Java
  public client 표면은 error code header를 직접 노출하지 않으므로 code round-trip은 evidence와
  정상 follow-up request로 검증한다.
- `RL-D5`: 같은 실행 안에서 request와 send를 섞어 여러 window로 지속 주입하고, 처리 성공과 단순
  latency drift 한계를 관측한다.

## 남은 항목

- 위 `RL-A1`은 runtime 오류 정규화와 RuntimeMonitoring의 socket source identity 수정이 선행되어야 한다.
- 위 `RL-B4`는 10.0.0 route-mesh runtime options 구현이 선행되어야 한다.

## 검증 방법

`run_e2e.sh all`은 RL-A1부터 RL-D5까지 selector를 실행한다. 차단 행은 runner가 성공 marker를
출력하더라도 표에 적은 10.0.0 단언이 모두 충족되기 전에는 완료로 바꾸지 않는다.

## 공통 scenario parity gap — 2026-07-29

다음 공통 scenario에는 Java actual fixture와 runner selector가 없다.

- `RL-E1`, `RL-E2`, `RL-E3`, `RL-E4`, `RL-E5`
- `RL-F1`, `RL-F2`, `RL-F3`, `RL-F4`, `RL-F5`, `RL-F6`, `RL-F7`
- `RL-F8`, `RL-F9`, `RL-F10`, `RL-F11`, `RL-F12`, `RL-F13`, `RL-F14`
