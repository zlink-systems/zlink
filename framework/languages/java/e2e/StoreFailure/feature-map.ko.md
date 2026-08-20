# Java StoreFailure E2E feature map

이 문서는 공통 E2E Config 6 `Store Failure Recovery` 계약과 Java 구현의 대응 관계를 정리한다.
검증 기준은 `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`다.

## 실행 구조 상태

- `implemented`: Gradle 실행 그래프는 `Shared`, `Client`, `Server/Provider`,
  `Server/Consumer`로 구성하며, Config 6 선택자를 Client가 실행한다.
- `implemented`: provider와 consumer는 Redis location store extension을 같은 endpoint와 key
  prefix로 등록한다.
- `implemented`: consumer는 public `ZLinkLocationRuntimeQuery`를 HTTP endpoint로 노출한다.
- Config 6의 각 selector가 Java location store 경로를 실행한다. 완료 여부와 남은 단언은 아래
  시나리오별 행을 기준으로 판정한다.

## 구현됨

- `SF-A1`: Redis location store + provider 2개 + consumer 자동 연결 baseline을 검증한다.
  consumer의 public MeshNode runtime snapshot에서 provider MeshNode descriptor 2개가 보이고, store status가 healthy이며,
  consumer request가 provider 중 하나에서 처리되는지 확인한다.
- `SF-A2`: change-stamp surface를 숨긴 polling-only consumer가 `watchEnabled=false` 상태에서
  provider 추가(`api-c`)와 제거를 polling interval 안에 descriptor/runtime snapshot와 request path로 반영하는지 확인한다.
- `SF-B1`: runner가 전용 Redis 컨테이너를 pause한 동안 기존 consumer 연결의 request가 계속
  성공하고, public runtime status가 store unhealthy와 owner lease heartbeat failure를 보여준 뒤
  unpause 후 healthy로 복구되는지 확인한다.
- `SF-B2`(fixture 실행, runtime blocker): Store failure grace를 넘긴 outage 중 실제로 `api-b` process를
  시작하고, 기존 `api-a` traffic이 유지되는지와 신규 provider가 ready target에 들어가지 않는지를
  확인하도록 runner를 확장했다. `logs/20260806-011016-2079715`에서 `api-b`가 startup 단계의
  `RedisConnectionException`으로 종료되어 recovery 뒤 재등록 단언까지 진행할 수 없었다.
- `SF-B3`(fixture 구현, runtime blocker): public Instance Spot request로 owner를 만들고 periodic timer
  evidence를 기록한다. Redis authority operation을 owner lease TTL보다 길게 중단한 뒤 Existing
  Channel request는 성공했지만, timer count가 `41 -> 47`로 증가했고 신규 Instance Spot request가
  정식 `UNAVAILABLE` 대신 `RedisCommandTimeoutException`으로 끝났다. 재현 로그는
  `logs/20260806-011447-2112646`이다.
- `SF-C1`: `api-b`를 SIGKILL해 row 제거 없이 종료시킨 뒤, owner lease 만료 후 public topology의
  `LOST` row를 ready target에서 제외하고 consumer request가 survivor인 `api-a`로만 처리되는지
  확인한다. Java endpoint가 public `state`와 `draining`을 전달하도록 보완했으며,
  `logs/20260806-010946-2078321`에서 통과했다.
- `SF-C2`: provider HTTP `/shutdown`으로 `api-b`를 정상 종료시킨 뒤, owner lease TTL을 기다리지
  않고 public topology에서 ready `api-b`가 사라지고 request가 `api-a`로만 가는지 확인한다.
  `logs/20260806-005659-1895028`에서 통과했다.
- `SF-C3`: Instance Spot owner process를 `SIGSTOP`하여 lease를 만료시키고 같은 routing role의
  replacement process에서 같은 Spot ID를 다시 만든다. Old process를 재개한 뒤 20개 marker가
  replacement lifecycle에서만 처리되고 old lifecycle evidence가 증가하지 않는지 확인한다.
  `logs/20260806-005721-1896889`에서 통과했다.
- `SF-D1`: 전용 Redis proxy를 owner lease TTL보다 짧게 pause/unpause하면서 background client가
  계속 request를 보내고, 모든 request 성공과 복구 후 healthy status 및 provider row 2개를 확인한다.
  `logs/20260806-005803-1899932`에서 통과했다.
- `SF-D2`(runtime blocker): 전용 Redis proxy를 owner lease TTL보다 길게 pause하고 그 동안 `api-b`를
  SIGKILL한다. `api-a` request는 실제로 처리됐지만 복구 뒤 public topology에 `api-a` ready row가
  재등록되지 않아 후속 survivor 판정을 완료하지 못했다. 재현 로그는
  `logs/20260806-005829-1901959`이다.
