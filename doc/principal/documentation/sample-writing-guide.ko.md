# 샘플 문서 작성 가이드

> 이 문서는 Framework 공통 sample 문서를 작성하거나 수정하는 작업자가 따르는 절차다.
> 주 독자는 문서를 작성하는 AI와 사람 작업자다. 이들이 만드는 sample 문서는 구현하거나 review하는
> 개발자가 시스템 구조, 업무 흐름과 언어별 공통 기준을 파악할 수 있어야 한다.
>
> 목표는 처음 읽는 개발자가 **어떤 업무 문제를 보여 주는 sample인지 이해하고**, 각 언어
> 구현자가 **같은 역할·message·흐름·검증 기준으로 구현할 수 있게 하는 것**이다. 기존 문서의
> 절 제목을 기계적으로 맞추는 것이 목표가 아니다.

[기술문서 작성 원칙](documentation-principles.ko.md) ·
[사용자 가이드 문서 작성 가이드](guide-writing-guide.ko.md) ·
[E2E 시나리오 문서 작성 가이드](e2e-scenario-writing-guide.ko.md) ·
[Framework 공개 계약 관리](../../../framework/doc/framework/common/spec/00-public-contract-governance.ko.md) ·
[Framework 공통 sample](../../../framework/doc/framework/common/sample/README.ko.md)

## 1. 샘플 문서의 역할

공통 sample 문서는 정식 public contract를 실제 업무 흐름에 적용한 **언어 중립 구현 기준**이다.
사용자는 sample을 통해 Framework가 어떤 책임을 맡는지 확인하고, 언어별 구현자는 같은 업무
결과를 자기 언어의 public API로 재현한다.

샘플 문서는 다음 내용을 소유한다.

- Sample이 해결하는 업무 문제와 범위
- Process와 server 역할, 연결 관계와 상태 소유자
- 언어가 달라도 유지해야 하는 message 이름, field와 의미
- 정상·실패·복구를 포함한 업무 흐름
- Client self-check, smoke 실행 순서와 완료 기준
- 언어별 구현이 유지해야 하는 공통 의미

샘플 문서는 새 public API나 새 공개 동작을 정하지 않는다. 다음 표에 따라 다른 문서의 책임과
구분한다.

| 문서 | 답하는 질문 |
|---|---|
| 정식 spec | Application이 의존할 수 있는 공개 동작과 제약은 무엇인가? |
| 언어별 exact interface | 해당 언어에서 어떤 signature로 계약을 호출하는가? |
| 사용자 guide | 사용자가 기능을 언제 선택하고 어떻게 사용하는가? |
| 공통 sample 문서 | 여러 기능을 실제 업무 흐름에서 어떻게 조합하는가? |
| 언어별 sample code | 공통 흐름을 해당 언어의 public API로 어떻게 구현하는가? |
| E2E 문서 | 어떤 배포 조건과 evidence로 public contract를 검증하는가? |

공통 guide는 기능의 의도와 표준 사용 pattern을 확인하는 근거다. Public 동작과 언어별 signature는
공통 spec과 exact interface에 고정되어 있어야 한다. Sample에 필요한 기능이 정식 계약에 없으면 기대
API를 만들어 넣지 않는다. 계약 후보를 별도 설계 대상으로 분리하고, 확정 전에는 구현 차이 또는
feature map에 gap으로 남긴다.

## 2. 완료된 문서가 답해야 하는 질문

완성된 sample 문서는 관련 code를 먼저 읽지 않아도 다음 질문에 답할 수 있어야 한다.

| 질문 | 문서가 제공해야 하는 답 |
|---|---|
| 무엇을 보여 주는가? | Sample 하나를 선택해야 하는 업무 문제와 정상 결과 |
| 어디까지 다루는가? | 시작 지점, 종료 지점과 명시적으로 제외한 범위 |
| Framework가 무엇을 맡는가? | Routing, lifecycle, state ownership, timer와 push 중 실제 사용 요소 |
| Application이 무엇을 맡는가? | 도메인 판단, 외부 저장소, 보상과 idempotency 같은 application 책임 |
| 어떤 역할이 필요한가? | Process 수, 각 역할의 책임과 분리 이유 |
| 상태는 어디에 있는가? | 상태별 단일 소유자와 저장·복구 방법 |
| Message는 어떻게 이동하는가? | 발신자, 수신자, 호출 방식, 주요 field와 응답 의미 |
| 어떻게 확인하는가? | Client가 확인할 결과, runner 순서, readiness와 완료 marker |
| 다른 언어는 무엇을 유지하는가? | 이름·field·업무 순서·최종 결과 중 공통으로 고정할 항목 |

목적과 범위까지 읽으면 sample이 보여 주는 업무 문제, 적용 범위와 Framework의 책임을 설명할 수
있어야 한다. 문서 끝까지 읽어야 sample의 목적을 알 수 있다면 구조를 다시 정리한다.

## 3. 작업 순서

### 3.1 계약과 공통 정책 확인

초안을 쓰기 전에 다음 순서로 근거를 확인한다.

1. Sample이 사용하는 공통 Framework spec과 package spec을 확인한다.
2. 언어별 exact interface에서 목표 public signature와 언어별 표현을 확인한다.
3. 공통 Framework guide에서 기능의 의도와 표준 사용 pattern이 spec과 일치하는지 확인한다.
4. [Framework 공통 sample](../../../framework/doc/framework/common/sample/README.ko.md)에서 topology,
   message 이름, porting, 설정, runner와 self-check 정책을 확인한다.
5. 현재 배포 package의 public export와 Application이 compile하는 public declaration의 source에서
   실제 사용 가능 여부를 확인한다.
6. 기존 언어별 sample code와 test는 실행 가능성과 언어 제약을 확인하는 증거로 사용한다.
7. 자료가 충돌하면 정식 spec과 exact interface를 목표 계약으로 유지하고, 현재 구현과의 차이는
   implementation gap으로 분리한다.

다른 언어 구현에만 있는 helper나 편의 API는 공통 sample 계약의 근거가 아니다. 반대로 정식
계약에 있는 기능이 한 언어에 없으면 공통 흐름에서 빼지 않고 public contract gap으로 기록한다.

### 3.2 한 문장으로 정리한 Sample의 주장

먼저 다음 문장을 채운다.

```text
이 sample은 ______ 상황에서 Framework가 ______을 맡아,
Application이 ______에 집중할 수 있음을 보여 준다.
```

빈칸에 기능 이름만 여러 개 들어가면 목적이 아직 좁혀지지 않은 것이다. 사용자가 겪는 문제,
Framework가 흡수하는 복잡성, Application에 남는 책임이 드러나야 한다.

예를 들어 actor, timer와 push를 모두 사용한다는 사실은 기능 목록이다. 대화 상태를 한곳에서
순서대로 바꾸고 연결이 교체된 client에도 최신 session으로 알림을 보낸다는 설명은 사용자가
확인할 수 있는 결과다.

### 3.3 시작과 종료 범위

