<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Store 장애·복구](config-6-store-failure-recovery.ko.md) | [다음: 실행 turn과 terminator](config-8-execution-turn.ko.md)
<!-- framework-adapter-nav:end -->

# Config 7 — Runtime status와 변화 관찰

Application은 public runtime monitoring API로 Host와 RouteMesh의 현재 상태를 한 번에 읽거나, 상태가
바뀔 때마다 완전한 새 status를 받을 수 있다. 이 정보는 운영 화면과 readiness 판단에 사용되므로 실제
peer·Channel 상태와 달라지거나 느린 observer 때문에 업무 message가 지연되어서는 안 된다.

이 config는 여러 MeshNode의 시작, 종료, Store 장애와 capacity 변화를 만들고 public `GetStatus`와 status
stream이 실제 application 결과와 일치하는지 검증한다. Socket monitor, Location Store record와 private
runtime counter는 사용하지 않는다.

## 1. 확인 범위

- Host status와 RouteMesh status의 source·sequence 구분
- Peer와 Channel readiness의 추가·제거·복구
- Location Store 장애와 복구 상태
- Active Actor·Spot 수와 placement 가능 여부
- Logical Multicast 실행과 topology status의 독립성
- 느리거나 실패한 observer와 다른 업무 처리의 격리
- 잘못된 public 조회와 반복 restart 뒤 status 복구

언어별 E2E는 해당 언어의 정식 monitoring interface만 사용한다.

| 언어 | 정식 interface |
|---|---|
| C++ | [`route_mesh_runtime_t`](../spec/server/languages/cpp/interfaces/08-monitoring.ko.md) |
| .NET | [`IZLinkRouteMeshRuntime`](../spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md) |
| Java | [Java monitoring](../spec/server/languages/java/interfaces/monitoring.ko.md) |
| Kotlin | [Kotlin monitoring](../spec/server/languages/kotlin/interfaces/monitoring.ko.md) |
| Node.js | [`ZLinkRouteMeshRuntime`](../spec/server/languages/node/interfaces/03-location-observability.ko.md) |

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic discovery와 owner lease를 제공한다. 실행마다 전용 namespace를 사용한다. |
| Service node | 2 | 같은 MeshName과 ChannelName에 참여한다. Channel handler, Actor·Spot factory, Logical Multicast target과 public monitoring endpoint를 제공한다. |
| E2E client | 1 | 역할 server의 application endpoint를 호출하여 status 조회·관찰과 업무 operation을 시작한다. |

역할 server의 evidence endpoint는 public status 값, application handler marker와 operation 결과만
제공한다. Status는 조회 또는 observer callback에서 받은 immutable value를 그대로 저장하며 Framework
내부 object를 나중에 다시 읽지 않는다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 process, Store namespace와 marker를 새로 만든다. Status 전이는 observer stream을
bounded polling하고 마지막에는 `GetStatus`를 다시 호출하여 현재 상태와 대조한다. Observer stream은
변화를 합칠 수 있으므로 모든 중간 state나 연속된 sequence를 요구하지 않는다. 같은 source 안에서 관찰한
sequence가 증가하고 최종 status가 실제 상태와 일치하면 된다.

업무 처리 여부는 public request 결과와 handler application evidence로 확인한다. File log와 structured
log는 이 config의 통과 조건이 아니다.

## 4. Scenario

### Track A — 현재 상태와 readiness를 확인

#### MON-A1 Host와 RouteMesh status를 각각 읽는다

우선순위: `P0`

Host lifecycle과 개별 RouteMesh topology는 서로 다른 source다. Application이 두 sequence를 하나의
timeline처럼 비교하면 정상 상태를 오래된 값으로 오해할 수 있다.

**검증 질문:** Host status와 RouteMesh status가 각자의 source와 sequence로 완전한 현재 상태를
제공하는가.

- 시작 조건: `svc-a`만 시작하고 Host와 RouteMesh가 ready다.
- 절차: 두 status를 각각 읽어 보관한다. `svc-b`를 시작하고 peer와 Channel이 ready가 된 뒤 두 status를
  다시 읽는다.
- 검증: 두 번째 RouteMesh status는 ready peer와 ready target 증가를 반영하고 같은 Mesh source의 첫
  sequence보다 크다. Host status는 Host state와 새 작업 수락 여부를 제공하며 자기 source 안에서만
  sequence를 비교한다. 처음 보관한 status 값은 후속 변화로 바뀌지 않는다.
