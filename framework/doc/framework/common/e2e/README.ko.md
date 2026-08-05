<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [공통 스펙](../README.ko.md) | [공통 샘플](../sample/README.ko.md)
<!-- framework-adapter-nav:end -->

# Framework Scenario E2E 테스트

이 문서는 ZLink Framework의 언어별 구현이 **실제 배포와 똑같이 생긴 서버 위에서도 제대로
실행되는지**를 확인하는 e2e 테스트를 정리한 것이다.

E2E는 공통 spec에 정의된 계약을 검증하고 구현 누락을 찾는 기준이다. E2E 시나리오나
다른 언어 구현만을 근거로 새 public API를 추가하지 않는다. 시나리오 수행에 새 API가
필요해 보이면 먼저 공통 spec에 계약 근거가 있는지 확인한다.

같은 검증이라도 e2e는 contract 테스트나 샘플과 결이 다르다.

- contract 테스트는 API 하나하나의 약속을 in-process로 빠르게 못 박는다.
- 샘플은 사용자가 그대로 따라 할 수 있는 정상 흐름을 보여 준다.
- e2e는 거기서 한발 더 나아간다. 실제 공유 location store를 두고, 위치를 실제로 resolve하고,
  provider를 여러 개 두고, 프로세스 경계까지 실제로 나눈 상태 — 즉 **배포 현장과 같은
조건**에서 기능이 의도대로 동작하는지를 확인한다.

## 1. 분류 원칙 — config 중심

e2e는 기능을 평면적으로 나열하지 않는다. **실제 배포와 같은 서버 구성(config)을 하나의
단위로** 두고, 그 위에서 세부 동작을 실 사용자처럼 검증한다. 각 config는 sample 프로젝트처럼
독립 실행 앱이고, 서버 구성을 한 번 시작한 뒤 여러 client 시나리오를 차례로 실행한다.

### 선정 기준

시나리오는 "기존 테스트와 안 겹치는가"로 고르지 않는다. **현실적인 배포 구성에서 실 사용자가
하는 흐름인가**로 고른다.

- 기존 unit/contract/in-process 테스트와 단언이 겹쳐도 된다. 차별점은 단언의 새로움이 아니라
  현실적인 배포 컨텍스트와 sample 수준 public API 사용이다.
- 같은 기능이라도 실 공유 store·실 resolve·다중 노드·프로세스 경계가 끼면 다르게 동작할 수
  있다. 바로 그 지점을 본다.

### 코드 작성 규칙

- e2e는 실제 사용 흐름을 흉내 낸다. client 시나리오는 framework 내부 API를 직접 호출하지 않고,
  실제 사용자처럼 **기능을 제공하는 역할 server app의 HTTP endpoint**를 호출한다. 시나리오 실행만
  대신해 주는 driver/test-runner server endpoint를 호출하는 구조는 실사용 흐름으로 보지 않는다.
- client는 언어별 HTTP client wrapper를 사용한다. `.NET`에서는 `ZLinkHttpClient`를 사용하고, raw
  `HttpClient`로 e2e app endpoint를 직접 호출하지 않는다. stream connector 자체를 검증하거나,
  상태 변경·event·push처럼 값이 바뀌는 순간을 관찰해야 하는 시나리오에서는 client stream
  connector를 공개 client 표면으로 사용한다.
- client 코드에서 channel/fanout/spot framework client, framework host 구성, test-only
  helper를 직접 사용하지 않는다. 예를 들어 `.NET` client에서는 `IZLinkChannelClient`,
  `AddZLinkFramework`, `Host.CreateDefaultBuilder`, reflection 우회, private/internal API 접근을
  쓰지 않는다.
- request/send/publish/resolve 같은 framework 호출은 기능을 제공하는 실제 역할 server app 내부에
  둔다. server는 app endpoint를 통해 사용자가 하는 요청을 받고, 그 안에서 공개 framework API로
  실제 기능을 실행한다. 기존 client 검증 코드를 `Server/Driver`, `ScenarioRunner`, `TestHost` 같은
  별도 프로젝트로 옮겨서 framework 호출을 대신 수행하게 만들지 않는다.
- process 시작·종료, restart, scale-out/in, failover처럼 서버 구성을 바꾸는 작업은 `run_e2e.*` 또는
  client support 코드에서 다룰 수 있다. 하지만 이런 코드가 framework messaging 호출을 대신 수행하면
  안 된다. framework messaging 호출은 실제 역할 server endpoint 안에 있어야 한다.
- client 시나리오는 파일별로 나누고, 각 시나리오 파일 첫머리에 무엇을 검증하는지 짧게 설명한다.
  시나리오 본문은 가급적 connector 또는 HTTP client 호출 흐름이 보이게 작성하고, 검증 helper가
  핵심 흐름을 숨기지 않게 한다. 시나리오 파일이 `/run`, `/scenario/all`, `/execute`처럼 시나리오
  전체를 위임하는 endpoint 하나만 호출해서 끝나면 안 된다.
- 상태 변경을 확인하려고 같은 HTTP 조회를 짧은 간격으로 수십 번 반복하지 않는다. 서버가 push로
  알려 줄 수 있는 흐름이면 client stream connector를 먼저 연결해 두고, HTTP 호출은 상태 변경을
  트리거하는 역할로만 쓴다. 검증은 connector가 받은 push payload와 실제 역할 server evidence/log를
  함께 대조한다.
- Pub/Sub처럼 검증 대상이 client stream session이 아니라 별도 subscriber 역할 server가 받은 fanout
  delivery인 경우에는 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로
  사용할 수 있다. 이때 evidence는 실제 subscriber 역할 server에서 남긴 것이어야 하고, client는
  publisher endpoint로 publish를 트리거한 뒤 각 subscriber의 marker를 확인한다.
- 검증은 client가 볼 수 있는 결과와 실제 역할 server가 남긴 evidence/log를 조합해 직접 표현한다.
  시나리오 실행 전용 driver가 남긴 evidence만으로 기능 검증을 대신하지 않는다.
- E2E 시나리오가 요구하는 기능이 특정 언어에 없더라도, spec 또는 공통 framework spec/guide에
  공개 계약 근거가 없으면 새 public API를 바로 추가하지 않는다. 다른 언어 구현은 계약 해석을
  비교하는 참고 자료일 뿐, 그 자체로 새 계약의 근거가 아니다.
- 공개 계약 근거가 없어서 구현할 수 없는 항목은 내부 helper, private API, 프레임 바이트를 직접
  조작하는 우회로 메우지 않는다. feature-map에 public contract gap으로 남기고, 필요한 draft/spec/guide
  검토를 별도 작업으로 분리한다.

### 메시지 이름 원칙

e2e 메시지 이름도 공통 샘플의 메시지 이름 원칙을 따른다. e2e는 샘플보다 검증 범위가 넓지만,
client와 server가 주고받는 payload는 사용자가 보는 공개 예시와 같은 기준으로 읽혀야 한다.

request로 호출하고 응답을 기다리는 payload는 `Req`와 `Res`를 쌍으로 사용한다. 이 기준은
channel request, route request, stream request, HTTP request에 모두 적용한다. 응답이 없는
단방향 send payload는 `Msg`를 사용하고, server가 client stream/session으로 밀어 주는 payload는
`Notify`를 사용한다.