Sample이 어디에서 시작하고 어디에서 끝나는지 정한다. 전체 제품을 흉내 내려 하지 말고,
Framework 조합의 이점이 드러나는 최소 업무 구간을 선택한다.

다음 내용을 한 문단으로 적을 수 있어야 한다.

- 시작할 때 이미 존재한다고 가정하는 데이터와 연결
- Client가 처음 수행하는 operation
- 정상 흐름이 끝났다고 판단하는 업무 결과
- 다루지 않는 기능과 그 기능을 제외한 이유

제외 범위는 단순한 누락 목록이 아니다. 인증, 결제, UI 또는 운영 기능을 생략했다면 이 sample의
주장을 확인하는 데 왜 필요하지 않은지 설명한다.

### 3.4 업무 흐름과 Framework 요소의 대응

처음부터 `Actor`, `Spot`, `Channel` 이름으로 설명하지 않는다. 먼저 사용자가 수행하는 동작과
업무 상태 변화를 평이한 문장으로 쓴다. 그다음 각 책임을 맡는 Framework 요소를 연결한다.

```text
1. 사용자가 요청을 시작한다.
2. 상태를 소유한 역할이 같은 대상의 요청을 순서대로 처리한다.
3. 처리 결과를 저장하고 연결된 client에 알린다.
4. Client가 응답과 push의 값·순서를 확인한다.
```

이 흐름을 쓴 뒤에 “같은 대상의 상태를 소유하고 순서대로 처리하는 실행 단위로 User Spot을
사용한다”처럼 역할을 먼저 설명하고 이름을 붙인다.

### 3.5 역할과 상태 소유권

각 역할에는 하나의 주된 책임을 둔다. 역할 표를 만들기 전에 다음 질문에 답한다.

1. 외부 요청이나 stream 연결을 어느 역할이 받는가?
2. 업무 상태를 어느 역할이 변경하는가?
3. 각 상태의 단일 소유자는 누구인가?
4. 어떤 역할이 다른 process여야 sample의 경계가 드러나는가?
5. 공유 Store와 외부 service는 어떤 정보만 소유하는가?

같은 상태를 두 역할이 모두 변경한다면 조정 규칙이 문서 전체에 퍼진다. 먼저 책임을 한곳으로
모을 수 있는지 검토한다. 물리적으로 한 process에 배치할 수 있는 역할도 책임이 다르면 문서에서
구분한다.

### 3.6 Message와 상태 전이

Message를 나열하기 전에 각 업무 흐름의 시작 상태와 종료 상태를 정한다. 그 뒤 message마다 다음을
확인한다.

- 누가 보내고 누가 받는가?
- Request/reply, one-way send, push 또는 publish 중 무엇인가?
- 호출자가 어떤 결과를 기다리는가?
- Routing과 업무 판단에 꼭 필요한 field는 무엇인가?
- State가 어느 역할에서 어떤 값으로 바뀌는가?
- Timeout, 중복 또는 늦게 도착한 message를 어떻게 처리하는가?

Transport identity, owner node와 private route처럼 Framework가 알아서 관리하는 정보는 application
message에 넣지 않는다. 호출자가 모를 수 있는 값을 field로 추가해야 한다면 책임 경계가 잘못된
것인지 먼저 검토한다.

### 3.7 검증 흐름

문서의 설명을 실제 실행 결과로 확인할 수 있어야 한다. Client self-check는 response와 push payload를
직접 검증하고, runner는 필요한 process와 외부 resource를 준비한다.

검증 항목은 다음과 같이 나눈다.

| 층 | 확인하는 내용 |
|---|---|
| Client self-check | 사용자가 받는 response, push, 상태와 순서 |
| Server evidence | Handler가 만든 application 결과와 업무 상태 전이 |
| Smoke runner | Build, resource 준비, readiness, client 실행, cleanup과 process 종료 |

Log 한 줄은 진단 자료일 뿐 업무 성공을 대신하지 않는다. Public 결과나 application evidence로
판정할 수 없는 내부 동작은 sample의 성공 기준으로 만들지 않는다.

### 3.8 언어 parity

공통 문서가 고정하는 것과 언어가 선택할 수 있는 것을 분리한다.

| 공통으로 유지한다 | 언어별로 달라도 된다 |
|---|---|
| 역할과 책임, message 의미 | Project와 package 배치 |
| Message 이름과 wire field | Record, class와 struct 같은 타입 표현 |
| 업무 흐름과 상태 전이 | DI와 host 구성 문법 |
| 정상·실패 결과와 self-check 순서 | Framework가 허용한 관용적 비동기 표현 |
| Runner가 제공하는 실행 의미 | Shell, PowerShell, Gradle, npm과 CMake 사용법 |

한 언어에서 같은 의미를 구현할 수 없으면 sample 흐름을 그 언어에 맞게 약화하지 않는다. 누락된
public API와 사용자가 체감하는 차이를 feature map 또는 구현 차이 문서에 기록한다.

### 3.9 작성 후 contract review

