# Node Framework transport readiness

이 문서는 Node Framework가 Core monitor event와 Router receive 결과를 사용해
transport readiness를 판정하는 내부 구조를 설명한다. Framework public/domain
계약에는 binding type이나 transport pair identity를 노출하지 않는다.

## 책임 경계

```text
+-----------------------------+
| Framework domain contract   |
+-----------------------------+
              |
              v
+-----------------------------+
| Semantic route admission    |
+-----------------------------+
              |
              v
+-----------------------------+
| Node binding public API     |
+-----------------------------+
              |
              v
+-----------------------------+
| Core transport and Router   |
+-----------------------------+
```

Monitor의 `CONNECTION_READY` event는 물리 연결 후보를 식별하는 자료이다.
그 event만으로 같은 routing id의 현재 Framework peer를 결정하지 않는다.
Wire admission과 liveness 결과가 Framework topology의 lifecycle 조건을
만족한 뒤에만 해당 후보를 semantic route로 선택한다.

## Event loop wake와 polling 지연 하한

Node binding의 public `Poller`는 현재 상태를 nonblocking으로 확인할 수 있지만, socket이
readable로 바뀌는 시점에 JavaScript callback을 호출하는 wake API는 제공하지 않는다. 따라서
Framework는 event loop를 막지 않는 timer로 다음 poll turn을 예약한다.

- RouteMesh backend는 1 ms마다 Application 수신, Completion 진행, monitor event와 liveness를
  확인한다. Completion이 Framework 내부에서 직접 만들어진 경우에는 ready callback이 이 주기보다
  먼저 pump를 깨울 수 있다.
- Channel ROUTER, subscriber와 route receive loop는 process에서 하나의 5 ms idle waiter를 공유한다.
  처리할 record가 이어지는 동안에는 batch 예산이 끝날 때 `setImmediate`로 양보하고 바로 다음
  turn을 진행하므로 5 ms를 매 batch마다 기다리지 않는다.

이 값은 event loop가 즉시 timer를 실행할 수 있을 때의 **최선 지연 하한**이다. 동기 application
작업이 event loop를 점유하거나 timer phase가 밀리면 실제 지연은 더 길어진다. 두 주기는 public
설정이 아니며 topology나 Spot 수에 따라 timer를 추가하지 않는다.

## 수신 경로의 pair identity

Router가 메시지를 수신하면 Core는 payload를 전달한 source pipe에서
`transport_pair_id`와 `transport_pair_generation`을 읽는다. 확장 public API인
`zlink_router_recv_part_v2()`는 이 값을 part metadata와 함께 반환한다. multipart
메시지에서는 첫 part를 전달한 source pipe의 identity를 전체 수신 sequence에
유지한다.

Node binding은 이 metadata를 내부 receive record에 전달한다. Framework는
HELLO, ADMIT, REJECT, liveness ACK와 같이 수신 pipe에 응답해야 하는 control
message를 해당 pair로 보낸다. RID별 current route로 다시 선택하면 reciprocal
connection이 있는 동안 다른 physical pipe를 선택할 수 있기 때문이다.

수신 metadata가 없는 일반 socket 경로는 기존 routing id 동작을 사용한다.
pair identity는 Framework domain message나 사용자 handler의 인자로 변환하지
않는다.

## lifecycle 규칙

- pair id와 generation이 모두 0이 아니어야 pair-scoped operation을 사용한다.
- generation은 같은 pair id의 이전 연결을 재사용하지 않도록 함께 비교한다.
- exact pair를 찾지 못한 송신은 다른 RID route로 fallback하지 않는다.
- monitor 후보 제거와 topology peer 제거는 semantic admission 결과와 lifecycle
  순서를 함께 확인한 뒤 수행한다. paired transport의 어느 lane에서
  `DISCONNECTED`가 관찰되어도 해당 pair의 readiness를 제거한다.