- `SF-D3`: 전용 Redis proxy를 pause/unpause하면서 public runtime status가 healthy 상태에서 last
  refresh와 owner lease 갱신 시간을 노출하고, outage 중에는 store/owner lease unhealthy와 last
  error를 보이며, 복구 뒤 last refresh 시간이 전진하는지 확인한다.
  `logs/20260806-005930-1904617`에서 통과했다.
- `SF-E1`: consumer의 Redis proxy가 실제 Redis response를 1,200ms 지연시킨다. 지연된 descriptor
  query가 대기하는 동안 같은 consumer의 일반 request p99가 budget 안에 머무르고, response gate를
  해제한 뒤 follow-up request가 성공하는지 확인한다. proxy delay evidence와 결과는
  `logs/20260806-011246-2101811`에 있다.
- `SF-F9`: Channel provider process를 `SIGSTOP`하여 lease를 만료시키고 같은 routing role의 replacement를
  시작한다. Replacement가 20개 marker를 처리한 뒤 old process를 재개하고, 추가 20개 marker가 계속
  replacement lifecycle에서만 처리되는지 public reply와 두 provider의 HTTP evidence로 확인한다.
  `logs/20260806-010010-1907349`에서 통과했다.

## 남은 항목과 확인된 전제

- 위 `SF-B2`는 grace 초과 뒤 outage 중 `api-b`를 실제로 시작하고, `api-a` traffic 유지와 신규 provider의 ready target 편입을 검증한다. `logs/20260806-011016-2079715`에서 `api-b` startup이 `RedisConnectionException`으로 종료되어 recovery 재등록까지 진행할 수 없었으므로 runtime blocker로 남긴다.
- `SF-B3` fixture가 확인한 timer fence 실패는 StoreFailure 디렉터리 밖의 stateful authority runtime
  수정이 필요하다.
- `SF-C4`는 한 provider process에 두 RouteMesh role, ClientServer server와 fanout publisher를 함께
  구성하고 role별 consumer를 추가해야 한다. 현재 provider는 RouteMesh role 하나만 제공한다.
- `SF-C5`, `SF-F6`이 요구하는 object page query는 public Java
  `ZLinkLocationRuntimeQuery`에 구현되었다. `findActorLocation`, `findSpotLocation`과
  `listObjectLocations`는 bounded continuation과 4 MiB page budget을 적용한다. SF-C5의
  남은 작업은 1,001개 object를 실제 process fixture에서 생성하고 누락·중복 없는 결과를
  확인하는 것이다.
- `SF-F1`은 다른 언어 caller와 replacement process가 필요하다. Java StoreFailure 디렉터리 안의
  process만으로 언어 간 payload와 identity를 검증할 수 없다.
- `SF-F2`, `SF-F3`, `SF-F4`, `SF-F5`, `SF-F7`, `SF-F8`, `SF-F10`, `SF-F11`은 relocation store,
  state adapter와 capture/restore gate를 제공하는 stateful fixture가 필요하다. 현재 fixture는 relocation
  store를 등록하지 않고 `LeaseProbeSpot`의 relocation을 명시적으로 비활성화한다.
- `SF-G1`, `SF-G2`는 Actor·User Spot factory와 aggregate membership, factory gate 및 여러
  capacity variant를 제공하는 fixture가 필요하다. 현재 fixture는 Instance Spot factory 하나만 등록한다.

## 검증 방법

`run_e2e.sh`에 등록된 selector를 단일 ID로 실행하면 해당 process fixture와 client/server 검증을
수행한다. `all`은 등록 순서대로 실행하므로 현재는 `SF-B2` runtime blocker에서 중단된다. 완료
판정은 selector가 출력하는 `scenario <ID> passed`만으로 하지 않고 위의 public 상태·request·evidence
결과와 blocker를 함께 따른다.

## 공통 scenario parity gap — 2026-08-05

다음 공통 scenario는 현재 Java StoreFailure fixture와 public surface만으로는 완료할 수 없다.

- `SF-C4`
- `SF-F1`, `SF-F2`, `SF-F3`, `SF-F4`, `SF-F5`, `SF-F6`
- `SF-F7`, `SF-F8`, `SF-F10`, `SF-F11`
- `SF-G1`, `SF-G2`

## SF-C5 구현 및 검증 상태

SF-C5의 Java fixture는 서로 다른 1,001개 Instance Spot object를 public request 경로로 만든다. Consumer는
public `ZLinkLocationRuntimeQuery.listObjectLocations`를 통해 page size 1, 100, 1,000을 순서대로 조회하고,
continuation token을 따라가며 중복·누락·잘못된 stable type·잘못된 generation을 검사한다. Page가 끝났을 때
관찰한 global ID 수가 정확히 1,001개인지 확인한다.

구현 파일은 `Client`, `Server/Consumer`, `Shared`, `run_e2e.sh`에 있다. 2026-08-06 focused runner는
provider channel readiness 단계에서 중단되어 scenario 결과를 통과로 기록하지 않았다. native runtime 기동
문제를 해결한 뒤 같은 selector로 재검증해야 한다.