초안을 완성한 뒤에는 [작성 후 contract review](#8-작성-후-contract-review)를 반드시 수행한다.
문서에 등장하는 Framework 주장과 API를 모두 추출하고, 공통 spec, 언어별 exact interface와 현재
public export를 각각 대조한다. 근거가 없는 기능을 발견하면 문장을 다듬어 남기지 않고 계약 후보나
문서 오류로 판정한다.

## 4. 통일된 문서 구조

새 공통 sample 문서는 다음 순서를 기본으로 사용한다. 독자가 목적에서 실행과 검증까지 같은 방향으로
읽게 하기 위한 순서다.

1. 목적과 범위
2. 요구사항
3. 시스템 구성과 topology
4. 역할과 책임
5. 사용하는 Framework 요소와 선택 이유
6. Message 계약
7. 업무 흐름
8. 구현 구조
9. Client self-check
10. Smoke 실행
11. 완료 기준

비교 배경, 상세 도메인 규칙, event sourcing, 복구, UI와 운영 기능은 필요한 sample에만 추가한다.
기존 방식과의 비교가 sample의 선택 조건과 책임 경계를 이해하는 데 필요하면 비교 배경을 유지한다.
통일된 기본 절에 포함되지 않았다는 이유로 기존 비교 절을 삭제하거나 축약하지 않는다. 조건부 절은
가장 관련 있는 기본 절 바로 뒤에 둔다. 예를 들어 비교 배경은 요구사항 뒤에, event sourcing은 상태
소유권을 설명한 뒤에, UI 규격은 client 구조 뒤에 둔다.

번호와 제목은 다음 template을 출발점으로 삼는다. 내용이 없는 절을 형식 때문에 만들지 않는다.

```markdown
# <SampleName> Sample Scenario

> <업무 상황, Framework가 맡는 책임과 사용자가 확인할 결과를 짧게 설명한다.>

## 1. 목적과 범위

## 2. 요구사항

### 2.1 기능 요구사항

### 2.2 운영·품질 요구사항

## 3. 시스템 구성과 topology

## 4. 역할과 책임

## 5. 사용하는 Framework 요소와 선택 이유

## 6. Message 계약

## 7. 업무 흐름

### 7.1 정상 흐름

### 7.2 실패·복구 흐름

## 8. 구현 구조

## 9. Client self-check

## 10. Smoke 실행

## 11. 완료 기준
```

기존 문서를 개정할 때는 번호만 바꾸는 대량 편집부터 하지 않는다. 중복된 절을 합치고 책임이 다른
내용을 옮긴 뒤, 마지막에 이 순서로 정렬한다. 문서 안 링크는 절 번호만 쓰지 말고 제목이 드러나는
anchor link로 고친다.

## 5. 절별 작성 방법

### 5.1 목적과 범위

첫 문단은 사용자가 겪는 상황, Framework가 제공해야 하는 결과와 sample의 주장을 설명한다. Server
이름, API 목록과 사용 기술부터 나열하지 않는다.

목적 절에는 다음 내용을 둔다.

- 해결하려는 업무 문제
- Framework가 대신 맡는 복잡성
- Application에 남는 도메인 책임
- 시작과 종료 범위
- 비슷한 sample과 다른 선택 기준

비슷한 sample을 비교할 때는 기능 개수 대신 상태 소유권, 전달 신뢰성, 연결 방식이나 복구 정책처럼
선택에 영향을 주는 축을 사용한다.

### 5.2 요구사항

요구사항은 뒤의 설계와 검증을 평가하는 기준이다. 기능 이름이 아니라 관찰 가능한 결과로 쓴다.

```text
이렇게 쓴다: 같은 ConversationId의 message는 하나의 owner가 순서대로 처리한다.
흔한 이탈:   conversation 기능을 지원한다.
```

기능 요구사항에는 사용자가 시작하는 operation과 업무 결과를 둔다. 운영·품질 요구사항에는 순서,
중복, 유실, 확장, 복구와 latency처럼 설계 선택을 바꾸는 조건만 둔다. 목표 수치에는 측정 대상,
시작·종료 시점과 허용 범위를 함께 적는다.

각 요구사항이 Framework 보장인지 sample의 domain 정책인지 구분한다. Framework 보장이라면 정식
spec 근거가 있어야 한다. Domain 정책은 sample이 선택한 규칙임을 밝히고 Framework의 일반 동작으로
확장하지 않는다.

### 5.3 비교 배경

비교는 해당 sample을 선택하는 이유를 이해하는 데 필요할 때만 둔다. 비교 대상을 일부러 불리하게
만들지 말고, 가장 단순하게 해결되는 조건과 추가 장치가 필요한 조건을 먼저 구분한다.

`framework/doc/framework/common/sample/event/`의 sample처럼 기존 방식의 구성 요소와 처리 흐름을
보여 주어야 Framework가 맡는 책임을 이해할 수 있으면 비교 절을 문서의 일부로 유지한다. 기존 문서를
가이드 구조에 맞춰 개정할 때도 이 비교가 답하는 질문과 비교 축을 보존한다. 계약 오류를 바로잡거나
중복을 줄이는 수정은 할 수 있지만, 절을 선택 사항으로 분류했다는 이유만으로 비교 내용이나 diagram을
제거하지 않는다.

비교 절은 다음 질문에 답해야 한다.

1. 기존 방식만으로 충분한 조건은 무엇인가?
2. 어떤 경계에서 추가 조정, 저장 또는 복구 장치가 필요한가?
3. Sample은 그중 무엇을 Framework에 맡기는가?
4. Sample을 사용해도 Application에 남는 책임은 무엇인가?

제품이 모든 복잡성을 없앤다고 쓰지 않는다. 형태가 바뀌거나 다른 module로 이동한 책임도 정확히
남긴다.

비교 diagram은 기본 시스템 topology와 목적이 다르다. 기존 구성에서 순서, 일관성, 전달과 복구를
담당하는 component를 보여 주는 데 필요하면 Load Balancer, log, database, cache와 scheduler 같은
외부 component와 message 흐름을 포함할 수 있다. Diagram 제목과 앞 문단에서 기존 방식의 비교
구성임을 밝히고, sample 자체의 기본 topology로 오해되지 않게 구분한다.

### 5.4 시스템 구성과 topology

기본 topology diagram은 Client와 server component의 배치와 구조적 연결 관계를 보여준다. STREAM,
Channel과 RouteMesh는 별도 component로 배치하지 않고 component 사이의 connection label로 표시한다.
Request, response, push와 publish의 시간 방향도 topology에 화살표로 그리지 않는다. 이 정보는 업무
흐름의 sequence diagram이 소유한다.

이 제한은 sample 자체의 기본 topology에 적용한다. 기존 방식이 어떤 외부 component로 같은 문제를
해결하는지 보여 주는 비교 diagram에는 [비교 배경](#53-비교-배경)의 기준을 적용한다.

Redis, database와 외부 API가 component 관계를 이해하는 데 꼭 필요하면 diagram 아래의 resource 표로
설명한다. 배포 resource 자체의 관계가 핵심인 sample만 별도 deployment diagram을 사용한다. 기본 시스템
구성도에 Framework connection과 외부 Store를 server component처럼 섞지 않는다.

Diagram 뒤에는 반드시 다음을 산문으로 설명한다.

- 외부 요청이 처음 도착하는 역할
- 업무 상태를 소유하는 역할
- 역할을 다른 process로 분리한 이유
- 자동 discovery와 수동 endpoint 중 선택한 방식
- 별도 stream, HTTP, fanout 또는 Store 연결

하나의 diagram에 모든 message field, 실패 분기와 내부 class를 넣지 않는다. 시간 순서는 업무 흐름의
sequence diagram으로 분리한다.

### 5.5 역할과 책임

역할 표는 이름과 개수만 나열하지 않는다.

| 역할 | 수 | 책임 | 분리 이유·소유 상태 |
|---|---:|---|---|
| `<Role>` | 1 | `<받는 요청과 제공하는 결과>` | `<왜 독립 역할이며 어떤 상태를 소유하는가>` |

역할 설명에는 public operation과 application 결과를 적는다. Internal class 이름과 socket 배선은
공통 sample의 업무 기준을 이해하는 데 꼭 필요할 때만 쓴다. 같은 상태의 소유자가 여러 표에서 다르게
보이지 않는지 확인한다.

### 5.6 사용하는 Framework 요소와 선택 이유

Framework 요소는 기능 목록이 아니라 선택 근거로 설명한다.

| 필요한 동작 | 선택한 요소 | 선택 이유와 경계 |
|---|---|---|
| 같은 ID의 상태를 한곳에서 순서대로 변경한다 | `<Framework element>` | `<Application에서 없어진 조정 책임과 남은 책임>` |

선택한 Framework 요소마다 public 동작을 소유하는 spec의 정확한 절을 연결한다. 선택 이유가 spec의
보장 범위를 넓히거나 특정 구현의 동작을 일반 계약으로 만들지 않는지 확인한다.

Public API를 설명해야 하면 산문에 함수 이름을 이어 쓰지 않는다. Spec과 exact interface에서 계약을
확인한 뒤, 실제 language sample과 public source에서도 확인한 짧은 code를 넣는다. 핵심 호출 옆
주석으로 목적과 제약을 설명한다. 공통 문서가 특정 언어 code를 정본으로 만들지 않도록 code block에는
언어와 출처를 밝힌다.

### 5.7 Message 계약

이 절의 message 계약은 지원 언어의 sample 구현이 함께 지킬 wire 이름, field와 업무 의미다.
Framework의 새 public contract를 정의하는 절이 아니다. Message 표는 호출자가 무엇을 기다리고
수신자가 무엇을 판단하는지 보여준다.

먼저 sample이 선택한 codec에 맞는 언어 중립 message declaration으로 wire 구조를 고정한다. 그다음
표에는 field를 반복하지 않고 방향, 호출 방식과 완료 의미를 적는다.

Protobuf sample은 실제 `.proto` 문법을 사용한다. Message 이름, field 이름과 type, tag,
`optional`, `repeated`, `oneof`와 `reserved`를 생략하지 않는다. 언어별 생성 option이나 package 이름은
공통 wire 구조에 영향을 줄 때만 포함한다.

JSON sample은 특정 언어의 class, record, interface 또는 type alias를 사용하지 않는다. 다음과 같은
언어 중립 선언으로 JSON wire 이름과 type을 고정한다.

```text
message JoinRoomReq {
  roomId: string
  playerId: string
  expectedVersion?: int64
}

message RoomState {
  status: "Waiting" | "Running" | "Finished"
  players: Player[]
  winnerId?: string | null
}
```

JSON 선언은 required와 optional, `null` 허용 여부, 배열 원소 type, 중첩 message와 enum의 wire 값을
구분해야 한다. 실제 JSON payload 예제는 serialization 결과를 설명할 때만 추가하며 declaration을
대신하지 않는다.

| Message | 방향·호출 방식 | 의미와 완료 결과 |
|---|---|---|
| `<NameReq/Res>` | `<Caller> -> <Target>, request` | `<요청 목적과 reply가 뜻하는 완료 경계>` |

Message 이름은 [공통 message 이름 원칙](../../../framework/doc/framework/common/sample/README.ko.md#메시지-이름-원칙)을
따른다. Request/reply, one-way send, client push와 publish를 구분하고, 호출 방식과 접미어가 맞아야
한다. Enum과 state 값은 별도 표에서 각 값이 되는 조건을 설명한다.

Declaration만으로 끝내지 않는다. Identity field, idempotency key, attempt와 version처럼 처리 결과를
가르는 값은 어느 역할이 만들고 검증하는지 업무 흐름에서 다시 연결한다.

### 5.8 업무 흐름

업무 흐름 하나는 다음 순서로 쓴다.

1. 시작 상태와 readiness를 밝힌다.
2. 동작 주체와 입력 값을 적는다.
3. Message가 지나가는 역할을 순서대로 설명한다.
4. 각 역할이 바꾸는 application state를 적는다.
5. Caller가 받는 response나 push를 적는다.
6. Client self-check가 확인할 값과 금지 결과를 연결한다.

정상 흐름을 먼저 작성하고, 정상 흐름과 비교해야 의미가 분명한 timeout, 중복, disconnect, restart와
복구 흐름을 뒤에 둔다. 실패 흐름에는 실패가 발생한 위치와 caller가 받는 terminal 결과를 함께 쓴다.

세 역할 이상을 거치거나 request, response, push와 publish가 이어지는 주요 정상 흐름은 Mermaid
sequence diagram으로 표현한다. 하나의 큰 diagram에 전체 scenario를 넣지 않고, 인증, 업무 시작,
상태 변경과 종료처럼 사용자가 구분할 수 있는 단계로 나눈다. Participant는 topology의 Client와 server
component 이름을 기준으로 표시한다. 같은 server 안의 logical object를 구분해야 흐름을 이해할 수 있으면
`Play / Player Actor`처럼 소유 component와 object 역할을 함께 쓴다. 역할 사이의 주요 message와 의미
있는 Framework operation만 표시하고, internal class, socket과 helper는 participant로 추가하지 않는다.

Sequence diagram은 사람이 역할 간 상호작용과 시간 순서를 빠르게 이해하도록 돕는다. Diagram만으로
계약을 정의하지는 않는다. 각 diagram 아래의 산문이나 번호 목록에서 시작 상태, application state 변경,
완료 결과, timeout과 실패 조건을 설명해야 한다. Diagram에서 message identifier로 표시한 이름은 message
declaration과 일치해야 하며, 같은 흐름의 Client self-check와도 대응해야 한다. 실패·복구 흐름은 발생
순서 자체가 판정에 중요할 때만 별도 sequence diagram으로 작성하고, 그렇지 않으면 정상 흐름과
대조하는 산문이나 표로 설명한다.

### 5.9 구현 구조

구현 구조는 역할과 책임이 code에서 어디에 놓이는지 찾게 한다. 언어별 문법을 고정하지 않고 logical
module을 기준으로 설명한다. 모든 지원 언어에서 같은 역할을 같은 순서로 찾을 수 있어야 한다. “같은
업무 결과를 만든다”는 조건만으로는 충분하지 않다.

```text
Sample
+-- Client
|   +-- Program
|   +-- Scenario
+-- Shared
|   +-- Configuration
|   +-- Contracts
+-- Server
    +-- Stateless Role
    |   +-- Program
    |   +-- Handlers
    +-- Stateful Role
        +-- Program
        +-- Domain
        +-- Application
        +-- Infrastructure
```

ASCII diagram 안에는 영문만 사용하고 모든 행의 가시 폭을 맞춘다. Directory tree 아래에는 각
module이 소유하는 판단과 의존성 방향을 설명한다. 단순한 file 목록이나 아직 없는 class 이름을
완성 기준처럼 적지 않는다.

파일명을 고정하지 않더라도 다음 logical location은 sample에 필요한 범위에서 명시한다.

- Client entry point와 self-check scenario
- Shared configuration과 wire contract
- 각 server 역할의 entry point
- Domain, Application과 Infrastructure 경계
- Handler, session, Actor, Spot과 외부 Store adapter의 소유 module
- State 또는 Actor relocation을 사용하는 경우 해당 adapter의 위치

언어별 구현은 역할을 생략하거나 다른 역할과 합쳐 탐색 구조를 바꾸지 않는다. 한 언어가 여러 type을
한 파일에 둘 수는 있지만 module, namespace, package 또는 type 이름에서 같은 logical component를 찾을
수 있어야 한다. 언어별 build tool 때문에 directory가 추가되더라도 공통 component 사이에 새로운
업무 layer를 만들지 않는다.

공통 sample 문서의 구현 구조는 다음 parity contract를 함께 고정한다.

1. 최상위 logical module은 `Client`, `Shared`, `Server` 순서를 유지한다.
2. `Client/Program`, `Client/Scenario`, `Shared/Configuration`, `Shared/Contracts`는 sample에 필요한
   범위에서 모든 언어가 같은 위치와 책임으로 제공한다.
3. `Server/<Role>/Program`과 각 역할의 `Domain`, `Application`, `Infrastructure` 경계는 공통 문서의
   역할 표와 같은 이름으로 대응한다. 한 언어가 여러 역할을 하나의 process에 배치할 수는 있지만
   module과 type에서 각 역할을 따로 찾을 수 있어야 한다.
4. 언어별 문서는 공통 sample의 operation 순서, message 선언, state 전이와 self-check 항목을 다시
   설계하지 않는다. 문법·package·host 구성만 해당 언어의 public API로 표현한다.

Handler 집합은 같게 유지하되 등록 방법은 언어 runtime의 차이를 보존한다. .NET의 attribute,
Java·Kotlin의 annotation과 Node.js의 decorator처럼 Framework가 선언형 metadata를 scan할 수 있는
언어는 handler 목록을 구성 코드에 반복하지 않고 자동 등록한다. C++은 runtime reflection scanner가
없으므로 compile-time type과 public builder로 같은 handler 집합을 명시 등록한다. C++에 자동 등록을
요구하거나, C++의 방식을 다른 언어에 수동 등록 관례로 확장하면 안 된다. 정확한 C++ 표면은
[C++ handler 공개 계약](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)을
따른다. 이 차이는 등록 방법에만 적용하며 message, handler 책임, 처리 순서와 self-check를 바꾸지 않는다.

따라서 각 sample의 `구현 구조` 절에는 위 tree와 함께 logical component별 책임 표를 둔다. 이 표가
없으면 구현자가 같은 업무 결과만 유지하고 handler, state owner와 adapter 위치를 임의로 바꿀 수
있으므로 언어 parity가 확인된 것으로 보지 않는다.

구현 구조를 `Client`, `Server`, `Domain`처럼 한 단계 이름만 나열해 끝내지 않는다. sample에서
실제로 필요한 역할마다 `Program`, `Handlers` 또는 `Session`, `Domain`, `Application`,
`Infrastructure` 중 해당 책임이 있는 위치를 tree에 표시하고, 표에서 입력·출력과 의존 방향을
설명한다. 외부 Store와 runtime resource는 server module로 오인하지 않도록 resource 표에서 별도로
소유자를 적는다.

상태를 소유하는 server의 상세 책임과 의존 방향은
[상태 소유 server 공통 구조](../../../framework/doc/framework/common/sample/README.ko.md#상태-소유-서버-공통-디렉토리-구조)를
따른다. Framework handler, session과 Spot adapter는 `Infrastructure`가 소유하며 `Domain`은 Framework
type과 transport에 의존하지 않는다.

언어별 package layout이 실제로 다르면 공통 문서의 logical structure와 실제 package의 대응 관계를
[언어별 표현 기준](../../../framework/doc/framework/common/sample/languages/README.ko.md)에 따라 기록한다.
별도 언어 문서는 공통 component를 대체하거나 줄이는 근거가 아니다.

### 5.10 Client self-check

Self-check는 성공 문구를 출력하는 절차가 아니라 사용자가 받는 결과를 자동으로 판정하는 scenario다.
각 항목에는 입력, 기대 값, 순서와 금지 결과를 적는다.

다음을 직접 확인한다.

- Request와 response의 correlation field와 업무 값
- Push 또는 publish payload와 도착 순서
- Timeout, 중복과 늦게 도착한 결과의 처리
- 재연결이나 복구 뒤 유지되어야 하는 상태
- Application message에 포함되면 안 되는 transport 내부 정보

Push 도착을 file log나 임의 sleep으로 판정하지 않는다. 해당 언어 Framework가 제공하는 public wait
interface와 bounded timeout을 사용한다. 어느 physical node가 owner가 되었는지처럼 계약이 정하지 않은
결과는 성공 조건에서 제외한다.

### 5.11 Smoke 실행

Smoke 절은 새로운 개발자가 runner를 실행했을 때 어떤 순서로 무엇이 준비되는지 설명한다.

1. Build한다.
2. 실행별 외부 resource와 설정을 준비한다.
3. 의존 순서에 따라 server를 시작한다.
4. Public readiness로 준비 완료를 확인한다.
5. Client self-check와 server evidence 검사를 실행한다.
6. 성공 marker와 artifact 위치를 출력한다.
7. 성공과 실패 모두에서 해당 실행이 만든 resource를 정리한다.

고정 sleep, 이미 실행 중인 공유 resource와 이전 실행의 cache에 의존하지 않는다. Redis, 설정 전달,
log와 runner의 정확한 공통 규칙은 [Framework 공통 sample](../../../framework/doc/framework/common/sample/README.ko.md)을
링크하고, sample별로 달라지는 역할 순서와 marker만 이 절에 둔다.

### 5.12 완료 기준

완료 기준은 “구현했다”가 아니라 reviewer가 확인할 수 있는 결과로 쓴다. 다음 범위를 구분한다.

- 모든 지원 언어가 같은 업무 흐름과 message 계약을 구현한다.
- 각 언어의 build와 sample runner가 통과한다.
- Client self-check가 response, push와 상태 순서를 직접 검증한다.
- Public API만 사용하며 private helper나 raw frame 우회가 없다.
- 공통 topology, 설정, codec와 runner 정책을 지킨다.
- 관련 guide, feature map과 언어별 표현 문서가 필요한 범위에서 갱신됐다.

미구현 언어를 제외하고 완료로 표시하지 않는다. 구현할 수 없는 계약 차이는 gap과 후속 조건을
문서화한 뒤 완료 범위에서 분리한다.

## 6. 읽기 쉬운 형식을 유지하는 규칙

### 6.1 제목에 드러나는 흐름

제목은 `기타`, `상세`, `추가 고려사항`처럼 범위를 알 수 없는 말을 피한다. `Timeout 뒤 재배정`,
`재연결 뒤 session binding 교체`처럼 조건과 결과가 드러나는 명사구를 사용한다.

같은 수준의 제목은 같은 종류의 정보를 담는다. `server 구성`, `Message 계약`, `업무 흐름` 사이에 특정
method 하나를 같은 수준의 절로 끼우지 않는다.

### 6.2 문단별 판단

한 문단에서 topology, state ownership, timeout과 언어 차이를 모두 설명하지 않는다. 첫 문장에 그
문단의 판단을 두고, 뒤 문장은 이유와 결과를 뒷받침한다.

독자가 되돌아가야 이해되는 조건은 해당 동작이 처음 나오는 자리로 옮긴다. 문서 끝의 완료 기준에서
새 설계 규칙이 처음 등장하면 본문 구조가 잘못된 것이다.

### 6.3 업무 용어와 Framework 용어의 구분

`Order`, `Conversation`, `Zone` 같은 업무 개념은 sample의 예시다. `Actor`, `Spot`, `Channel`과
`Location Store`는 Framework 요소다. 업무 규칙을 Framework의 일반 보장처럼 쓰거나, Framework
계약을 업무 비유만으로 설명하지 않는다.

처음 나오는 제품 개념은 하는 일을 먼저 설명한 뒤 이름을 붙인다. Server 개발자가 일상적으로 쓰는
request, handler, timeout, retry, payload와 routing 같은 용어는 영어로 유지한다.

### 6.4 표와 diagram의 역할

표는 역할 비교, message field, state 값과 언어 parity처럼 같은 축을 반복해서 비교할 때 사용한다.
시간 순서는 번호 목록이나 Mermaid sequence diagram으로 설명한다. 짧은 사실을 한 행 표로 만들지
않는다.

Diagram은 한 가지 질문에만 답한다. 시스템 topology diagram은 Client와 server component의 배치와
구조적 연결을 보여 준다. Connection 종류는 선의 label로 표시하며 Channel, RouteMesh와 Store를 server
component처럼 배치하지 않는다. Sequence diagram은 request, response, push와 publish의 시간 순서를
보여 준다. ASCII diagram은 영문 전용, 고정 폭과 80자 이하 규칙을 따른다. Diagram 아래에는 독자가
그림에서 판단해야 하는 내용을 산문으로 설명한다. Topology와 sequence diagram에서 component 이름과
소유 관계를 일치시키고, sequence diagram에서 message identifier로 표시한 이름은 message 계약의
declaration과 일치시킨다.

### 6.5 소유 문서 연결

공통 topology, message 이름, 설정과 runner 정책을 sample마다 전체 복사하지 않는다. Sample별로 어떤
선택을 했는지와 왜 달라졌는지만 쓰고 공통 정책의 정확한 anchor를 링크한다.

반대로 현재 업무 흐름을 이해하는 데 필요한 역할과 message 의미를 링크로 대신하지 않는다. 독자가
이 sample 문서 하나만 읽고 흐름을 이해할 수 있을 만큼은 다시 설명한다.

## 7. 흔한 이탈과 수정 방법

| 이탈 | 문제 | 수정 방법 |
|---|---|---|
| 첫 절이 기능과 API 목록이다 | 어떤 문제에 쓸 sample인지 판단할 수 없다. | 업무 상황, Framework가 맡는 책임과 결과를 먼저 쓴다. |
| 모든 기존 문서의 절을 합쳐 template으로 만든다 | Sample마다 빈 절과 중복이 늘어난다. | 공통 기본 절만 두고 조건부 절은 필요한 위치에 추가한다. |
| 역할 표에 server 이름만 있다 | 상태 소유자와 분리 이유를 알 수 없다. | 책임, 소유 상태와 process 분리 이유를 함께 쓴다. |
| Message declaration만 있다 | Message가 언제 쓰이고 무엇을 완료하는지 알 수 없다. | 호출 방식과 완료 의미 표를 추가하고 상태 전이를 업무 흐름에 연결한다. |
| Internal helper로 기능을 메운다 | Sample이 계약 밖의 사용법을 정본으로 만든다. | 근거가 없으면 contract gap으로 분리한다. |
| Domain code가 raw payload를 해석한다 | Framework의 codec 책임이 호출자에게 노출된다. | Framework의 기본 typed codec 경로가 끊긴 지점을 수정한다. |
| 물리 node ID를 업무 message에 넣는다 | Routing이 application 계약으로 노출된다. | 업무 ID로 호출하고 Framework가 owner를 찾게 한다. |
| Diagram과 표가 같은 내용을 반복한다 | 변경할 곳이 늘고 서로 어긋난다. | Diagram은 관계, 표는 정확한 비교만 맡긴다. |
| Self-check가 성공 log만 확인한다 | Payload와 상태가 틀려도 통과한다. | Response, push, 순서와 금지 결과를 직접 assert한다. |
| Runner가 sleep 뒤 client를 실행한다 | 느린 환경에서 불규칙하게 실패한다. | Public readiness를 bounded wait로 확인한다. |
| 한 언어 구현을 공통 계약으로 복사한다 | 다른 언어의 public surface와 충돌할 수 있다. | Spec과 공통 sample을 기준으로 삼고 언어 구현은 증거로만 사용한다. |
| 완료 기준에서 새 규칙이 처음 나온다 | 구현자가 본문만 읽고 필수 조건을 놓친다. | 해당 설계·흐름 절로 옮기고 완료 기준에는 검증 문장만 남긴다. |

## 8. 작성 후 contract review

초안을 마치면 문서에 등장하는 Framework 주장과 API를 처음부터 다시 추출해 정식 계약에
대조한다. Spec 링크가 있다는 사실만 확인하거나 현재 sample code가 compile된다는 사실만으로
통과시키지 않는다. API의 공개 여부와 동작의 계약 근거를 각각 확인한 뒤, sample 고유의 업무
규칙과 Framework 보장을 분리해야 한다.

Review는 contract 근거, 현재 구현, sample 내부 일관성과 표현 순서로 진행한다. 앞 단계가 실패하면
문장을 다듬기 전에 contract 문제부터 해결한다.

### 8.1 Framework 주장과 API 목록 추출

Sample 문서를 처음부터 끝까지 읽으며 다음 항목을 review 목록에 옮긴다.

- API, type, option, callback, error와 state 이름
- 호출 가능한 lifecycle 시점과 사전 조건
- 성공 결과와 operation이 완료되는 시점
- Timeout, cancellation, backpressure와 retry 의미
- Handler 직렬화, message 순서와 중복 처리 보장
- Discovery, target 선택, routing과 relocation 동작
- Ownership, disposal, session binding과 state 보존 규칙
- Self-check와 완료 기준이 기대하는 public 결과

본문만 보지 않는다. 표, diagram label, code block, 주석, message 흐름, self-check와 완료 기준도 같은
대상이다. Diagram이나 code 주석에만 있는 보장도 독자가 계약으로 받아들일 수 있기 때문이다.

추출한 항목은 다음 기준으로 분류한다.

| 분류 | 의미 | 처리 |
|---|---|---|
| Framework public contract | Application이 모든 지원 언어에서 의존할 수 있는 기능과 결과 | 공통 spec과 exact interface 근거를 연결한다. |
| Sample 업무 규칙 | 이 sample이 선택한 domain 상태, message와 처리 정책 | Framework 보장처럼 쓰지 않고 sample 소유임을 밝힌다. |
| 구현 상세 | Runtime 내부 배선, private protocol, Store record와 임시 helper | 공통 sample에서 제거하거나 internals로 옮긴다. |
| 계약 후보 | Sample에 필요하지만 정식 spec에 없는 공개 기능 | Sample에서 보장하지 않고 별도 spec review와 gap으로 분리한다. |

분류할 수 없는 문장은 그대로 두지 않는다. 주체와 소유 문서를 찾을 수 있을 때까지 문장을
나누거나 근거를 다시 조사한다.

### 8.2 정식 spec과 기능 대조

Framework public contract로 분류한 항목은 공통 spec의 구체적인 절과 한 항목씩 대조한다. 공통
guide에서는 기능의 의도와 표준 사용 pattern이 spec과 일치하는지 확인한다. API 이름만 찾지 말고
sample이 주장하는 전체 동작을 확인한다. Guide와 spec이 어긋나면 spec을 기준으로 판정하고 guide를
별도 수정 대상으로 기록한다.

| 확인 영역 | 대조할 계약 |
|---|---|
| 입력과 호출 시점 | 유효한 상태, 필수 값, 기본값과 사전 조건 |
| 정상 결과 | 반환값, callback, side effect와 완료 시점 |
| 실패 결과 | Public error kind, timeout, cancellation과 cleanup |
| 실행 순서 | 직렬화 범위, callback 순서와 동시 실행 허용 범위 |
| Routing | Identity, target 선택, discovery와 stale route 처리 |
| 수명과 ownership | 생성, binding, relocation, shutdown, disposal과 보존 범위 |
| 관측 결과 | Public status, monitoring event와 application이 확인할 수 있는 값 |

Spec이 여러 terminal 결과를 허용하면 sample을 단순하게 만들기 위해 하나만 Framework 보장으로
좁히지 않는다. 반대로 spec이 반드시 보장하는 결과를 `A 또는 B`처럼 느슨하게 만들지 않는다.
Sample이 선택한 retry 횟수, codec, timeout 값이나 Store는 sample 설정으로 쓸 수 있지만 Framework의
일반 기본값이나 보장으로 표현하지 않는다.

정식 spec에 근거가 없는 기능은 다음 중 하나로 판정한다.

- 순수한 domain 규칙이면 Sample 업무 규칙으로 다시 쓴다.
- Runtime 내부 동작이면 sample에서 제거하고 필요한 경우 internals를 링크한다.
- Application이 호출할 새 public API나 새 보장이 필요하면 계약 후보로 분리한다.

다른 언어 구현, 공통 E2E 또는 기존 sample에 같은 기능이 있어도 이 판정은 바뀌지 않는다. 이 자료는
계약 해석과 누락을 찾는 증거지만 public contract의 출처가 아니다.

### 8.3 Exact interface와 public API 대조

공통 spec에서 기능 근거를 확인한 뒤, sample이 지원하는 각 언어의 exact interface에서 정확한 public
표현을 확인한다. 다음 항목을 signature 단위로 대조한다.

- Package와 public type 이름
- Method, constructor, factory와 builder entrypoint
- Parameter 순서, type, generic, nullable과 optional 표현
- Return type, 비동기 표현과 cancellation 입력
- Public option, enum, result와 error
- Callback parameter, lifetime과 disposal 방법

문서의 code가 `internal`, `private`, `runtime`, `dist/runtime` 경로를 import하거나 reflection, raw frame과
test-only adapter를 사용하면 review 실패다. Public wrapper가 있더라도 private API를 호출하기 위한
pass-through helper라면 같은 실패로 본다.

어떤 언어 구현에 public API가 있어도 exact interface와 공통 spec에 근거가 없으면 sample 계약으로
사용하지 않는다. 반대로 exact interface에 있지만 현재 package에 없는 API는 정식 목표 계약으로
유지하되 해당 언어의 implementation gap을 기록한다.

### 8.4 현재 배포 package와 구현 대조

Spec과 exact interface 검토는 목표 계약을 확인하는 단계다. 그다음 Application이 compile하는 public
declaration을 소유하는 production contract source와 현재 배포 package를 확인해 sample code가 실제로
compile되고 실행될 수 있는지 별도로 판정한다.

1. Application이 사용하는 package entrypoint에서 API를 public하게 import할 수 있는지 확인한다.
2. Source declaration의 visibility와 실제 배포 artifact의 export가 같은지 확인한다.
3. Sample code가 정식 signature, 기본값과 lifecycle 순서대로 호출하는지 확인한다.
4. Build 또는 compile test로 private dependency 없이 사용할 수 있는지 확인한다.
5. Contract test와 sample self-check가 spec의 완료·실패 결과를 실제로 판정하는지 확인한다.

현재 구현이 목표 계약과 다르다는 이유로 sample 문서를 현재 최소 기능에 맞춰 축소하지 않는다.
차이는 언어별 implementation gap과 후속 조건으로 기록한다. 다만 실제로 실행되지 않는 상태에서
해당 언어 sample이나 전체 sample을 완료했다고 표시해서는 안 된다.

### 8.5 Contract review 기록과 판정

Review 중에는 다음 형식으로 작업 기록이나 PR 설명에 근거를 남긴다. 공통 sample 본문에는 검토 이력을
넣지 않는다. 장기 추적이 필요한 차이는 feature map이나 implementation gap 문서가 소유한다.

```markdown
#### <Sample 절 또는 API>

- 분류: <public / domain / internal / candidate>
- 공통 spec: <file#anchor>
- Exact interface: <file#anchor 또는 gap>
- Public source: <file:line 또는 gap>
- 검증: <contract test 또는 self-check>
- 판정: <적합 / 구현 gap / 계약 후보 / 문서 오류>
```

판정은 다음 의미로 사용한다.

| 판정 | 의미 | 후속 작업 |
|---|---|---|
| 적합 | Spec 근거, public interface와 sample 표현이 일치한다. | 다음 review 단계로 진행한다. |
| 구현 gap | 정식 계약은 있지만 현재 언어 package나 sample 구현이 없다. | Gap과 완료 조건을 기록하고 구현 전 완료 표시를 막는다. |
| 계약 후보 | Sample이 요구하지만 정식 public contract가 없다. | Sample 보장에서 제거하고 spec·guide 설계 review로 분리한다. |
| 문서 오류 | Spec과 반대이거나 private 동작을 public 보장처럼 썼다. | 문서를 수정한 뒤 처음부터 다시 대조한다. |

`계약 후보`와 `문서 오류`가 하나라도 남아 있으면 sample 문서를 승인하지 않는다. `구현 gap`은
target-first 문서에 남을 수 있지만, 지원 언어의 구현 완료나 전체 sample 완료를 뜻하지 않는다.

### 8.6 Sample 내부 일관성 검증

Contract review를 통과한 뒤 sample 문서 안에서 같은 정보가 서로 맞는지 확인한다.

1. 첫 문단만 읽고 업무 문제, Framework의 역할과 정상 결과를 말할 수 있는지 확인한다.
2. 요구사항마다 이를 실행하는 업무 흐름과 판정하는 self-check가 있는지 확인한다.
3. 역할 표, topology와 업무 흐름에서 상태 소유자와 message 방향이 같은지 대조한다.
4. Message 표의 이름·field·호출 방식이 흐름과 언어별 sample code에서 같은지 확인한다.
5. 정상, timeout, 중복, disconnect와 복구 흐름의 terminal 결과가 서로 충돌하지 않는지 확인한다.
6. Self-check가 내부 상태나 특정 log line이 아니라 public 결과와 application evidence를 판정하는지
   확인한다.
7. 완료 기준이 본문에 없는 새 요구사항을 추가하지 않는지 확인한다.
8. 모든 지원 언어가 같은 의미로 구현할 수 있는지 확인한다.

### 8.7 표현과 형식 검증

내용 검토를 통과한 뒤 표현을 검토한다.

1. 제목만 읽어도 목적에서 검증까지 흐름이 이어지는지 확인한다.
2. 한 문단에 하나의 판단만 있는지 확인한다.
3. 처음 나오는 개념은 하는 일을 먼저 설명했는지 확인한다.
4. 영어 명사를 이어 붙인 압축 표현, 구어체와 의인화를 제거한다.
5. 표, code fence, 상대 링크와 diagram 문법을 확인한다.
6. [기술문서 작성 원칙](documentation-principles.ko.md)에 따라 전체 문장을 다시 검토한다.
7. `git diff --check`와 저장소의 문서 검사를 실행한다.

### 8.8 독립 review

큰 개정이나 새 sample 문서는 작성자 검토 뒤 독립 reviewer가 다시 확인한다. Review는 다음 축을
분리한다.

| 축 | 단일 기준 | 확인 내용 |
|---|---|---|
| Contract 부합 | 공통 spec, exact interface와 public source | API 공개 여부, 동작·완료·오류 의미와 sample 검증의 일치 |
| 문서 원칙 준수 | [기술문서 작성 원칙](documentation-principles.ko.md) | 독자와 범위, 현재 상태, 용어, 문체, 표와 diagram 사용 |

Finding은 `[축][심각도] 위치 — 문제 — 근거 — 제안` 형식으로 남긴다. Contract finding에는 spec이나
exact interface의 file과 anchor를, 현재 구현 finding에는 production source나 배포 artifact의 정확한
위치를 근거로 단다.

작성자는 finding의 근거를 직접 확인한 뒤 검증된 내용만 반영한다. 목표 contract와 현재 구현을
혼동한 finding은 implementation gap으로 다시 분류한다. 수정 뒤에는 영향을 받은 항목의 contract
review와 문서 원칙 검토를 다시 수행한다.

## 9. 완료 점검표

### 목적과 책임

- [ ] 첫 문단이 업무 문제, Framework가 맡는 책임과 사용자가 확인할 결과를 설명한다.
- [ ] 시작, 종료와 제외 범위가 분명하다.
- [ ] 공통 sample이 새 public contract를 만들지 않는다.
- [ ] Framework 책임과 Application 책임을 구분했다.
- [ ] 비슷한 sample과의 차이를 선택 기준으로 설명했다.
- [ ] 기존 방식과의 비교가 선택 조건과 책임 경계를 설명한다면 비교 절과 diagram을 유지했다.

### 구조와 흐름

- [ ] 기본 절을 필요한 범위에서 같은 순서로 배치했다.
- [ ] 조건부 절을 관련 기본 절 가까이에 두고 빈 절을 만들지 않았다.
- [ ] 역할마다 책임, 상태 소유권과 분리 이유가 있다.
- [ ] 시스템 topology가 Client와 server component 및 구조적 연결만 보여 준다.
- [ ] 기존 방식의 비교 diagram과 sample 자체의 기본 topology를 구분했다.
- [ ] Message 방향은 topology가 아니라 업무 흐름이나 sequence diagram에서 설명한다.
- [ ] 세 역할 이상을 거치는 주요 정상 흐름을 단계별 sequence diagram으로 표현했다.
- [ ] Sequence diagram의 component 소유 관계와 표시한 message identifier가 topology 및 message 계약과 일치한다.
- [ ] Diagram 아래의 산문이 시작 상태, state 변경, 완료 결과와 실패 조건을 설명한다.
- [ ] 모든 지원 언어에서 같은 logical component를 같은 순서로 찾을 수 있다.
- [ ] Client, Shared contract, server entry point와 Domain·Application·Infrastructure 대응이 명시돼 있다.
- [ ] 정상 흐름을 먼저 쓰고 실패·복구 흐름을 대조할 수 있게 배치했다.
- [ ] 각 흐름에 시작 상태, 주체, message, state 변경과 client 결과가 있다.

### 계약과 언어 parity

- [ ] 본문, 표, diagram, code, self-check와 완료 기준의 Framework 주장과 API를 모두 추출했다.
- [ ] 추출한 항목을 Framework public contract, Sample 업무 규칙, 구현 상세와 계약 후보로 분류했다.
- [ ] 모든 Framework 기능과 관찰 결과를 공통 spec의 구체적인 절에 대응시켰다.
- [ ] 각 지원 언어의 API를 exact interface의 signature와 대응시켰다.
- [ ] 현재 package의 public export 확인을 목표 계약 확인과 별도로 수행했다.
- [ ] Spec이 허용한 결과를 좁히거나 반드시 보장한 결과를 느슨하게 만들지 않았다.
- [ ] Sample이 선택한 codec, timeout, retry와 Store를 Framework의 일반 보장처럼 쓰지 않았다.
- [ ] Codec에 맞는 언어 중립 declaration으로 wire 이름, type과 cardinality를 고정했다.
- [ ] Protobuf의 tag·reserved 또는 JSON의 optional·null·배열 의미를 생략하지 않았다.
- [ ] Message 이름, 호출 방식, field와 완료 의미가 일치한다.
- [ ] Transport identity와 private route를 application message에 노출하지 않는다.
- [ ] 언어별 sample은 public API만 사용한다.
- [ ] Internal import, reflection, raw frame, test-only adapter와 private pass-through helper가 없다.
- [ ] 공통으로 고정할 의미와 언어별 표현 차이를 구분했다.
- [ ] Handler 집합은 같고, scan을 지원하는 언어의 자동 등록과 C++의 compile-time 명시 등록 차이를 유지했다.
- [ ] 미구현 기능을 조용히 제외하지 않고 gap과 후속 조건을 기록했다.
- [ ] Review 기록에 각 항목의 spec 근거, exact interface, 검증과 판정을 남겼다.
- [ ] `계약 후보`와 `문서 오류` 판정이 남아 있지 않다.

### 실행과 검증

- [ ] Self-check가 response, push, 상태와 순서를 직접 assert한다.
- [ ] 성공 조건이 internal state, physical owner나 특정 log line에 의존하지 않는다.
- [ ] Runner가 build, resource 준비, readiness, client 실행, evidence와 cleanup을 담당한다.
- [ ] 고정 sleep과 다른 실행의 resource에 의존하지 않는다.
- [ ] 완료 기준이 지원 언어, build, runner와 문서 갱신 범위를 판정할 수 있게 썼다.

### 최종 확인

- [ ] 제목과 문단을 [기술문서 작성 원칙](documentation-principles.ko.md)에 따라 다시 검토했다.
- [ ] 표와 diagram은 비교나 관계를 더 쉽게 이해하게 하는 경우에만 사용했다.
- [ ] ASCII diagram은 영문, 고정 폭과 폭 제한을 지킨다.
- [ ] 상대 링크와 anchor가 실제 문서를 가리킨다.
- [ ] `git diff --check`와 저장소의 문서 검사가 통과한다.
- [ ] Contract review 뒤 sample 내부 일관성과 표현을 순서대로 다시 검토했다.
- [ ] 큰 개정이나 새 문서는 contract 부합과 문서 원칙 준수 축으로 독립 review를 마쳤다.
- [ ] 처음 읽는 개발자가 이 문서만으로 sample의 목적, 구조와 검증 방법을 설명할 수 있다.