- readiness가 true로 보이더라도 exact pair 송신이 실패하면 E2E 시나리오의
  readiness 계약을 만족한 것으로 판정하지 않는다.

이 규칙은 stale connection이 replacement node의 메시지를 처리하거나, old
provider가 lease expiry 뒤 다시 current peer로 선택되는 것을 방지한다.

## Location Store 장애와 기존 transport

Location Store read 또는 owner lease renewal이 일시적으로 실패해도 이미
semantic route로 선택된 transport를 즉시 끊지 않는다. `StoreFailureGrace` 동안
Node Framework는 마지막으로 완전히 읽은 descriptor 집합을 기준으로 기존
connection의 liveness를 계속 관찰한다. Store 장애를 감지했다는 이유만으로
`fenceLocationAutoConnect()`를 호출하면 이 순서를 건너뛰므로 기존 request가
실패할 수 있다.

Store 장애 중에는 Store에서 확인하지 않은 새 provider를 semantic route에
추가하지 않는다. Store가 복구된 뒤 전체 descriptor를 다시 읽고, 그 결과에
따라 새 connection을 만들거나 이전 connection을 제거한다. 이 정책은 owner
lease가 새 ownership 작업을 허용하는지와 이미 설정된 transport가 liveness를
유지하는지를 서로 다른 판단으로 처리한다.

물리 peer가 제거되면 Node backend는 해당 endpoint의 connection intent도 함께
정리한다. intent를 남겨 두면 auto-connect가 원격 process 종료를 관찰하지 못한
상태로 같은 target을 재연결할 수 있다. 이 통지는 실제 peer 제거 시점에만
발생하며, Store 장애를 이유로 transport를 강제 종료하는 신호로 사용하지 않는다.

Liveness timeout도 같은 peer 제거 경계를 사용한다. timeout 판정 뒤에는
semantic topology, physical connection candidate와 backend connection intent를
분리해서 정리하지 않고 하나의 `removePeer()` 경로에서 함께 갱신한다. 이 순서를
나누면 route status에서는 peer가 제거되어도 auto-connect가 이전 endpoint를 다시
선택할 수 있다.

Instance Spot의 close 직후 authority generation만 갱신된 경우에는 같은
application owner가 유지되는지 확인한 뒤 현재 route fence를 사용해 대기 중인
message를 materialization 이후 enqueue한다. owner identity가 바뀐 경우에는
이 경로에서 이전 message를 새 owner로 재전송하지 않는다. 이는 lifecycle 수렴과
application message retry를 구분하기 위한 제한이다.

## Sample client 시작 경계

Node sample runner는 TCP 또는 HTTP listener가 열렸다는 사실만으로 client를
시작하지 않는다. Object Server 역할은 public RouteMesh status의 `isReady`와
`placement.isAvailable`을 확인한 뒤 client-facing endpoint를 ready로 공개한다.
Object Client 역할은 placement를 소유하지 않으므로 `isReady`만 확인한다.

이 구분이 없으면 listener는 열렸지만 Instance Spot 또는 Room Spot의 admission이
끝나지 않은 시점에 첫 request가 전송될 수 있다. 그 결과는 readiness timeout이
아니라 placement target 없음, actor route fence 불일치 또는 operation cancellation으로
나타날 수 있다. runner는 임의의 sleep으로 이 순서를 보정하지 않고 public snapshot과
observation 결과를 evidence로 사용한다.

Stateful Instance Spot의 timer도 owner lease 경계를 따른다. Provider의 public
location status에서 lease가 만료된 뒤에는 기존 transport가 유지되더라도 신규
stateful request와 timer callback을 실행하지 않는다. Lease 만료 직전에 이미
serial executor에 들어간 tick은 해당 경계 이전의 accepted work로 처리될 수
있으므로, E2E 검증은 provider lease 만료 시점을 확인한 뒤 timer period에 따른
bounded observation 구간을 거쳐 이후 evidence만 비교한다.

