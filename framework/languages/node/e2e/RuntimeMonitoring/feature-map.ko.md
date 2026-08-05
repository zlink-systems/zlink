# Node.js RuntimeMonitoring E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`

## 10.0.0 목표 판정

Node.js E2E는 public `ZLinkRouteMeshRuntime` snapshot과 typed event를 Config 7의 판정 기준으로
사용한다. 기존 socket·location·Spot monitoring marker와 runner log는 관련 행의 부분
증거로만 유지하며 internal helper나 raw frame으로 빈 runtime field를 대신하지 않는다.

| 시나리오 | 상태 | 현재 증거 | 남은 gap |
|---|---|---|---|
| MON-A1 | 10.0.0 전환 대상 | `svc-b` 종료·재기동에서 topology node 수와 service summary 수가 감소한 뒤 복구된다. | Snapshot 하나의 MeshNode·peer·channel·claim·location·drain field, sequence와 불변성을 검증한다. |
| MON-A2 | 10.0.0 전환 대상 | Transient client 연결·해제의 socket marker와 topology 변경을 기록한다. | Typed peer event·snapshot의 RID, generation, descriptor revision, endpoint, admission, ready, last failure를 재시작 전후로 대조한다. |
| MON-A3 | 10.0.0 전환 대상 | Socket weight를 0·100으로 변경하고 admission 변경 marker를 drain과 분리해 관측한다. | Channel event, local weight, ready member 수·selectable과 실제 ChannelName request 선택 결과를 각 전이 뒤 비교한다. |
| MON-A4 | 10.0.0 전환 대상 | 우아한 종료 뒤 같은 RID·다른 endpoint replacement와 `SIGKILL`·owner lease 만료 뒤 후속 request를 검증한 로그가 있다. | 두 경로를 fresh topology로 나누고 각 peer·channel event 뒤 generation·endpoint·ready member를 최신 snapshot과 대조한다. |
| MON-A5 | 10.0.0 전환 대상 | Location runtime과 Spot status marker, Redis-backed topology 경로가 있다. | Redis 정지·failure grace·복구에서 store event, location state·last success·last failure와 current owner token 재검증을 단언한다. |
| MON-B1 | 미구현 | Logical Multicast publish 전용 snapshot type·field·metric·runtime event가 public source에서 제거되었다. | Target 0 publish가 transaction 시작 뒤 결과값 없이 완료하고 제거된 monitoring 이름이 관측되지 않는 process scenario를 추가한다. |
| MON-B2 | 미구현 | Logical Multicast target별 수락·drop은 public 결과나 monitoring으로 집계하지 않는 계약으로 변경되었다. | Local subscriber가 있어도 target별 count와 publish 전용 event가 없고 공통 runtime snapshot은 유지되는 process scenario를 추가한다. |
| MON-C1 | 10.0.0 전환 대상 | Monitoring handler 예외가 error marker로 보고된 뒤 같은 Trigger request가 성공한다. | Application gate, 느린 observer와 정상 observer를 함께 열어 claim progress·request completion·coalescing·sequence gap 후 snapshot resync를 단언한다. |
| MON-D1 | 10.0.0 전환 대상 | 중복 source, 비양수 interval, 없는 Spot·socket source startup 검증과 service 종료·재시작 경로가 있다. | 등록하지 않은 MeshName·0 이하 observer capacity 오류와 비정상 종료·lease 만료·재시작 3회의 sequence·snapshot·event field 제한을 검증한다. |

### Object Client 연결 상태

Node.js runtime은 Object Client pair를 다음과 같이 구분한다.

- 양쪽 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없으면
  peer 상태를 `NotRequired`로 기록한다. ready peer와 liveness 대상에는 포함하지 않는다.
- 어느 한쪽에 RouteMesh Channel Server membership이 있으면 연결이 필요하다. weight가 `0`이어도
  membership은 유지되므로 이 규칙은 바뀌지 않는다.
- 연결이 필요한데 ready connection이 없으면 `NotConnected`이며 `NotRequired`로 바꾸지 않는다.

Automatic discovery와 Manual handshake는 같은 내부 판정을 사용한다. Focused test는 planner,
reason `4` admission, ready peer와 liveness 제외를 검증했다. Config 1 `RM-A3` actual-process
scenario는 아직 실행하지 않았으므로 MON-A2 완료 증거로 사용하지 않는다.

## 실행 증거

- 전체 runner 로그: `framework/languages/node/e2e/RuntimeMonitoring/logs/20260703-220339-17357/`
- Topology 변경 집중 로그: `framework/languages/node/e2e/RuntimeMonitoring/logs/20260715-075251-2259558/`
- Lifecycle 집중 로그: `framework/languages/node/e2e/RuntimeMonitoring/logs/20260703-220318-16504/`

기존 runner 통과는 표의 현재 증거만 보존한다. 남은 gap을 구현한 뒤 결과값 없는 publish terminal,
publish 전용 monitoring 부재와 공통 runtime snapshot 유지를 함께 단언하는 canonical scenario를 재실행한다.