업무 이름이 event처럼 보이더라도 호출 방식이 request/reply이면 `Req`/`Res`가 맞다. 예를 들어
상태 변경을 요청하고 처리 결과를 기다리는 e2e payload는 `StatusChangedReq`와 `StatusChangedRes`
처럼 명명한다. 반대로 client가 server push로 받는 상태 변경 알림은 `StatusNotify`처럼
명명한다.

### 수명·배치 시나리오 용어

서버 수와 프로세스 상태가 바뀌는 시나리오는 아래 뜻을 구분해서 쓴다. 이름이 다르면 검증해야 할
계약도 다르므로, 한 시나리오를 여러 용어로 부르지 않는다.

- **scale-out**은 실행 중인 node를 추가하는 것이다. 기존 요청 처리 주체를 자동으로 바꾸는 동작을
  뜻하지 않는다.
- **scale-in**은 실행 중인 node를 제거하는 것이다. Logical continuity가 필요한 상태 이전과
  host 종료는 `Relocate`, 새 relocation 없는 bounded cleanup은 `Shutdown`으로 구분한다.
- **restart**는 같은 application 역할의 process를 중단한 뒤 다시 시작하는 것이다. Location Store를 사용하는
  automatic topology에서는 lifecycle마다 prefix 뒤에 새 RID를 발급하며 이전 RID를 재사용하지 않는다.
- **replacement**는 중단된 node와 같은 application 역할을 새 process가 이어받는 것이다. Automatic topology의
  replacement도 새 RID·lifecycle을 사용한다. 같은 RID를 재사용하는 검증은 Store descriptor가 없는 manual
  topology의 role `None`에만 둔다.
- **failover**는 하나의 node가 비정상 종료된 뒤에도 이미 실행 중인 다른 node가 처리 가능한 작업을
  계속 받는 것이다. 중단된 node의 상태가 자동으로 이전된다는 뜻은 아니다.
- **Actor relocation**은 특정 Actor의 처리 주체를 공개 relocation 계약으로 바꾸는 명시적 동작이다.
  Host `Relocate`가 relocation을 시작하는 경우에는 **maintenance handoff**라고 구분한다.
- **recovery**는 restart, replacement, 재join, replay처럼 실패 뒤 서비스를 정상 상태로 되돌리는
  절차 전체를 뜻한다.

MeshNode를 추가해도 기존 Spot 또는 actor의 owner는 자동으로 바뀌지 않는다. 새 node는 공개 배치
입력과 정책에 따라 이후 새로 만드는 Spot 또는 actor의 배치 후보가 될 수 있다. 기존 owner를
바꾸려면 명시적 Actor relocation이나 `Relocate` maintenance handoff처럼 별도 계약에 따른 동작이 있어야 한다. 따라서
scale-out 시나리오는 기존 owner 유지와 신규 배치를 검증하고, Actor 이동 시나리오는 상태·mailbox·
bound session 인계 계약을 별도로 검증한다.

## 2. 표준 프로젝트 구조

각 config는 언어별 표준 위치에 sample과 분리된 e2e 앱으로 둔다. 서버와 client는 실제 프로세스
경계를 가진 앱으로 구성하고, 모든 언어가 같은 의미의 폴더 구조를 유지한다. 언어별 build 도구
이름과 파일 확장자는 달라도 역할 분리, 시나리오 파일 분리, 파일 분류, evidence/wait 방식은 같은
의미를 유지한다.

E2E app은 아래 구조를 사용한다. 다른 언어로 시나리오를 옮길 때도 파일 위치를 그대로 복사하지 않고,
같은 역할의 코드를 아래 책임에 맞춰 배치한다.

언어별 구현은 이 문서와 각 config 문서가 정의한 역할 경계를 따른다. 예를 들어 provider와
workflow가 서로 다른 배포 역할이면 언어와 관계없이 별도 실행 프로젝트로 구성하고, 하나의 서버를
옵션만 바꿔 두 역할로 사용하지 않는다. client의 시나리오 코드와 공용 실행 지원 코드도 서로 다른
책임으로 분리한다.

예:

```text
framework/languages/<lang>/e2e/<Config>/
|-- Shared/
|-- Server/
|   |-- <Role>/
|   |   |-- Program.*
|   |   |-- <Role>HostFactory.*
|   |   |-- <Config>.<Role>.<project>
|   |   |-- Configuration/
|   |   |-- Endpoints/
|   |   |-- Handlers/
|   |   `-- Infrastructure/
|   `-- <OtherRole>/
|-- Client/
|   |-- Program.cs
|   |-- Scenarios/
|   `-- Support/
|-- run_e2e.sh
|-- feature-map.ko.md
`-- README.ko.md
```

`README.ko.md`는 config별 보충 설명이 필요할 때 둔다. 공통 시나리오 정의와 완료 기준은
`framework/doc/framework/common/e2e/config-*.ko.md` 문서가 기준이다.

실행 방식은 sample smoke와 비슷하다. test framework가 같은 프로세스 안에서 host를 직접 만드는
게 아니라, `run_e2e.*`가 서버 프로세스를 순서대로 시작하고 포트 readiness를 확인한 뒤 client
시나리오를 실행한다. scale·failover 같은 시나리오는 같은 스크립트가 프로세스를 추가로 시작하거나
종료한다.

역할 server endpoint 사용 방식은 `PubSub`와 `RegistrationCodec`의 client 흐름을 참고할 수 있다.
client는 publisher/subscriber/main 같은 실제 역할 server의 endpoint를 직접 호출하고, server endpoint
안에서 framework 기능을 실행한다. 다만 오래된 config의 파일 위치가 이 절의 폴더 분류와 다르면,
그 위치까지 그대로 따라 하지 않는다. 별도 driver server를 시작한 뒤 client가 그 driver에 "전체
시나리오 실행"을 맡기는 구조는 이 문서의 표준 구조가 아니다.

### 2.1 로컬 E2E 대기 기준

모든 framework e2e runner는 로컬 실행에서 같은 대기 기준을 사용한다. 기준값은 각 `run_e2e.sh`
상단의 명시적인 config 상수로 둔다. 환경변수는 느린 CI나 진단용 override가 필요할 때만 사용할 수
있고, 기본 완료 증거는 override 없이 이 값으로 통과한 실행 결과여야 한다.

기본값은 아래와 같다.

| 항목 | 기본값 | 의미 |
|------|--------|------|
| local readiness timeout | 3초 | 새로 시작한 로컬 process의 port, health, readiness가 준비될 때까지 기다리는 최대 시간 |
| local readiness poll interval | 0.1초 | readiness를 다시 확인하는 간격 |
| route settle | 5초 | public readiness를 확인한 뒤 route status와 application evidence가 안정화되는지 확인하는 bounded wait 상한. sleep만으로 성공을 판정하지 않는다 |
| scenario settle | 3초 | 이전 operation의 terminal과 evidence 정리 뒤 사용하는 cleanup grace 상한. 다음 시나리오의 준비나 성공을 판정하지 않는다 |
| HTTP probe/admin/evidence request timeout | 3초 | `/health`, `/evidence`, `/admin/*`, control ping 같은 로컬 HTTP probe 한 번의 최대 시간 |

이 값 안에 준비되지 않는 로컬 e2e는 대기 시간을 늘려서 통과시키지 않는다. startup 순서, readiness
endpoint, location row 등록, route propagation, stale process/port, 오래된 build artifact, lifecycle
cleanup 같은 원인을 먼저 찾아 수정한다. 긴 대기는 버그를 늦게 발견하게 만들기 때문에 완료 조건으로 인정하지
않는다.