- 세부 동작: [Runtime monitoring §2](../spec/24-runtime-monitoring.ko.md)를
  검증한다.

#### MON-A2 Peer가 추가되고 제거된 결과를 관찰한다

우선순위: `P0`

Observer는 peer lifecycle의 모든 짧은 중간 state를 반드시 전달하지 않지만, current Ready peer가 바뀐
결과는 완전한 status로 제공해야 한다.

**검증 질문:** Peer 시작·종료·재시작 뒤 observer와 최신 조회가 current peer를 정확히 보여 주는가.

- 시작 조건: `svc-a`에서 RouteMesh observer를 열고 initial status를 받는다.
- 절차: `svc-b`를 시작하여 ready가 될 때까지 관찰한다. `svc-b`를 정상 종료하여 ready 목록에서 빠진
  것을 확인한 뒤 새 process로 다시 시작한다.
- 검증: 각 단계의 최신 status와 `GetStatus`가 같은 ready peer 집합을 제공한다. 재시작 뒤 새 Node RID가
  ready이고 이전 RID는 ready 목록에 없다. 관찰한 sequence는 같은 Mesh source에서 단조 증가한다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)와
  [§3](../spec/24-runtime-monitoring.ko.md)을 검증한다.

#### MON-A3 Channel readiness와 실제 request 결과를 대조한다

우선순위: `P0`

Channel status가 ready라고 표시되면 신규 request를 처리할 target이 있어야 한다. 반대로 positive weight
target이 하나도 없으면 선택 가능한 상태로 표시해서는 안 된다.

**검증 질문:** Channel의 ready target 수와 실제 request 성공 여부가 weight 변경 전후에 일치하는가.

- 시작 조건: `svc-b`만 해당 Server Channel을 weight 100으로 제공하고 `svc-a`의 status에서 target count가
  1이다.
- 절차: 정상 request를 한 번 보낸다. Public runtime update로 `svc-b` weight를 0으로 바꾸고 status가
  반영된 뒤 새 request를 보낸다. Weight를 100으로 복원하고 ready가 된 즉시 다시 요청한다.
- 검증: 처음과 복원 뒤 request는 handler에서 한 번 처리된다. Weight 0인 동안 status는 ready target이
  없음을 나타내고 request는 정식 오류 모델의 terminal 결과로 끝난다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)와
  [Channel topology §7](../spec/07-channel-topology.ko.md)을 검증한다.

#### MON-A4A 정상 replacement 뒤 readiness를 복원한다

우선순위: `P1`

정상 종료한 provider를 새 process로 바꾸면 이전 peer가 ready 목록에 남지 않고 새 peer가 target으로
선택되어야 한다.

**검증 질문:** 정상 replacement 뒤 최신 status의 첫 ready 시점부터 신규 request가 성공하는가.

- 시작 조건: 두 service node가 ready이고 `svc-b`가 유일한 target인 Channel이 있다.
- 절차: `svc-b`를 정상 종료하고 status에서 target 제거를 확인한다. 같은 역할의 새 process를 시작하고
  status가 ready가 되는 즉시 request를 보낸다.
- 검증: 최신 status에는 새 RID만 ready target으로 나타나며 request는 새 process handler에서 한 번
  처리된다. Application retry와 추가 settle sleep을 사용하지 않는다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)을
  검증한다.

#### MON-A4B Crash 뒤 stale peer를 제외하고 복구한다

우선순위: `P1`

Provider가 crash하면 정상 종료 통지를 보내지 못한다. Owner lease가 만료된 뒤에는 이전 descriptor를
ready target으로 사용하지 않고 새 process의 상태로 수렴해야 한다.

**검증 질문:** Crash한 peer가 lease 만료 뒤 ready 목록에서 빠지고 replacement request가 성공하는가.

- 시작 조건: Fresh topology에서 `svc-b`가 유일한 target으로 ready다.
- 절차: Runner가 `svc-b`를 강제 종료한다. Status에서 target unavailable 또는 제거를 확인한 뒤 새
  process를 시작하고 ready를 기다린다.