## 한 host의 multi-role replacement

Config 6의 SF-C4는 한 process에 두 RouteMesh server, 한 ClientServer server와
한 classic fanout publisher를 등록한다. 이 등록은 다른 Store failure 시나리오의
topology와 evidence를 바꾸지 않도록 SF-C4 profile에서만 활성화한다. Consumer는
대응하는 두 RouteMesh client, ClientServer client와 automatic fanout subscriber를
같은 profile에서 구성한다.

Replacement readiness는 consumer의 public snapshot으로 각 역할을 따로 확인한다.
두 RouteMesh의 channel readiness, ClientServer의 ready target, fanout의 ready
publisher가 모두 만족되어야 marker를 보낸다. publisher가 Location Store에
descriptor를 기록하고 subscriber가 이를 연결하는 과정은 비동기이므로, runner는
고정 sleep 대신 bounded polling을 사용한다.

RouteMesh marker는 RouteMesh client를 사용하고, ClientServer marker는
ClientServer의 public channel client를 사용한다. Fanout marker는 replacement
provider의 public fanout client가 publish를 accept한 뒤 consumer subscriber의
evidence에 나타날 때까지 bounded observation으로 확인한다. publish acceptance와
subscriber handler 완료는 서로 다른 결과이므로 한 번의 HTTP 응답만으로 수신
완료를 판정하지 않는다.

Old provider의 evidence 파일은 replacement 전후의 line count를 비교한다. 새
process도 같은 semantic routing id를 사용할 수 있으므로 routing id만으로
process lifecycle을 판정하지 않는다. 따라서 public readiness와 old-process
evidence 불변 조건을 함께 사용한다.

## Node 실행 queue의 실제 기본 한도

Node의 Spot·Actor serial scheduler는 application callback과 lifecycle callback을
같은 owner 순서 안에서 처리하지만, 각 lane의 admission 예산은 따로 계산한다.
현재 구현의 기본값은 다음과 같다.

| 항목 | 기본값 |
|---|---:|
| Application 대기·실행 message | 4,096건 |
| Application 대기·실행 byte | 16 MiB |
| Lifecycle 대기·실행 message | 1,024건 |
| Lifecycle 대기·실행 byte | 4 MiB |
| 작업 하나의 고정 byte 비용 | 256 bytes |
| Owner가 연속으로 실행할 수 있는 시간 | 50 ms |
| Application에 실행 기회를 주기 전 lifecycle 연속 실행 | 8건 |

Byte 예산에는 payload와 metadata에 작업당 고정 비용을 더한다. Scheduler는 작업을
queue에서 꺼낸 시점이 아니라 callback이 끝난 뒤에 건수와 byte를 반환한다. 따라서
실행 중인 callback도 한도를 계속 사용한다. 이 값은 Node 내부 기본값이며 public
configuration contract가 아니다. 공통 internals의 수치는 구현 비교를 위한 참조값이므로
Node의 실제 기본값으로 해석하지 않는다.

## Raw transport 요청의 terminal 보장과 monitor queue

transport pair를 지정한 raw request가 동기적으로 거부되면, Framework는 이미 등록한
operation을 즉시 실패 terminal로 완료한다. 이 순서를 생략하면 caller의 awaitable이
timeout까지 남고, reconnect가 반복될 때 operation capacity와 timer가 누적될 수 있다.

Monitor callback은 readiness fence를 먼저 갱신한 뒤 drain queue에 event를 추가한다.
drain이 지연되는 장애 상황에서도 queue와 routing id 미확정 candidate는 고정된 상한을
넘지 않는다. 같은 physical connection의 event는 최신 event로 합친다. 서로 다른
connection의 event로 queue가 가득 차면 가장 오래된 event를 제거하고 새 lifecycle
event를 보존한다. 이 제한은 monitor event를 application message queue로 확장하지
않으며, 정상 경로의 message hot path allocation에도 영향을 주지 않는다.