client scenario process timeout, 전체 child group timeout, shutdown/recovery처럼 시나리오 자체가 긴
작업을 검증하는 timeout은 위 readiness/settle 기준과 구분해 별도로 명명한다. 이런 timeout은 테스트
프로세스의 상한이나 검증 대상 동작의 일부이지, 로컬 process가 준비되기를 기다리는 readiness 값이
아니다.
bounded evidence wait처럼 서버가 시나리오 event를 기다리는 요청도 같은 원칙을 따른다. 단순 evidence
snapshot 요청은 3초 HTTP 기준을 쓰지만, event가 나올 때까지 기다리는 bounded wait는 별도 이름의
시나리오 대기값으로 분리한다.

### 2.1.1 표준 runner 출력과 중단 정리

언어별 `run_e2e_all.sh`는 실행자가 지금 어디까지 진행됐는지 바로 알 수 있게 같은 형태의 요약
라인을 출력한다. 각 config의 상세 로그는 기존처럼 그대로 흘려보내되, 집계 runner가 시작, config
완료, 전체 완료를 짧게 요약한다.

기본 출력 형태는 아래와 같다.

```text
[<language>-e2e] start configs=<count> at=<iso-time>
[<language>-e2e] <Config> start scenario=<selector>
[<Config>] <Scenario> start
[<Config>] <Scenario> PASS (<seconds>s)
[<language>-e2e] <Config> PASS (<seconds>s)
[<language>-e2e] total PASS (<seconds>s)
```

`<language>`는 `dotnet`, `java`, `node`, `cpp`처럼 언어 runner를 구분하는 짧은 이름을 쓴다.
`<selector>`는 `all`, 단일 시나리오 ID, 또는 언어별 runner가 허용하는 쉼표 구분 시나리오 목록이다.
C++처럼 같은 config를 여러 start order로 반복하는 runner는 config 시작·완료 라인에
`start_order=<variant>`를 함께 기록한다.

실패하면 같은 위치에 `FAIL (<seconds>s, attempt <n>)`를 출력한다. bind 충돌처럼 재시도 대상인
실패는 재시도 안내를 한 줄 출력한 뒤 같은 config를 다시 실행한다. Redis 시작 로그, log directory,
개별 server stdout/stderr 같은 진단 출력은 이 요약 라인 사이에 나올 수 있다.

개별 config runner는 client 시나리오 진행 상황을 콘솔에 실시간으로 흘려보내야 한다. 로그 파일에만
쓰고 마지막에 한꺼번에 보여 주면 멈춘 것처럼 보이므로 표준으로 보지 않는다. 파일 로그가 필요하면
`tee`처럼 콘솔 출력과 파일 저장을 함께 만족하는 방식으로 처리한다.

`Ctrl-C`, `TERM`, 정상 종료 모두 같은 정리 경로를 사용한다. 집계 runner는 종료 시 자신이 시작한
현재 config 하위 프로세스만 정리한다. Redis는 개별 config runner가 자신이 만든 container id만
정리한다. 정리 함수는 여러 번 호출돼도 안전해야 한다. 중단 시에는 예를 들어 아래처럼 한 줄로
정리 중임을 알린 뒤 종료한다.

```text
[<language>-e2e] interrupted; stopping the current configuration...
```

### 2.2 언어별 포팅 단위

다른 언어에 e2e를 추가할 때는 config 하나를 작은 테스트 파일 묶음으로 보지 말고 독립 실행 배포
묶음으로 구현한다. 한 config를 구현할 때 필요한 기본 산출물은 아래와 같다.

- `Shared/`: server와 client가 함께 쓰는 request/reply/event/evidence DTO만 둔다.
- `Server/<Role>/`: provider, consumer, publisher, subscriber, play, session처럼 실제
  배포에서 구분되는 역할마다 하나의 실행 앱을 둔다. 같은 역할의 복제본은 같은 프로젝트를 여러
  번 띄워도 되지만, 서로 다른 역할은 프로젝트와 폴더를 분리한다.
- `Server/<Role>/Configuration/`: 해당 role의 실행 옵션과 인자 해석을 둔다.
- `Server/<Role>/Endpoints/`: HTTP endpoint mapping을 둔다. client가 호출하는 app endpoint와
  evidence/wait/shutdown 같은 운영 endpoint를 여기에 포함한다.
- `Server/<Role>/Handlers/`: framework handler, route handler, observer처럼 framework runtime에
  등록되는 타입을 둔다.
- `Server/<Role>/Infrastructure/`: evidence store, role 내부 상태 저장소처럼 endpoint와 handler가
  함께 쓰지만 public 메시지 계약은 아닌 구현을 둔다.
- `Client/Program.*`: 실행할 시나리오 목록을 선언하고, 옵션에 따라 전체 또는 단일 시나리오를
  순서대로 호출한다.
- `Client/Scenarios/<ScenarioId><Name>Scenario.*`: 시나리오 ID 하나마다 파일 하나를 둔다.
- `Client/Support/`: option parsing, assertion, process manager, evidence wait처럼 여러 시나리오가
  공유하는 보조 코드만 둔다.
- `run_e2e.*`: build, 포트 할당, 로그 디렉토리 생성, 서버 프로세스 시작·종료, client 실행,
  실패 시 로그 위치 출력까지 담당한다.
- `feature-map.ko.md`: 구현한 시나리오, 미구현 시나리오, public contract gap, harness gap을
  config 문서의 시나리오 ID와 맞춰 기록한다.

언어별 파일 확장자나 프로젝트 파일 이름은 자연스럽게 바꿔도 된다. 하지만 위 역할 경계와 파일
분류가 바뀌면 언어별 e2e 결과를 서로 비교할 수 없으므로 완료로 보지 않는다.

### 2.3 역할 서버와 endpoint 형태

역할 server는 사용자가 배포하는 app을 작게 만든 것이다. client가 호출하는 endpoint도 이 관점에서
정한다.

- endpoint는 실제 사용자가 일으키는 동작을 표현한다. 예: publish trigger, request submit,
  subscribe/bind, admin `Relocate`·`Shutdown`, socket weight 부하 제외·복원, evidence wait, topology wait.
- endpoint 하나가 여러 시나리오를 내부에서 실행하고 결과만 돌려주면 안 된다. `/run`,
  `/scenario/all`, `/execute`처럼 client 검증을 server에 위임하는 endpoint는 금지한다.
- endpoint 내부에서는 해당 언어 framework의 공개 API를 사용한다. private API, raw frame 조작,
  reflection 우회, test-only adapter를 쓰지 않는다.
- evidence endpoint는 역할 server가 실제로 처리한 marker를 노출한다. 시나리오 실행 전용 server가
  만든 marker만으로 성공을 판정하지 않는다.
- 값이 바뀌기를 기다려야 하면 역할 server에 bounded wait endpoint를 둔다. 예: `/evidence/wait`,
  `/topology/wait`, `/admin/weight/wait`. client가 같은 GET을 수십 번 반복해 값 변화를 관찰하는
  방식은 쓰지 않는다.
- stream, subscription, monitoring event처럼 server가 push할 수 있는 흐름은 client stream connector를
  먼저 연결하고 push payload로 검증한다. HTTP는 상태 변경을 일으키는 trigger로만 쓴다.
- 다만 Pub/Sub fanout처럼 event의 수신자가 client가 아니라 subscriber 역할 server인 config는
  subscriber server의 bounded evidence wait를 사용한다. client stream connector로 별도 observer를
  추가해 subscriber 역할을 우회하지 않는다.