- 검증: 최신 status에는 crash한 RID가 ready로 남지 않고 새 RID가 ready다. Ready 직후의 request는 새
  handler에서 한 번 처리된다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### MON-A5 Store 장애와 복구 상태를 관찰한다

우선순위: `P1`

Location Store를 사용할 수 없으면 topology 갱신의 신뢰도가 낮아진다. Application은 public status에서
degraded 상태를 확인하고, Store 복구 뒤 다시 ready 상태가 되는지 판단할 수 있어야 한다.

**검증 질문:** Store 장애와 복구가 RouteMesh의 current status에 반영되는가.

- 시작 조건: 두 service node와 Store가 정상이고 RouteMesh status가 ready다.
- 절차: Runner가 Store process를 중지한다. Public status가 configured failure grace에 맞춰 degraded로
  바뀌는지 관찰한다. Store를 재시작하고 status가 ready로 복구될 때까지 기다린다.
- 검증: 장애 중 status는 정식 topology state와 unavailable reason으로 Store 문제를 나타낸다. 복구 뒤
  ready target과 실제 request 성공이 함께 복원된다. 고정 sleep으로 grace 경계를 추정하지 않는다.
- 세부 동작: [Location runtime §8](../spec/21-location-runtime.ko.md)과
  [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### MON-A6 Placement 집계와 capacity 결과를 대조한다

우선순위: `P0`

운영자는 active Actor·Spot 수와 새 placement 가능 여부를 public status에서 확인할 수 있어야 한다.
Count가 실제 create 결과와 다르면 scale-out 판단이 잘못된다.

**검증 질문:** Actor·Spot 생성과 제거 뒤 placement count와 `IsAvailable`이 실제 operation 결과와
일치하는가.

- 시작 조건: 작은 Actor total과 Spot total limit을 가진 `svc-a`가 ready다.
- 절차: Public manager API로 Actor와 User Spot을 한 개씩 만들고 status를 읽는다. Limit까지 추가 생성한
  뒤 한 번 더 create하고, 기존 object를 하나 제거한 뒤 다시 create한다.
- 검증: Active count는 완료된 public lifecycle 결과와 단계마다 일치한다. Limit 초과 create는
  `CapacityExceeded`이고 status는 placement 불가를 나타낸다. Object 제거 뒤에는 available로 돌아오고 새
  create가 성공한다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)와
  [MeshNode §5](../spec/13-mesh-node.ko.md)를 검증한다.

### Track B — Logical Multicast와 topology status를 분리

#### MON-B1 Remote target 일부가 받지 못해도 topology status를 delivery 결과로 바꾸지 않는다

우선순위: `P0`

Logical Multicast는 target별 delivery report를 반환하지 않는다. 한 remote target의 queue가 message를
받지 못했다고 해서 peer·Channel readiness를 delivery 통계처럼 바꾸면 안 된다.

**검증 질문:** 수락 가능한 target은 message를 처리하고 topology status는 실제 connection 상태를
그대로 유지하는가.

- 시작 조건: 서로 다른 service node process에 있는 두 remote matching target이 ready다. 한 target의
  handler를 application gate에서 막고 public HWM보다 큰 deterministic blocker payload를 먼저 보내 handler
  진입과 public status의 Application receive paused가 `true`인 것을 확인한다.
- 절차: Source가 고유 marker를 Logical Multicast로 한 번 제출하고, 수락 가능한 target evidence와 전후
  RouteMesh status를 읽는다.
- 검증: Public submit은 target별 결과 없이 정식 terminal 의미로 끝나며 수락 가능한 target은 marker를
  한 번 처리한다. Network와 peer 상태가 바뀌지 않았다면 ready peer와 Channel status도 유지된다.