### 2.4 서버 프로젝트 구성 규칙

- 서버 역할이 다르면 `Server/<Role>/` 아래에 별도 실행 프로젝트로 둔다. 하나의 서버 프로젝트를
  `--role`, `--mode` 옵션으로 publisher/subscriber 또는 정상/오류/peer 서버처럼 바꾸지
  않는다.
- 역할별 프로젝트 안에는 해당 역할 코드만 둔다. 예를 들어 `Server/Provider` 프로젝트 안에
  publisher/subscriber/play/session/multi-node 분기와 handler가 함께 들어 있으면 안 된다. 같은
  `Program.cs`를 여러 역할 프로젝트에 복사한 뒤 default role만 바꾸는 방식도 금지한다.
- `Server/<Role>/`의 `<Role>`은 실제 배포에서 의미가 있는 역할이어야 한다. 이름을 `Main`,
  `Coordinator`, `Control`, `Scenario`처럼 바꿔도 시나리오 실행만 대신하고 실제 기능을 제공하지
  않는 server라면 만들 수 없다.
- `Program.cs`는 실행 진입점만 둔다. host 구성, DI 등록, framework 설정은 `*HostFactory.cs`에 둔다.
- `AddZLinkFramework` 설정과 Store 등록은 `*HostFactory.cs`에서 바로 보이게 작성한다.
  Location capability는 `AddLocationStore(instance)`, Relocation capability는
  `AddRelocationStore(instance)`로 각각 등록한다. Redis 전용 또는 두 capability를 묶는 등록 함수는
  사용하지 않는다([05 §10](../spec/06-framework-api.ko.md#10-location-store와-relocation-store)).
  얇은 wrapper/extension 메서드 뒤에 framework 설정을 숨기지 않는다.
- `Server/Driver`, `Server/TestRunner`, `Server/ScenarioRunner` 같은 별도 실행 프로젝트는 만들지
  않는다. 폴더 이름이 다르더라도 시나리오 실행만 위임받는 server는 같은 금지 대상이다. 테스트
  진행을 위해 프로세스 시작·종료가 필요하면 `run_e2e.*`와 client support 코드에서 다루고,
  framework 기능 호출은 실제 역할 server endpoint 안에 둔다.
- e2e 서버는 작은 실행 예시이지만, 다른 언어가 같은 방식으로 따라가기 쉽도록 파일 성격별 폴더를
  유지한다. `Configuration/`, `Endpoints/`, `Handlers/`, `Infrastructure/`
  같은 폴더를 둔다. 지금은 파일이 하나뿐이어도 그 역할이 분명하면 같은 폴더에 둔다. 이렇게 해야
  언어별 구현을 비교할 때 "옵션 파싱", "HTTP 표면", "framework handler", "evidence 저장" 위치를
  바로 찾을 수 있다.
- 같은 성격의 파일을 프로젝트 루트와 하위 폴더에 섞어 두지 않는다. 예를 들어 일부 endpoint는
  `Program.*`에 두고 일부 endpoint는 `Endpoints/`에 두는 식의 혼합 구조는 피한다.
- `Program.*`에 endpoint, handler, framework 설정을 길게 넣지 않는다. 실행 진입점은
  `RoleHostFactory.Create(args).Run()`에 해당하는 수준으로 유지하고, 실제 host 구성은
  `*HostFactory.*`로 분리한다.
- 공통 서버 library/shared 프로젝트는 기본으로 만들지 않는다. 중복이 조금 생기더라도 각 서버
  프로젝트가 자기 구성을 직접 드러내는 쪽을 우선한다. 정말 여러 config 또는 여러 역할에서 같은
  코드가 반복되어 유지 비용이 커질 때만 별도 shared 프로젝트를 검토한다.
- `Shared/`는 server와 client가 함께 쓰는 메시지·계약 타입만 둔다. server-only host factory,
  handler, filter, evidence store를 config의 top-level `Shared/`에 넣지 않는다.

### 2.5 client 프로젝트 구성 규칙

- client는 `Client/Program.cs`에서 시나리오를 순차 실행한다.
- `Client/Program.cs`에는 옵션 파싱, HTTP client 생성, scenario 호출 순서만 둔다. 개별 scenario의
  요청·검증 본문은 `Program.cs`에 두지 않는다.
- 시나리오는 `Client/Scenarios/` 아래에 scenario별 파일로 분리한다.
- 각 scenario 파일 첫머리에 해당 시나리오가 무엇을 검증하는지 설명한다.
- client support 코드는 option parsing, assertion, process lifecycle처럼 시나리오 흐름을 보조하는
  것만 둔다. framework 호출을 감추는 helper를 만들어 client가 server app을 우회하게 하지 않는다.
- 조건 확인, 예상 오류와 timeout 검증 같은 범용 assertion helper는 `Client/Support`가 소유한다.
  connector package의 production public API에 같은 helper를 추가하지 않는다.
- client는 server app endpoint를 언어별 HTTP client wrapper로 호출한다. server evidence endpoint와
  log marker는 검증에 사용할 수 있지만, framework 내부 상태나 private/test-only API를 직접 읽지
  않는다.
- 값 변경·event·push 수신을 기다리는 시나리오는 polling 전용 HTTP loop 대신 client stream
  connector를 사용한다. HTTP endpoint는 bind, subscribe, state-change trigger처럼 사용자가 실제로
  일으키는 동작을 표현하고, 변화가 도착했는지는 connector push 수신으로 단언한다. connector가 없는
  언어 또는 아직 public contract가 없는 기능은 feature-map에 gap으로 남긴다.
- Pub/Sub fanout 시나리오는 예외다. 해당 config는 subscriber 역할 server가 실제 수신자이므로,
  subscriber handler evidence를 bounded wait endpoint로 확인한다. 이 예외는 subscriber 역할 server의
  실제 dispatch marker에만 적용되며, 시나리오 실행 전용 driver evidence에는 적용하지 않는다.
- client scenario는 실제 역할 server endpoint 호출과 검증 흐름을 직접 보여야 한다. driver의
  `/run` endpoint 하나를 호출하고 "나머지는 server 쪽에서 알아서 검증"하게 만들면 안 된다.
  evidence 조회도 실제 역할 server에서 가져와야 하며, 시나리오 실행 전용 server의 결과만 읽으면
  안 된다.
- 시나리오가 여러 개이면 client scenario 파일도 여러 개로 나눈다. 여러 시나리오를 하나의
  `AllScenario`, `ScenarioSet`, `DriverScenario` 파일로 묶어 driver에 위임하지 않는다.
- config 문서의 시나리오 ID 하나는 client scenario 파일 하나와 대응해야 한다. 예를 들어 `RC-B1`,
  `RC-B2`, `RC-B3`, `RC-B4`는 하나의 codec 묶음 파일이 아니라 각각 독립 파일로 둔다. 공통 endpoint가
  같은 응답을 반환하더라도 client 검증 단위는 나누어야 한다.
- 여러 시나리오가 같은 server endpoint를 호출해도 된다. 단 각 scenario 파일은 자기 ID가 확인해야
  하는 reply, push, topology, evidence 조건만 직접 단언한다.

### 2.6 설정 전달 계약

모든 언어의 E2E는
[Sample/E2E 설정 정책](../sample-e2e-configuration-policy.ko.md)을 필수로 따른다. 개별 config
runner는 실행별 role 설정 파일을 생성하고 framework host에는 설정 파일 경로만 전달한다.
Framework host가 아닌 standalone client는 직접 연결하는 endpoint, 요청 timeout과 scenario
selector를 명시적인 CLI option으로 받되 시작할 때 한 번 검증한다. Endpoint, Redis, routing id,
timeout, 로그와 evidence 경로를 환경 변수나 JVM system property로 전달하지 않으며, server와
client 애플리케이션 코드에서 직접 사용할 수 있는 환경 변수는 0개다.

Scenario selector, process restart와 장애 주입 명령은 E2E 실행 제어 입력이므로 설정값과 구분한다.
Scenario selector는 standalone client에 전달한다. Process restart와 장애 주입은 runner option이나
client support process manager 명령으로 처리하며 framework host의 CLI로 전달하지 않는다.

환경 변수 interface를 예외 경로로 함께 제공하지 않는다. Framework host는 설정 파일과 typed
binding을 사용하고, standalone client는 검증된 CLI 입력 또는 필요한 경우 typed 설정 파일을
사용한다. 이 계약을 충족하지 못한 lane은 feature-map에 configuration gap으로 기록한다.

### 2.7 `run_e2e.*` 실행 계약

모든 언어의 실행 스크립트는 같은 사용 의미와 같은 Redis 구동 방식을 가져야 한다.

**필수 격리 규칙:** Redis가 필요한 각 E2E 실행은 그 실행만 사용하는 전용 Docker Redis
container를 새로 만들어야 한다. 이미 실행 중인 container, host Redis, 다른 E2E나 sample이 만든
Redis endpoint를 공유하거나 fallback으로 사용하면 안 된다. key prefix만 다르게 지정하는 것도
인스턴스 공유를 허용하지 않는다. pause, stop, restart, 지연 주입과 cleanup이 다른 실행에 영향을
주지 않게 하는 것이 이 규칙의 목적이다.

기준 템플릿은 이 디렉토리의 `runner-templates/` 아래에 둔다.

- `runner-templates/redis-common.template.sh`: Redis helper 기준
- `runner-templates/run_e2e.template.sh`: 개별 config e2e runner 기준
- `runner-templates/run_e2e_all.template.sh`: 통합 e2e runner 기준

- 기본 실행은 해당 config의 구현된 시나리오를 순차 실행한다.
- 개별 config runner는 단일 시나리오와 시나리오 리스트 실행을 지원한다. 예:
  `./run_e2e.sh RC-B2`, `./run_e2e.sh RL-A4`, `./run_e2e.sh RL-A4,RL-C2`,
  `./run_e2e.sh RL-A4 RL-C2`. 쉼표와 공백 인자는 같은 의미이며, runner는 이를
  client가 이해하는 하나의 scenario selector로 정규화해서 전달한다.
- 스크립트는 build → 로그 디렉토리 생성 → 서버 시작 → readiness 확인 → client 실행 → 서버 종료
  순서를 책임진다.
- 각 언어는 e2e runner들이 공유하는 Redis helper를 둔다. helper는 실행별 Redis container 시작과
  그 실행이 만든 container id 정리를 공통 함수로 제공하고, 개별 config script가 Docker 명령을
  직접 조합하지 않게 한다.
- readiness는 고정 sleep만으로 보지 않는다. 각 role server의 `/health`, 포트 open, 또는 명시 marker로
  확인한다.
- 실패 시 `log_dir=...`를 출력하고, 각 role server와 client의 stdout/stderr/framework log를 남긴다.
- 시나리오 선택은 client가 하되, client가 server-side scenario runner에 전체 실행을 위임하지 않는다.
- scale-out, restart, crash, store outage처럼 프로세스 제어가 필요한 경우는 스크립트나 client
  support process manager가 담당한다. framework request/send/publish 자체는 실제 역할 server endpoint
  내부에서만 수행한다.
- Redis location store가 필요한 config의 개별 `run_e2e.*`는 실행마다 전용 Docker Redis
  container를 새로 시작한다. 이미 떠 있는 Redis container나 host Redis endpoint를 재사용하면
  안 된다. Redis key prefix가 달라도 장애 주입, pause/stop/restart, flush, cleanup, latency
  injection이 다른 실행에 영향을 줄 수 있기 때문이다. 실행 종료 시에는 자신이 만든 container
  id만 정리한다. 개별 script가 같은 prefix의 다른 Redis container를 지우면 안 된다.
- Docker Redis를 만들지 못하면 runner는 즉시 실패한다. host Redis나 다른 실행의 endpoint로
  자동 전환해서 성공 처리하면 안 된다.
- Redis container 시작은 모든 언어에서 같은 순서를 쓴다.
  `docker create --name <scoped-name> --tmpfs /data -p 127.0.0.1::6379 <pinned-redis-image>`로 container를
  만들고, `docker start <container-id>`로 시작한 뒤, `docker inspect`로 실행 상태와 배정된
  host port를 읽는다. `docker run -d` 출력에 의존해 container id와 port를 동시에 처리하는
  방식은 쓰지 않는다.
- E2E Redis 데이터는 실행 중에만 필요하므로 Docker volume을 만들지 않는다. Redis 이미지가
  선언한 `/data` volume은 `--tmpfs /data`로 덮어쓰고, container 정리에는 `docker rm -fv`를
  사용한다. 이렇게 해야 반복 실행 후 anonymous volume이 남지 않는다.
- Redis container 이름에는 언어와 e2e 실행 범위를 드러내는 prefix를 추가한다. 예를 들어 Java e2e는
  `zlink-redis-java-e2e...`, Kotlin e2e는 `zlink-redis-kotlin-e2e...`처럼 잡는다. 다른 언어도
  같은 규칙으로 `<language>-e2e` 범위를 이름에서 확인할 수 있어야 한다.
- 통합 e2e runner는 다른 실행의 Redis를 정리하지 않고 config별 개별 `run_e2e.*`를 순차 호출한다.
  한 통합 실행 안에서는 config를 병렬 실행하지 않지만, 같은 언어의 다른 개별 실행이나 통합 실행과
  자원을 공유하거나 제거해서는 안 된다.
- 통합 e2e runner도 실행 대상을 좁힐 수 있어야 한다. 인자가 없으면 모든 config의 `all`을 실행하고,
  인자가 있으면 지정한 config만 실행한다. config 안의 일부 시나리오만 실행할 때는
  `Config:ScenarioA,ScenarioB` 형식을 사용한다. 예: `./run_e2e_all.sh RegistrationCodec:RC-B2,RC-B4`
  또는 `./run_e2e_all.sh ResilienceLifecycle:RL-A4,RL-C2 PubSub:PS-A1`. 통합 runner는 이 선택
  정보를 해석만 하고, 실제 readiness, Redis endpoint 생성, server 시작, client scenario 실행은
  해당 config의 개별 `run_e2e.*`에 위임한다.
- 통합 e2e runner는 config별 내부 동작을 다시 구현하지 않는다. 선택한 개별 `run_e2e.*`를 호출하고,
  retry 여부와 최종 결과만 관리한다. Redis endpoint 생성, readiness,
  로그 위치, scenario 실행 세부 절차는 개별 config script와 공통 helper가 맡는다.
- Redis host port는 고정하지 않는다. Docker가 비어 있는 loopback port를 배정하게 하고, runner가
  inspect 결과로 endpoint를 얻어 각 role server와 client에 전달한다. Redis key prefix, routing id,
  log directory도 실행마다 고유해야 한다.
- 같은 host에서 다른 sample/e2e가 Redis를 사용 중이어도 그 endpoint를 빌려 쓰지 않는다. 새
  Docker Redis container를 만들고 Docker가 할당한 다른 loopback port를 사용해야 테스트 간섭을
  막을 수 있다.
- Docker 명령 자체는 짧은 timeout으로 감싸고, Redis readiness는 port/readiness 대기 함수로 따로
  확인한다. Redis 시작이 늦은 경우와 Docker CLI가 응답하지 않는 경우를 같은 sleep으로 처리하지 않는다.
- Redis helper가 실패하면 개별 runner도 즉시 실패해야 한다. shell runner에서는
  `read ... < <(redis_start_function)`처럼 process substitution 결과를 읽는 방식으로 container id를
  받지 않는다. 이 방식은 helper가 실패해도 `read` 자체는 성공할 수 있어 Redis 없이 서버를 시작하는
  잘못된 실행으로 이어진다. helper는 `zlink_redis_start_scoped_assign`처럼 호출부 변수에 값을
  대입하는 함수로 제공하고, 함수 실패가 그대로 runner 실패가 되게 한다.
- 통합 e2e runner는 transient bind 실패(`Address already in use`, `EADDRINUSE`,
  `already bound`, `errno=98`)만 제한적으로 retry할 수 있다. scenario assertion 실패, runtime semantic
  failure, native abort, store recovery 조건 미충족은 retry 대상이 아니며 원인 로그를 남기고 실패한다.

### 2.8 feature-map 작성 규칙

언어별 e2e에는 config별 `feature-map.ko.md`를 둔다. 이 문서는 skip 목록이 아니라 구현 상태와 gap의
근거를 남기는 표다.

- config 문서의 모든 시나리오 ID를 행으로 둔다.
- 상태는 `implemented`, `not-supported`, `blocked`, `deferred`처럼 명확히 쓴다.
- `not-supported`는 해당 언어의 public contract에 기능이 없다는 뜻이다. 이 경우 필요한 public API와
  관련 spec/guide 근거가 있는지 함께 적는다.
- `blocked`는 runtime bug, bindings bug, harness 부족처럼 해결해야 할 원인이 있는 경우에만 쓴다.
  버그를 피해 시나리오를 약하게 만들지 않는다.
- 미구현 항목이 있어도 P0이면 완료가 아니다. P1/P2는 해당 기능 지원 여부와 실행 비용을 함께 적는다.

### 2.9 주석 작성 규칙

- 시나리오 파일 첫머리에는 이 파일이 어떤 사용자 흐름과 어떤 framework 동작을 검증하는지 적는다.
  독자가 파일을 열었을 때 "이 시나리오가 왜 필요한가"를 바로 알 수 있어야 한다.
- 주석은 시나리오 의도, 검증 기준, 기다림이 필요한 이유처럼 코드만으로 드러나지 않는 판단을
  설명할 때만 쓴다. 코드가 하는 일을 그대로 반복하는 주석은 넣지 않는다.
- HTTP 호출, server evidence 조회, 프로세스 재시작처럼 시나리오의 핵심 단계에는 짧은 주석을 둘 수
  있다. 이 주석은 "무엇을 호출한다"보다 "이 단계가 어떤 실사용 조건을 만든다"를 설명해야 한다.
- helper나 support 코드의 주석으로 핵심 흐름을 대신 설명하지 않는다. 시나리오 본문만 읽어도
  connector 또는 HTTP client를 통해 어떤 요청을 보내고 무엇을 확인하는지 보여야 한다.
- public contract가 없어 구현하지 못한 항목은 주석으로 "지원됨"처럼 보이게 만들지 않는다. 해당
  항목은 feature-map 또는 이슈로 남기고, 주석에는 현재 검증하는 공개 동작만 적는다.

## 3. config 목록

각 config는 현실적인 서버 구성 하나를 단위로, 그 위에서 messaging·연결·spot·codec 등 세부
동작을 검증한다.

| Config | 서버 구성 | 다루는 것 |
|--------|-----------|-----------|
| [Config 1 — Location messaging](config-1-location-messaging.ko.md) | Location Store + Channel provider 2 + Object Server 2 + Object Client 2 | Public RouteMesh status, automatic·manual topology, Channel provider 선택, request·send, timeout·backpressure와 global object identity 충돌 |
| [Config 2 — Spot 서비스](config-2-spot-service.ko.md) | Location Store + Relocation Store + Play node 2 + Session gateway 2 | Entry·User Spot, Actor create·Join, direct·Channel·multicast message, Session bind·relay·push, timer·close, crash와 scale-out |
| [Config 3 — Pub/Sub 이벤트](config-3-pubsub.ko.md) | Publisher + subscriber 3 + Location Store | Publisher discovery, topic filter, fanout, late subscriber, restart·Store 장애와 replay하지 않는 publish 의미 |
| [Config 4 — 등록·codec](config-4-registration-codec.ko.md) | Channel caller·provider | 언어별 handler 등록 방식, startup 검증, DI lifecycle, 기본 typed JSON과 root codec extension |
| [Config 5 — Resilience/lifecycle](config-5-resilience-lifecycle.ko.md) | 다중 node + Location Store | Restart·replacement·disconnect, terminal-once, hidden replay 금지, Relocate·Shutdown, capacity와 lifecycle 경합 |
| [Config 6 — Store 장애·복구](config-6-store-failure-recovery.ko.md) | Location·Relocation Store + provider 2 + consumer | Store 장애 중 public failure, owner 무효화와 복구, relocation 결과, capacity reservation의 사용자 관찰 결과 |
| [Config 7 — Monitoring](config-7-monitoring.ko.md) | Location Store + service 2 | Public RouteMesh·host status, topology 변화, Store 장애 반영, 느린 observer 격리와 bounded snapshot |
| [Config 8 — Execution turn](config-8-execution-turn.ko.md) | Play node 2 + worker service 2 + gateway | Spot·Actor serial execution, Yield·worker, deferred operation, timeout·cancellation·shutdown과 언어별 parity |
| [Config 9 — To-actor messaging](config-9-to-actor-messaging.ko.md) | Actor node 2 + Session gateway 2 + caller | Bind와 독립된 Actor ID send·request, Actor 재생성, stale location과 route failure의 공개 결과 |
| [Config 10 — Spot actor join/relocation](config-10-spot-actor-relocation.ko.md) | Location·Relocation Store + Actor node 2 + Session gateway 2 + caller | Local·remote Join, state와 이동 중 message 순서, Session binding route 갱신, Message Follow, PerActor·SpotWide relocation |
| [Config 11 — 관측·운영 배포](config-11-observability-ops.ko.md) | Session + Play 2 + workflow 2 + Stores | Public flow correlation·metrics, maintenance Relocate·Shutdown, patch와 drain의 client·application 결과 |
| [Config 12 — Channel egress routing](config-12-channel-egress-routing.ko.md) | Session·Play·API + ClientServer service 2 | ChannelName routing, local egress 선택, weight·shutdown·restart와 request·send terminal |
| [Config 13 — One-way submit admission](config-13-submit-admission.ko.md) | RouteMesh·ClientServer·Spot·Actor·Stream targets | One-way admission completion, timeout·cancellation·shutdown 경합, zero target, ordering과 hidden retry 금지 |
| [Config 14 — Instance Spot activation](config-14-instance-spot.ko.md) | Location·Relocation Store + caller 2 + owner 2 + User Spot owner | Cold activation, concurrent first call, first-message ordering, crash·deadline·capacity·relocation과 cross-language 결과 |

## 3.1 구성 축 — config를 관통하는 변형

같은 시나리오라도 서버 구성의 특정 조합에서만 드러나는 결함이 있다. 이전 topology를 단일
MeshNode로 합치는 검증에서 여러 언어 framework의 결함이 발견됐는데, 전부 "기능 자체는
e2e가 있었지만 그 구성 조합을 아무도 돌리지 않았던" 경로였다. 이를 막기 위해 config의
핵심 시나리오는 아래 축의 변형으로도 검증한다.

| 축 | 변형 | 주로 걸리는 config | 실제 발굴 사례 |
|----|------|--------------------|----------------|
| MeshNode 구성 | 하나의 RouteMesh와 location descriptor만으로 ChannelName·Spot·Actor를 함께 구성 | Config 2, 9 | 원격 actor join이 별도 channel 또는 Spot transport를 전제하던 구현 |
| 배치 | 세션과 spot을 **다른 프로세스로 분리** (colocated 변형과 쌍) | Config 2, 9 | colocated에서는 로컬 join이라 원격 relay 경로가 실행조차 안 됨 |
| rid 방향 | 요청자가 auto-connect **non-initiator**가 되도록 rid 사전순을 뒤집은 변형 | Config 1, 2, 9 | non-initiator의 spot 응답 correlate 누락(recv pump) — rid를 뒤집어야만 재현 |
| peer 수 | 한 발신자가 **2개 이상 노드로 연속 요청** | Config 1, 2 | 두 번째 peer의 응답 drain 누락 |
| 기동 순서 | 서버 역할의 **기동 순서를 뒤바꾼** 변형(의존 역방향 기동) | Config 1, 2, 9 | 특정 기동 순서에서만 나타나는 연결 수렴 레이스 — 순서가 고정된 러너에서는 재현 불가 |

축 변형은 시나리오를 새로 쓰는 게 아니라 같은 client 시나리오를 서버 topology만 바꿔
다시 돌리는 것이다. 모든 조합을 다 돌릴 필요는 없고, config별 P0 시나리오에 대해
"route mesh 없음 × 분리 배치" 조합을 우선 적용한다(발굴 결함의 대다수가 이 조합).

### 기동 순서 축의 절차

기동 순서 결함은 순서가 고정된 러너에서는 재현되지 않으므로, 러너가 순서를
바꿔 돌 수 있어야 한다.

- config 러너는 서버 역할 기동 순서를 인자(예: `E2E_START_ORDER=reverse|shuffle:<seed>`)로
  받는다. 기본은 정방향(의존 순서), 축 변형은 **역방향 전체 1회 + 고정 seed shuffle 1회**를
최소 범위로 실행한다. seed를 기록해 실패 조합을 재현 가능하게 한다.
- 어떤 순서로 떠도 **결과는 같아야 한다**: 각 역할이 자기 의존(store, peer)이 늦게 떠도
  발견·연결이 수렴하고, 수렴 직후 첫 요청이 재시도 없이 성공한다. 순서에 따라 되고
  안 되고가 갈리면 그 자체가 결함이다.
- 클라이언트 시작은 모든 역할의 readiness 이후로 고정한다 — 이 축이 검증하는 것은
  서버 간 상호 발견의 순서 무관성이지, 클라이언트의 조기 접속이 아니다(그건 "수렴
  직후 첫 요청" 요구가 따로 본다).

### 축과 별개로 모든 config가 지켜야 하는 검증 요구

- **계약 round-trip**: framework 공개 타입(routing id, actor ref snapshot 등)이 channel·spot·
  stream 표면을 넘을 때 값이 보존되는지 어서션한다. 응답에 실린 actor ref는 concrete해야
  한다(node rid 비어 있지 않음, generation > 0). — 직렬화 누락은 송신 측 로그에는 값이
  보이므로 수신 값 어서션 없이는 잡히지 않는다.
- **silent-drop 금지**: 등록되지 않은 handler로 향한 send/request는 조용히 버려지지 않고
  관측 가능한 실패(오류 응답 또는 로그 marker)를 남겨야 한다. 무응답 timeout으로만
  나타나는 유형은 진단 비용이 가장 크다.
- **소유 일관성**: 상태를 만드는 요청(start)과 이후 요청(continue)이 다른 노드로 가는
조합을 명시적으로 실행해, 소유권 위반이 fail-fast로 분류되는지와 owner 일관 라우팅이
  이를 예방하는지 본다. 소유가 해시 기반이면 실패가 간헐이므로 반복 횟수를 늘린다
  (3연속 통과로는 부족했던 사례 있음).
- **수렴 직후 첫 요청**: location 발견·dial 수렴 직후 settle 지연 없이 즉시 첫 요청을
  보낸다. 재시도나 sleep으로 가리지 않는다 — 첫 요청이 바로 성공하거나 fail-fast로
  분류되는 것 자체가 검증 대상이다.
- **인프라 게이트**: location store가 필수인 config는 store 없는 빌드/구성에서 조용히
  미연결로 돌지 않고 구성 시점에 실패해야 한다. 러너 스크립트는 표준 도구만 쓴다
  (미설치 도구 의존으로 판정 루프 전체가 무효가 된 사례 있음).
- **다단 push 사슬**: 서버 내부에서 role 경계를 두 번 이상 넘어(channel request → actor
  send → bound session push) 최종적으로 client stream에 도달하는 사슬을 끝까지 어서션한다.
  발신 role의 성공 로그만으로는 중간 hop의 미등록·미연결이 드러나지 않는다 — client가
  실제로 push를 받았는지가 유일한 성공 기준이다.

## 4. 우선순위

| 우선순위 | 의미 | 구현 기준 |
|----------|------|-----------|
| `P0` | config의 핵심 기능을 주장하려면 반드시 있어야 하는 검증 | 모든 언어에서 구현한다 |
| `P1` | 특정 기능을 지원한다고 문서화한 언어가 통과해야 하는 검증 | 지원 언어에서 구현한다 |
| `P2` | 운영 규모·rolling update처럼 비용이 큰 검증 | release gate에 선택 적용, 미구현 이유를 남긴다 |

축 변형(§3.1)의 우선순위: "route mesh 없음 × 분리 배치"는 Config 2·9의 P0 시나리오에
`P0`으로, rid 방향·다중 peer 변형은 `P1`로 적용한다.

## 5. 공통 실행 원칙

- 테스트는 독립된 임시 작업 디렉토리와 로그 디렉토리를 쓴다.
- 서버 프로세스는 config가 선언한 역할대로 시작한다. 공유 location store가 필요한 config는
  실행 전에 store(Redis 등)를 준비하거나 별도 프로세스로 시작하고, 실행 후 자신이 만든 store
  process 또는 container를 정리한다.
  multi-process config의 공유 저장소는 공식 Redis extension을 기본으로 한다. 단일 process
  smoke에서 위치 조회가 필요하면 process-local `IZLinkLocationStore` 구현체를
  `AddLocationStore(instance)`로 등록할 수 있다.
- port, routing id, Redis key prefix, 저장소 경로는 실행마다 격리한다. Docker Redis를 쓰는
  runner는 언어·e2e 범위 prefix로 container를 만들고 자신이 만든 container id만 정리한다.
  통합 runner도 같은 prefix의 다른 실행 container를 제거하지 않는다.
- 서버 준비 여부는 sleep만으로 판단하지 않고, 포트 readiness 또는 readiness marker로 확인한다.
- 성공 기준은 client 반환값, client stream connector가 받은 push, server의 public application evidence와
  정식 public flow·metric record를 조합한다. 일반 diagnostic log는 실패 조사 자료이며 성공 조건이 아니다.
  Location Store를 쓰는 config는 public RouteMesh status와 Actor·Spot manager의 resolve 결과도 성공
  기준에 넣는다. Application과 E2E client는 Store provider record를 직접 읽거나 해석하지 않는다.
- 실패하면 각 프로세스의 stdout/stderr, framework 로그, client 마지막 요청 정보를 남긴다.
- 실패 시 먼저 원인 레이어를 분리한다. `core-capi`, `bindings`, `framework`, `sample`, 테스트 실행
  스크립트 중 어디인지 evidence로 판정하고, 고친 레이어에 회귀 테스트를 둔다. framework 테스트를
  통과시키려고 C API나 bindings 버그를 framework에서 우회하지 않는다.
- 같은 시나리오는 언어별 public API 모양만 달라지고, 의미와 marker는 같아야 한다.

## 6. 로깅과 메시지 흐름 추적 (필수 공통)

모든 e2e는 **파일 로깅을 반드시 켜고**, 일반 시나리오에서는 메시지 흐름 추적도 켜서
작성·디버깅한다. ad-hoc `printf`나 콘솔 스크롤로 대신하지 않는다. 트레이싱은 "메시지가 도착했나 /
핸들러로 갔나 / 응답이 나갔나"를 표준 기능으로 기록하므로 테스트의 1차 디버깅 도구로 쓴다.
Tracing의 `off` 동작과 실행 중 level 변경을 검증하는 시나리오는 해당 구간에 한해 추적을 끈다.
(기능 스펙: [메시지 흐름 추적과 dispatch 관측](../spec/26-message-flow-tracing.ko.md))

### 6.1 모든 로그를 파일로 (`log/` 폴더)

- 각 서버/호스트·client 프로세스는 모든 framework 로그를 **실행별 `log/` 폴더 아래 파일**로
  출력한다. 콘솔 출력만으로 끝내지 않는다.
- 로그 디렉토리는 실행마다 격리하고(§5), VCS에서 제외한다(`.gitignore`). (C++ Bingo 예:
  `samples/Bingo/logs/`, `run_sample.sh`가 `BINGO_LOG_DIR`를 export.)
- 파일 sink는 부모 디렉토리를 자동 생성하는 API를 쓴다(C++ `app.logging().use_file(...)`/
  `use_rotating_file(...)`; `.NET`/Java/Node도 동일 의미 옵션). 디렉토리가 없다고 조용히 실패하면
  안 된다.
- 프로세스마다 파일을 분리해(예: `provider-a.log`, `play-a.log`, `session-a.log`, `client.log`)
  어느 노드 로그인지 바로 보이게 한다.

### 6.2 메시지 흐름 추적 켜기 (디버깅 1차 도구)

- e2e 실행 시 message flow 모드를 **최소 `key_transitions`**로 켠다. 그러면 한 메시지의
  인바운드(`received`→`dispatched`/`replied`)와 아웃바운드(`sent`→`reply_received`)가 한 줄씩
  찍힌다. 실패(`dropped`/error)는 같은 stream에 같은 `corr=`로 찍혀, 정상·실패가 하나의 흐름으로
  읽힌다.
- 로그 라인 토큰: `zlink flow: phase=… surface=… kind=… packet=… channel=… topic=… corr=…
  src=… spot=… actor=… [size=]`.
- **`corr=<id>`로 grep해 한 요청의 생애주기를 추적**한다. 노드 간에는 corr이 전파되는 경로
  (channel request↔reply, stream request↔reply echo, route 전파)에서 이어진다. (주의: corr은
  프로세스 전역 단조값이라 노드별 카운터가 독립이다 — 숫자만 같고 다른 메시지일 수 있으니, 노드 간
  연결은 corr이 실제 전파되는 경로에서만 신뢰한다. spot 구독/actor/publish 경로는 corr 대신
  spot/actor id로 키잉된다.)
- 트레이싱 로그는 앱 로그와 한 파일로 통합하거나(앱 로거 sink) 전용 파일로 분리할 수 있다
  (C++ `diagnostics.log_file`). 어느 쪽을 쓰든 §6.1대로 **파일로 남긴다**.
- 실행 중 level 변경은 public runtime control로 수행한다. 일반 e2e 캡처는 최소
  `key_transitions`로 두고, Config 11 `OBS-A5`는 `off` 전환 뒤 trace 전용 flow 정보와 log message가
  생성되지 않는지 확인한 다음 다시 켠다.
- 트레이싱은 **관측이지 제어가 아니다.** 켜도 기능 동작·성공 기준이 달라지면 안 되고, observer/trace
  실패가 메시지 처리나 테스트 판정을 바꾸면 안 된다.

### 6.3 실패 evidence에 포함

- 시나리오 실패 시 §5의 stdout/stderr·client 정보에 더해 위 **파일 로그(흐름 추적 포함)**를
  evidence로 남긴다. 원인 레이어 분리(`core-capi`/`bindings`/`framework`/`sample`/테스트 실행
  스크립트)도 `corr=` 흐름으로 먼저 좁힌다.

### 6.4 언어별 적용 기준

파일 로깅(§6.1)은 모든 언어에서 필수다. message flow tracing을 지원하는 언어는 §6.2의 증거도
남겨야 한다. 아직 tracing 계약을 구현하지 않은 언어는 feature map에 해당 증거의 누락과 적용
조건을 기록하며, 다른 로그를 tracing 증거로 대신하지 않는다.

## 7. 시나리오 ID 규칙

ID는 `config 접두사 - 트랙 - 번호`를 쓴다. 예: `RM-A1`(Location messaging, Track A, 1번).
Config 1은 `RM` 접두사를 사용한다.

| 접두사 | config |
|--------|--------|
| `RM` | Location messaging |
| `SM` | Spot messaging |
| `PS` | Pub/Sub |
| `RC` | 등록·codec |
| `RL` | Resilience/lifecycle |
| `SF` | Store 장애·복구 |
| `MON` | Monitoring |
| `TD` | 실행 turn과 terminator |
| `TA` | To-actor messaging |
| `ST` | Spot·Actor relocation |
| `OBS` | Observability와 운영 제어 |
| `CH` | Channel egress routing |
| `SA` | One-way submit admission |
| `IS` | Instance Spot |

테스트 이름은 언어 관례에 맞게 바꿔도 되지만, 리포트에는 config id와 시나리오 id가 드러나야 한다.

## 8. 완료 기준

- 각 config의 `P0` 시나리오는 모두 구현되어야 한다.
- `P1`은 해당 기능을 지원한다고 문서화한 언어에서 구현되어야 한다.
- 지원하지 않는 기능은 test skip이 아니라 feature map과 문서에 이유가 있어야 한다.
- 실패 경로는 client가 받은 결과와 실제 역할 server가 남긴 로그 또는 evidence를 함께 검증한다.
- 실패 원인 분류가 끝나지 않은 상태에서 workaround를 넣은 테스트는 완료로 보지 않는다.