- 세부 동작: [Spot messaging §4](../spec/12-spot-messaging.ko.md)와
  [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### MON-B2 Local target handler 대기가 다른 target 전달을 막지 않는다

우선순위: `P0`

한 local target의 handler가 대기해도 이미 처리할 수 있는 다른 target까지 rollback해서는 안 된다.
Topology status는 target별 delivery count를 대신하지 않는다.

**검증 질문:** Local target 하나의 handler가 대기해도 다른 matching target이 message를 먼저 처리하는가.

- 시작 조건: 같은 process에 matching target 두 개가 있고 하나의 handler는 application gate에서 대기한다.
  Network block이나 public HWM 경계는 사용하지 않는다.
- 절차: Source가 고유 marker를 한 번 publish하고 gate를 닫은 target과 다른 target의 application
  evidence를 수집한다. 다른 target의 evidence를 확인한 뒤 gate를 연다.
- 검증: 다른 target은 marker를 gate가 닫힌 동안 한 번 처리하고, gate를 연 target도 이후 한 번 처리한다.
  Publish는 target별 결과 payload를 반환하지 않으며 RouteMesh의 peer·Channel status는 변하지 않는다.
- 세부 동작: [Spot messaging §4](../spec/12-spot-messaging.ko.md)를
  검증한다.

### Track C — Observer를 업무 처리에서 격리

#### MON-C1 느리거나 실패한 observer가 다른 작업을 막지 않는다

우선순위: `P1`

Status observer는 운영 정보를 소비하는 Application callback이다. 한 observer가 status 처리를 늦게 하거나
예외로 종료되어도 message dispatch와 다른 observer는 계속 진행해야 한다.

**검증 질문:** 느린 observer가 막혀 있는 동안 request와 정상 observer가 계속 완료되는가.

- 시작 조건: `svc-a`에서 느린 observer와 정상 observer를 같은 Mesh에 연다. 느린 observer는 첫 callback을
  application signal에서 대기한다.
- 절차: `svc-b`를 시작·종료하여 여러 status 변화를 만들고 별도 Channel request를 보낸다. 정상 observer의
  최신 status와 request reply를 확인한 뒤 느린 observer를 예외로 종료한다.
- 검증: Request는 deadline 안에 reply를 받고 정상 observer는 current status를 제공한다. 느린 observer의
  sequence에 gap이 있어도 `GetStatus`를 다시 읽으면 최신 상태와 일치한다. 느린 observer 종료가 정상
  observer를 종료하지 않는다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)의
  observer 격리를 검증한다.

### Track D — 잘못된 조회와 반복 장애를 처리

#### MON-D1A 등록하지 않은 MeshName 조회를 거부한다

우선순위: `P1`

Application이 등록하지 않은 MeshName을 조회하면 다른 Mesh의 상태나 빈 정상 status를 반환해서는 안
된다.

**검증 질문:** 등록하지 않은 MeshName의 조회와 관찰 시작이 public validation error로 끝나는가.

- 시작 조건: Host에는 `game` Mesh만 등록한다.
- 절차: Application endpoint가 `missing-mesh`의 `GetStatus`와 `Observe` 시작을 각각 시도한다.
- 검증: 두 호출은 언어별 interface가 정한 configuration 또는 argument error로 끝나며 `game` status와
  observer에는 영향을 주지 않는다.
- 세부 동작: [Runtime monitoring §6](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### MON-D1B 반복 crash와 restart 뒤에도 status를 계속 관찰한다

우선순위: `P1`

Observer가 한 번의 장애만 처리하고 이후 변화를 놓치면 장시간 실행하는 운영 도구에서 사용할 수 없다.

**검증 질문:** Peer crash와 restart를 세 번 반복해도 같은 observer가 최종 ready 상태에 수렴하는가.

- 시작 조건: `svc-a`의 observer가 initial status를 받은 상태다.
- 절차: `svc-b` 강제 종료, peer 제거 확인, 새 process 시작과 ready 확인을 세 번 반복한다.
- 검증: 매 cycle의 최신 status는 실제 current RID와 ready target 수를 반영한다. 같은 source의 관찰
  sequence는 증가하며 마지막 `GetStatus`와 observer의 최신 값이 일치한다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)을
  검증한다.

## 5. 완료 기준

- 모든 판정은 public status, public operation result와 application handler evidence만 사용한다.
- Observer가 모든 중간 state와 연속 sequence를 전달한다고 가정하지 않는다. 마지막에는 항상
  `GetStatus`로 current 상태를 확인한다.
- Readiness, Store 복구와 handler 완료는 bounded polling하며 고정 sleep이나 log flush에 의존하지 않는다.
- Schema에 없는 private field가 없다는 검사는 E2E assertion으로 만들지 않는다. 언어별 public interface와 contract
  test가 public type shape를 검증한다.
- 한 observer의 지연과 예외가 다른 observer, message dispatch와 request reply를 바꾸지 않아야 한다.
