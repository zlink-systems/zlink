# E2E 시나리오 문서 작성 가이드

> 이 문서는 Framework 공통 E2E 시나리오 문서를 작성하거나 수정하는 작업자가 따르는
> 절차다. 주 독자는 문서 작업을 수행하는 AI다.
>
> 가장 중요한 목표는 처음 읽는 개발자가 **어떤 상황을 만들고, 무엇이 어떻게 동작하는지**
> 쉽게 이해하게 하는 것이다. 그 이해를 바탕으로 실행 조건과 통과 기준을 찾을 수 있어야
> 한다. 정해진 문서 모양을 반복하거나 test 항목을 빠짐없이 나열하는 것이 목표가 아니다.

[기술문서 작성 원칙](documentation-principles.ko.md) ·
[스펙 문서 작성 가이드](spec-writing-guide.ko.md) ·
[Framework 공통 E2E](../../../framework/doc/framework/common/e2e/README.ko.md)

## 1. 최우선 원칙 — 테스트하는 동작을 먼저 이해시킨다

E2E 문서는 test code를 옮겨 적은 목록이 아니다. 실제 사용자가 겪을 수 있는 상황에서 여러
process와 service가 어떻게 함께 동작하는지 설명하고, 그 동작이 계약대로 이루어지는지 확인하는
문서다.

따라서 config와 scenario는 기술적인 실행 방법보다 다음 이야기를 먼저 전달한다.

1. Application이 어떤 상황에 놓이는가.
2. 정상이라면 Framework가 무엇을 해 주어야 하는가.
3. 이 동작이 실패하면 Application에서 어떤 문제가 보이는가.
4. E2E에서는 그 정상 동작이나 실패를 무엇으로 확인하는가.

예를 들어 `provider restart`, `ready convergence`, `stale connection`을 바로 나열하지 않는다. 먼저
다음처럼 사용자가 겪는 상황을 설명한다.

> 요청을 처리하던 provider가 재시작되면 consumer에 남아 있던 이전 connection은 더 이상 사용할 수
> 없다. Framework는 재시작한 provider와 새 connection을 설정하고, provider가 요청을 처리할 수 있는
> 상태가 된 뒤 신규 request를 그 connection으로 보내야 한다. 이 scenario는 재시작 직후의 첫
> request가 별도 retry 없이 처리되는지 확인한다.

이 설명을 읽은 뒤에야 독자는 `ready`, connection 교체와 첫 request가 왜 함께 등장하는지 이해할 수
있다. 쉬운 설명을 생략한 채 용어, API와 assertion만 정확하게 나열하면 완료된 문서로 보지 않는다.

완성된 문서는 구현 개발자와 검토자가 관련 코드를 먼저 읽지 않아도 다음 질문에 답할 수 있어야 한다.

| 질문 | 문서가 제공해야 하는 답 |
|---|---|
| 어떤 상황인가? | Application과 server가 처한 시작 상태를 일상적인 개발 언어로 설명한다. |
| 정상이라면 어떻게 동작하는가? | 요청부터 최종 결과까지 Framework가 보장해야 하는 흐름을 설명한다. |
| 왜 확인해야 하는가? | 이 동작이 깨졌을 때 사용자가 겪는 실패를 설명한다. |
| 무엇을 검증하는가? | 앞에서 설명한 동작을 판정할 하나의 검증 질문을 제시한다. |
| 왜 E2E가 필요한가? | 단위 test만으로 확인할 수 없는 process·node·store 경계를 밝힌다. |
| 어떤 배포가 필요한가? | 역할, process 수, 연결 관계와 필수 외부 resource를 설명한다. |
| 무엇을 먼저 준비하는가? | 시작 상태, fixture, readiness와 scenario별 입력을 설명한다. |
| 어떤 순서로 실행하는가? | 동작 주체, 호출, 장애 주입과 상태 변경 순서를 설명한다. |
| 무엇을 보고 판정하는가? | Client 결과, public status와 역할 server가 public endpoint로 제공하는 application 결과를 구분한다. |
| 언제 성공하거나 실패하는가? | 기대 값, 허용 범위, terminal 결과와 금지되는 결과를 명시한다. |
| 다른 언어도 같은 것을 검증하는가? | 언어별 API 모양과 무관하게 유지해야 하는 의미와 marker를 고정한다. |

절차와 assertion이 정확해도 앞의 세 질문에 답할 수 없다면 이해하기 쉬운 문서가 아니다. 반대로
쉬운 설명만 있고 실행 조건과 관측 값이 없으면 검증 문서로 사용할 수 없다. 독자의 이해를 먼저
확보한 뒤, 그 이해를 실제로 검증할 수 있을 만큼 정확하게 작성한다.

각 config 문서는 독립적으로 읽을 수 있어야 한다. 관련 spec과 다른 config의 링크는 세부 계약을
확인하는 경로다. 현재 scenario를 이해하는 데 꼭 필요한 상황과 동작 설명을 링크로 대신하지 않는다.

## 2. E2E 문서가 소유하는 내용

E2E 문서는 정식 spec의 공개 계약이 실제 배포 조건에서도 유지되는지 검증하는 기준이다.
문서가 소유하는 내용은 다음과 같다.

- 계약을 실행할 역할과 topology
- 재현할 정상·실패·경합 조건
- 언어 구현이 public contract를 통해 공통으로 제공할 evidence
- 통과와 실패를 나누는 관찰 가능한 조건
- scenario ID, priority와 언어별 구현 범위

E2E 문서는 새 public API나 새 공개 동작을 정하는 문서가 아니다. 계약의 출처는 정식 spec과
언어별 interface다. E2E를 설계하다 계약에 없는 public API가 필요하다는 사실을 발견하면
scenario에 기대 API를 만들어 넣지 않는다. 먼저 정식 계약으로 받아들일지 별도 설계 대상으로
분리하고, 확정 전에는 feature map에 public contract gap으로 기록한다.

다른 문서와의 책임은 다음처럼 구분한다.

| 문서 | 답하는 질문 |
|---|---|
| 정식 spec | Application이 의존할 수 있는 공개 동작은 무엇인가? |
| 언어별 interface | 그 계약을 해당 언어에서 어떤 signature로 호출하는가? |
| E2E 시나리오 문서 | 어떤 실제 배포와 evidence로 그 계약을 검증하는가? |
| E2E test code와 runner | 문서의 절차와 판정을 어떻게 자동 실행하는가? |
| feature map | 어느 언어가 어떤 scenario를 구현했고 어떤 gap이 남았는가? |

### 2.1 Framework public contract 검증

E2E가 Framework 동작을 시작하고 결과를 판정할 때는 정식 spec과 언어별 interface에 정의된
public API만 사용한다. E2E라는 이유로 internal helper, private protocol, Store 내부 record나 runtime
자료구조를 직접 읽지 않는다.

Client는 역할 server의 public application endpoint를 호출한다. 역할 server는 그 요청을 받은 뒤 public
Framework API로 operation을 시작하고, public reply·error·status 또는 application handler 결과를 client가
확인할 수 있는 응답으로 제공한다. Handler 호출 횟수나 받은 payload가 필요하면 역할 server가
application state로 기록하고 public evidence endpoint로 제공할 수 있다. 이 evidence는 Framework 내부
queue, connection, descriptor나 private counter를 노출해서는 안 된다.

Runner는 검증 조건을 만들기 위해 process를 시작·종료하거나 crash시키고, 공개 설정으로 port, timeout,
HWM과 Store를 구성할 수 있다. 하지만 다음 값은 일반 E2E의 통과 조건으로 사용하지 않는다.

- 내부 connect attempt, reconnect와 liveness probe 횟수
- Physical connection ID, socket write와 private handshake 단계
- Framework 내부 queue 길이, reservation, descriptor record와 CAS 횟수
- Private frame, internal callback과 test-only hook 결과
- 특정 internal log line의 존재 또는 부재

이 값이 구현 정확성에 필요하면 contract test나 internal protocol test에서 검증한다. Public monitoring,
tracing이나 observer 자체가 정식 public contract이고 그 기능을 검증하는 scenario라면 해당 public API가
반환한 값은 통과 조건으로 사용할 수 있다. 일반 scenario의 file log는 실패 원인을 찾는 진단 자료일
뿐, public 결과를 대신하지 않는다.

## 3. 작업 순서

### 3.1 계약 근거 확인

시나리오를 쓰기 전에 다음 순서로 근거를 확인한다.

1. 검증할 공통 Framework spec과 package spec을 확인한다.
2. 언어별 interface에서 호출 가능한 public API를 확인한다.
3. [공통 E2E README](../../../framework/doc/framework/common/e2e/README.ko.md)에서 config의
   범위, 공통 실행 원칙, scenario ID와 priority 규칙을 확인한다.
4. 기존 언어 구현과 E2E test는 누락과 실제 제약을 찾는 증거로 사용한다.
5. 자료가 충돌하면 정식 계약의 소유 문서를 기준으로 판정한다.

구현에만 존재하는 동작이나 다른 언어에만 있는 편의 API를 공통 scenario의 전제로 삼지
않는다. 반대로 정식 spec에 있는 공통 기능이 특정 언어에 없으면 그 언어의 E2E에서 조용히
제외하지 않고 feature map에 gap과 후속 조건을 기록한다.

### 3.2 검증 동작의 쉬운 설명

계약을 확인했으면 API, state와 error 이름을 잠시 내려놓고 동작을 쉬운 문장으로 정리한다. 다음
네 문장을 채우면 scenario의 기본 설명이 된다.

```text
이 scenario의 시작 상태는 ______이다.
Application이 ______하면 Framework는 ______해야 한다.
이 동작이 깨지면 Application에는 ______로 보인다.
따라서 E2E는 ______를 확인한다.
```

빈칸에 내부 class나 helper 이름이 들어가면 한 단계 더 풀어서 쓴다. 독자는 구현 component보다
먼저 요청, 연결, 상태와 결과의 관계를 알아야 한다. 제품 용어가 꼭 필요하면 하는 일을 설명한
문장 뒤에 그 이름을 소개한다.

쉬운 설명은 정확한 계약을 줄이는 요약이 아니다. 먼저 전체 흐름을 이해시키고, 뒤의 절차와 검증
항목에서 identity, 횟수, error와 완료 시점을 정확하게 고정한다.

### 3.3 단일 검증 질문

각 scenario는 하나의 주된 질문에 답한다. 질문은 실행 결과를 보고 `예` 또는 `아니요`로
판정할 수 있게 쓴다.

이렇게 쓴다.

> Provider가 ready 상태가 된 직후 첫 request를 보내면 추가 sleep이나 application retry 없이
> reply를 받는가.

이렇게 쓰지 않는다.

> Provider discovery와 연결 상태, retry, timeout, failover가 모두 올바른가.

두 번째 질문은 여러 원인과 완료 조건을 한 scenario에 묶는다. 어느 동작이 실패했는지 판정하기
어렵고 언어별 구현 범위도 달라진다. 입력, 장애 조건, 기대 terminal 결과가 다르면 scenario를
나눈다. 다만 하나의 업무 흐름을 끝까지 확인해야 의미가 있는 여러 hop은 한 scenario로 유지한다.

### 3.4 E2E에서 드러나는 경계

검증 질문을 정한 뒤 왜 이 검증이 E2E여야 하는지 확인한다. 다음 중 하나 이상이 있어야 한다.

- 둘 이상의 process나 node 사이에서 상태가 전파된다.
- 실제 transport 연결, discovery 또는 routing 선택이 필요하다.
- 공유 store나 외부 service의 장애와 복구를 포함한다.
- Host lifecycle, crash 또는 restart가 다른 process의 결과에 영향을 준다.
- 여러 언어 구현 사이에서 같은 payload와 terminal 의미를 확인한다.
- 최종 client까지 이어지는 다단 message 흐름을 확인한다.

한 process의 함수 반환값만으로 충분히 검증할 수 있다면 contract test나 unit test가 더 적합하다.
그 경우 E2E에 같은 검증을 반복하기보다 해당 test의 책임으로 보낸다.

### 3.5 통제 조건과 관찰 결과의 분리

초안을 쓰기 전에 다음 두 목록을 만든다.

| 구분 | 포함할 내용 |
|---|---|
| 통제할 조건 | 역할 수, 배치, fixture, 기동 순서, readiness, 요청 값, 장애 주입 시점 |
| 관찰할 결과 | Client 반환값, public status, public 역할 endpoint의 application evidence, process 종료 상태 |

내부 field를 직접 바꾸거나 Store record를 해석해야만 만들 수 있는 조건은 E2E fixture로 적합한지
다시 검토한다. Application과 E2E client가 public contract로 접근할 수 없는 내부 상태를 성공 기준으로
쓰지 않는다. 장애 주입에 내부 hook이 꼭 필요하다면 공개 계약 검증과 internal protocol test를
분리한다.

### 3.6 최소 배포로 정상 경로를 먼저 쓴다

먼저 계약을 드러내는 가장 작은 정상 배포와 흐름을 작성한다. 그다음 정상 경로와 비교해야
의미가 분명한 실패, restart와 경합 scenario를 추가한다.

최소 배포는 process 수를 무조건 줄인다는 뜻이 아니다. 검증할 경계를 실제로 지나면서도 결과에
영향을 주지 않는 역할과 변형을 제거한다는 뜻이다. Remote routing을 검증하면서 caller와 target을
같은 process에 두면 필요한 경계가 사라지므로 최소 배포가 아니다.

### 3.7 문서와 계약 재대조

초안 뒤에는 다음 항목을 근거 문서와 대조한다.

| 확인 영역 | 확인할 내용 |
|---|---|
| Public surface | 절차가 정식 public API만 사용하는가? |
| 정상 결과 | Reply, state와 side effect가 spec과 같은가? |
| 실패 결과 | Error kind, timeout, cancellation과 cleanup 경계가 같은가? |
| 완료 시점 | Submit 수락, remote 수락, handler 완료와 reply 도착을 구분했는가? |
| 선택과 배치 | 자동 선택, 직접 지정, weight와 capacity 조건이 같은가? |
| 수명 | Ready, drain, shutdown, restart와 stale state 규칙이 같은가? |
| 언어 parity | 모든 지원 언어에서 같은 의미로 구현할 수 있는가? |

자연스럽게 설명하려고 조건을 빼거나, 검증을 강하게 보이려고 spec에 없는 보장을 추가하면 대조
실패다.

## 4. 문서의 큰 구조

Config 문서는 다음 흐름을 출발점으로 삼는다. 내용에 필요 없는 절을 억지로 만들지 말고, 독자가
실행 조건을 이해하는 데 필요한 순서를 유지한다.

1. 사용자가 겪는 상황과 Framework가 제공해야 하는 동작
2. 이 config가 확인하는 범위와 확인하지 않는 범위
3. 역할, process 수와 연결 관계
4. 공통 fixture, 실행 모델과 결과 확인 방법
5. Track별 scenario
6. 관련 정식 계약과 언어별 구현 범위
7. 완료 조건

### 4.1 결과와 범위를 먼저 쓴다

첫 단락은 사용자가 겪는 상황과 Framework가 제공해야 하는 결과를 설명한다. Component 이름,
scenario ID와 test 개수부터 나열하지 않는다. 첫 단락만 읽어도 이 config가 필요한 이유와 정상
동작의 큰 흐름을 말할 수 있어야 한다.

```text
이 config는 provider가 중단되거나 교체되어도 consumer가 공개 topology 상태에 따라
새 target을 선택하고, 이미 시작한 request는 정해진 terminal 결과로 끝나는지 검증한다.
```

이 문장에 제품 용어가 필요하면 바로 다음 문장에서 역할을 설명한다. 예를 들어 topology 상태가
어떤 provider에 새 요청을 보낼 수 있는지 나타내는 값이라면, 그 역할을 설명한 뒤 정식 이름과
계약 문서를 연결한다.

범위 절에는 `다룬다`와 `다루지 않는다`를 함께 둔다. 비슷한 config나 contract test가 소유하는
검증을 링크하면 중복 scenario와 책임 충돌을 줄일 수 있다.

### 4.2 서버 구성의 역할과 이유

역할 표에는 이름과 개수만 쓰지 않는다. 각 역할이 제공하거나 소비하는 공개 기능, 다른 process로
분리해야 하는 이유와 scenario에서 남기는 evidence를 설명한다.

| 역할 | 수 | 설명할 내용 |
|---|---:|---|
| caller | 1 이상 | 어떤 public API로 동작을 시작하고 어떤 결과를 받는가 |
| provider | 검증에 필요한 수 | 어떤 handler나 capability를 제공하고 무엇을 evidence로 남기는가 |
| store | 필요할 때만 | 어떤 공개 provider 계약에 필요하며 실행별 데이터를 어떻게 격리하는가 |
| controller | 필요할 때만 | 어떤 lifecycle 또는 장애 동작을 언제 시작하는가 |

같은 역할을 두 process에 두는 경우 `2`라고만 쓰지 말고 왜 둘이 필요한지 밝힌다. 예를 들어 target
선택, failover 또는 경합 중 무엇을 관찰하기 위한 구성인지 설명한다.

복잡한 topology는 Mermaid로 나타낼 수 있다. Diagram에는 process와 논리 message 방향을 보여주고,
상세 field와 모든 scenario 분기를 한 그림에 넣지 않는다.

### 4.3 공통 실행 모델

모든 scenario가 공유하는 다음 조건은 scenario마다 반복하지 않고 실행 모델 절에 둔다.

- Process 기동과 readiness 확인 방법
- 실행별 port, routing ID, key prefix와 log 경로 격리
- 공통 fixture와 baseline 상태
- Public application evidence endpoint와 client 결과 수집 방법
- 실패 진단용 file log 보존 방법
- 실패 시 보존할 artifact
- 종료와 resource 정리 방법

개별 scenario에서 달라지는 값과 순서만 해당 scenario에 적는다. 공통 실행 모델을 바꾸는
scenario라면 무엇을 바꾸고 실행 뒤 어떻게 복원하는지 명시한다.

## 5. Scenario 작성 방법

### 5.1 기본 구성

Scenario에는 다음 정보를 둔다.

```markdown
#### RL-A1 provider 재시작 뒤 재연결

우선순위: `P0`

Provider가 재시작하면 기존 connection은 사용할 수 없다. Consumer는 재시작한 provider와
새 connection을 설정하고, provider가 요청을 처리할 수 있는 상태가 된 뒤 신규 request를
보내야 한다. 이 동작이 없으면 재시작이 끝났는데도 첫 request가 실패하거나 이전
connection으로 전달될 수 있다.

**검증 질문:** 같은 identity의 provider가 재시작한 뒤 consumer가 새 connection으로
request를 보내고 reply를 받는가.

- 시작 조건: provider와 consumer가 연결되어 있고 baseline request가 한 번 성공했다.
- 절차: Provider를 정상 종료하고
  같은 identity로 다시 시작한다. Public status에서 이전 connection 제거와 새 connection의
  ready 전이를 확인한 직후 request를 보낸다.
- 검증: baseline과 재시작 뒤 request가 각각 한 번 처리된다. 두 번째 request는 application
  retry 없이 새 provider의 public application evidence에 기록되고, client는 정해진 reply를 받는다.
  Public status에 이전 peer가 ready 상태로 남거나 handler가 중복 실행되면 실패다.
- 세부 동작: [Transport liveness §6](../../../framework/doc/framework/common/spec/29-transport-liveness.ko.md#6-connection-loss와-reconnect)의
  reconnect와 target readiness 계약을 검증한다.
```

제목과 priority 다음의 설명은 생략하지 않는다. 이 설명은 `무엇을 시험하는가`만 반복하지 않고,
시작 상황, 정상 동작과 이 동작이 필요한 이유를 연결한다. 같은 설명이 Track의 첫머리에 있고 각
scenario가 작은 변형만 다룬다면 공통 내용은 Track에서 한 번 설명하고 scenario에는 달라지는 상황만
쓴다.

이 형식은 정보를 찾는 위치를 일정하게 만드는 출발점이다. 시작 상태가 단순하면 `시작 조건`을
설명 문단이나 절차의 첫 문장에 넣을 수 있다. 복잡한 경합에서 단계별 barrier를 설명해야 하면 별도
항목을 추가할 수 있다. 그러나 쉬운 상황 설명, 검증 질문, 절차와 검증 기준은 항상 구분할 수 있어야
한다.

### 5.2 제목은 조건과 결과를 드러낸다

제목에는 scenario ID와 독자가 구분해야 하는 핵심 동작을 쓴다.

| 읽기 쉬운 제목 | 피할 제목 |
|---|---|
| `Provider crash 중 in-flight request 종료` | `Crash test` |
| `Weight 0 전파 뒤 신규 request 제외` | `Weight 동작` |
| `Store 복구 뒤 owner lease 갱신 재개` | `Recovery scenario 2` |

제목에 모든 조건을 넣어 문장처럼 길게 만들지 않는다. 상세 조건은 검증 질문과 절차가 설명한다.

### 5.3 검증 질문은 관찰 가능한 결과를 묻는다

검증 질문에는 입력 조건, 대상 동작과 기대 결과가 있어야 한다. `정상인가`, `올바른가`, `문제가
없는가`처럼 판정 기준이 없는 표현은 피한다.

한 질문에 `그리고`가 여러 번 나오면 다음을 확인한다.

- 하나의 terminal 결과를 설명하기 위해 함께 필요한 조건인가?
- 서로 다른 public contract를 한 번에 검증하고 있지는 않은가?
- 첫 조건이 실패해도 뒤 조건을 독립적으로 판정할 수 있는가?

독립적으로 판정할 수 있으면 scenario를 나눈다. 같은 request의 중복 실행 금지와 terminal-once처럼
한 결과를 함께 정의하는 조건은 한 scenario에 둘 수 있다.

### 5.4 절차는 재현 가능한 상태 변경을 쓴다

절차는 누가, 무엇을, 언제 하는지 순서대로 설명한다. 다음 정보를 빠뜨리지 않는다.

1. 시작 시점의 topology와 상태
2. Baseline이 필요하면 먼저 확인할 정상 결과
3. 요청이나 상태 변경을 시작하는 주체
4. Crash, pause, timeout 또는 경쟁을 주입하는 정확한 시점
5. 다음 단계로 넘어가게 하는 readiness나 barrier
6. 마지막으로 수집할 결과

`잠시 기다린 뒤`, `충분히 요청한 뒤`, `적절한 시점에 종료한다`처럼 실행마다 달라지는 표현은
사용하지 않는다. Readiness marker, handler-start evidence, 고정된 request 수나 공개 state 전이를
기준으로 다음 단계를 시작한다.

시간 자체가 계약이면 timeout과 허용 범위를 수치로 쓴다. 시간이 계약이 아니라 순서만 중요하면
고정 sleep 대신 사건 기반 barrier를 사용한다고 적는다.

### 5.5 검증은 값과 금지 결과를 함께 쓴다

검증 항목은 `성공한다`, `정상이다`, `에러가 난다`로 끝내지 않는다. 다음 내용을 필요한 만큼
명시한다.

- 어느 process에서 어떤 값을 관찰하는가
- 몇 번 호출되거나 기록되어야 하는가
- 어떤 identity, generation, sequence나 payload가 보존되어야 하는가
- 어느 public status나 terminal result가 나와야 하는가
- 어느 시간 범위에 끝나야 하는가
- 어떤 fallback, replay, 중복 처리 또는 stale 상태가 없어야 하는가

성공 evidence와 부재 evidence를 구분한다. 특정 log가 없다는 사실만으로 업무 처리가 없었다고
단정하지 않는다. 요청 처리 횟수처럼 부재를 검증해야 하면 bounded evidence counter, 고유 marker와
최종 상태를 함께 사용한다.

### 5.6 Evidence의 출처와 의미를 밝힌다

Evidence는 test가 본 값을 다른 개발자가 같은 뜻으로 해석할 수 있게 정의한다.

| Evidence | 확인할 수 있는 내용 | 단독으로 확인할 수 없는 내용 |
|---|---|---|
| Client 반환값 | Caller가 받은 reply, error와 완료 시점 | Remote handler가 중복 실행되지 않았는지 |
| 역할 server evidence | Handler 호출 횟수, 입력 값과 application state | Caller가 실제 reply를 받았는지 |
| Public status | Ready target, peer와 lifecycle 상태 | 특정 request의 handler 완료 여부 |
| Structured log | 실패 원인을 조사할 때 message 단계와 correlation을 참고할 수 있다. | 일반 scenario의 통과 여부. Public tracing 계약을 직접 검증할 때만 예외다. |
| Process exit status | 기동 또는 종료 성공 여부 | Message 처리의 정확성 |

하나의 evidence로 확인할 수 없는 계약은 두 출처를 조합한다. 예를 들어 request 성공은 client reply와
provider의 handler evidence를 함께 확인해야 중간 transport 성공과 중복 실행을 구분할 수 있다.

Application과 E2E client는 provider의 private Store record, 내부 queue, connection counter와 private
protocol frame을 직접 읽어 성공을 판정하지 않는다. 공개 결과가 부족하다면 test-only 우회부터 만들지
말고 public contract와 관측 책임을 다시 검토한다.

### 5.7 `세부 동작`의 추적 근거

`세부 동작`에는 scenario를 짧게 다시 말하는 label만 두지 않는다. 어떤 정식 계약의 어느 결과를
검증하는지 링크한다. 여러 계약이 한 흐름에서 만나는 경우에도 scenario의 주된 판정 근거를 먼저
제시한다.

이렇게 쓴다.

> 세부 동작: [Host Relocate와 Shutdown §12](../../../framework/doc/framework/common/spec/28-graceful-drain-handoff.ko.md#12-state별-admission)의
> 신규 admission 차단과 이미 수락한 request의 완료 경계를 검증한다.

이렇게 쓰지 않는다.

> 세부 동작: shutdown 처리.

정식 계약 링크가 없다면 현재 E2E가 새 계약을 먼저 요구하고 있지 않은지 확인한다.

## 6. 정상, 실패와 경합 Scenario

### 6.1 정상 경로는 이후 scenario의 대조군이 된다

정상 scenario는 기능이 한 번 성공한다는 사실만 확인하지 않는다. 이후 실패 scenario가 비교할
최소 evidence를 정한다.

- 선택한 target identity
- Handler 실행 횟수
- Request와 reply payload
- Public status의 시작값과 완료값
- Public status와 application evidence marker

실패 scenario는 같은 값을 사용해 무엇이 달라지고 무엇이 유지되는지 설명한다.

### 6.2 Failure 위치와 caller 결과

실패 scenario에는 다음 경계를 함께 쓴다.

1. 어떤 component의 어느 상태에서 실패를 만든다.
2. 요청이 아직 제출되지 않았는지, 이미 remote에 제출되었는지 밝힌다.
3. Caller가 받는 error, timeout 또는 cancellation을 밝힌다.
4. Pending 작업, reservation과 connection이 어떻게 정리되는지 밝힌다.
5. 다른 target이나 다음 request가 영향을 받는지 확인한다.

`실패하면 retry한다`고만 쓰지 않는다. Runtime의 자동 retry인지 application이 새 operation을 시작하는
것인지 구분하고, 자동 replay가 금지되는 경로에서는 중복 실행이 없다는 evidence를 요구한다.

### 6.3 경합 barrier와 terminal 수

Race scenario는 운에 맡긴 반복 실행으로 작성하지 않는다. 경쟁하는 각 작업이 어느 지점에 도달했는지
확인하는 barrier를 두고, 그 뒤 동시에 진행시킨다.

다음 결과를 명시한다.

- 경쟁에 참여하는 caller 또는 node 수
- 같은 identity나 resource를 두고 경쟁하는지 여부
- 성공할 수 있는 작업 수
- 각 작업이 도달해야 하는 terminal 결과
- 생성, handler 실행과 cleanup의 전체 횟수
- 승자를 판정하는 public state

내부 동기화 hook으로만 만들 수 있는 race는 E2E와 internal test의 책임을 분리한다. E2E에서는 process
pause, 외부 store 지연, public lifecycle control처럼 배포 환경에서도 의미가 있는 수단을 우선한다.

### 6.4 Timeout과 cancellation의 적용 구간을 쓴다

Timeout 값만 적지 말고 어느 구간을 제한하는지 설명한다. Cancellation도 caller의 대기를 끝내는지,
remote 실행까지 중단하는지 구분한다.

이미 remote에 제출된 작업이 계속 실행될 수 있다면 caller timeout 뒤의 server evidence를 확인한다.
그 결과를 retry 안전성이나 중복 실행 보장으로 확대 해석하지 않는다.

## 7. Config와 Scenario를 나누는 기준

새 config는 별도 배포 topology나 공통 fixture가 필요할 때 만든다. 같은 역할과 실행 모델을 공유하며
입력과 기대 결과만 달라지면 기존 config의 새 Track 또는 scenario로 둔다.

| 변화 | 배치 위치 |
|---|---|
| 같은 process 구성에서 다른 public 동작 검증 | 같은 config의 새 Track 또는 scenario |
| 역할 수, store 종류나 process 경계가 본질적으로 다름 | 새 config 후보 |
| 기존 scenario의 기동 순서·RID 방향·분리 배치 변형 | 같은 scenario의 구성 축 |
| 한 언어에만 있는 구현 상세 | 공통 scenario가 아니라 언어별 test 또는 gap |

Track은 독자가 비슷한 질문을 함께 찾게 하는 분류다. 파일 분량을 나누기 위해 의미 없는 Track을
만들지 않는다. 하나의 Track 안에서는 정상 경로에서 실패·복구처럼 독자가 이해하기 쉬운 순서를
사용한다.

## 8. 언어별 parity를 유지하는 방법

공통 scenario는 언어마다 API 철자가 달라도 같은 동작을 검증해야 한다. 다음 값은 가능한 한 공통으로
고정한다.

- Scenario ID와 priority
- 역할 이름과 topology 의미
- Fixture payload와 marker
- 요청 수, 순서와 장애 주입 조건
- Terminal 결과와 evidence 의미
- 성공·실패 판정

언어별 문서나 test code에서는 실제 public API 이름을 사용할 수 있다. 공통 문서의 산문에 여러 언어의
method 이름을 나열하지 않는다. 호출 방식의 차이가 검증에 영향을 줄 때만 언어별 interface를
링크한다.

특정 언어에서 scenario를 구현할 수 없으면 skip만 추가하지 않는다. Feature map에 다음 내용을 남긴다.

- 누락된 public contract 또는 runtime 기능
- 근거 spec
- 사용자가 관찰하는 동작 차이
- 구현을 시작할 수 있는 조건
- 현재 대신 검증하는 범위가 있다면 그 정확한 경계

## 9. 읽기 쉬운 문장과 구성

읽기 쉬운 문서는 기술 내용을 줄인 문서가 아니다. 독자가 이미 알고 있는 상황에서 시작해, 새로운
상태와 용어를 한 번에 하나씩 더하는 문서다. 각 절은 다음 순서로 설명한다.

1. 사용자가 보게 되는 상황이나 결과
2. Framework가 그 결과를 만들기 위해 보장하는 동작
3. 그 동작을 구분하는 정식 용어와 public identifier
4. E2E가 실행할 조건과 확인할 값

처음부터 API, state와 내부 구성 요소를 모두 제시하면 각각의 관계를 이해하기 어렵다. 쉬운 설명과
정확한 판정 기준을 위 순서로 연결하되, 같은 내용을 여러 표현으로 반복하지 않는다.

### 9.1 한 문단에는 하나의 판단만 둔다

동작 주체, 시간 단계, 정상·실패 조건 또는 evidence 출처가 바뀌면 문장을 나누거나 새 문단으로 옮긴다.
하나의 bullet에 절차와 모든 예외를 함께 넣어 긴 문장으로 만들지 않는다.

### 9.2 주어와 동작을 밝힌다

`연결 후 검증한다`, `복구되어 처리된다`처럼 주체가 없는 문장을 피한다.

이렇게 쓴다.

> Runner가 provider의 readiness marker를 확인한 뒤 client를 시작한다. Client는 application retry 없이
> 첫 request를 보낸다. Provider는 받은 marker와 handler 호출 횟수를 evidence에 기록한다.

각 문장에서 runner, client, provider 중 누가 동작하는지 확인할 수 있다.

### 9.3 용어 설명 순서

처음 나오는 제품 용어는 현재 scenario에서 하는 일을 설명한 뒤 이름을 붙이고 관련 glossary나 spec을
링크한다. `admission`, `fence`, `authority`, `generation` 같은 명사만으로 절차를 대신하지 않는다.

이렇게 쓴다.

> Runtime은 종료를 시작한 node를 신규 요청의 자동 선택 대상에서 제외한다. 이미 그 node가 수락한
> 요청은 정해진 완료 시점까지 처리한다. 신규 요청을 차단하는 이 상태를 `Draining`이라고 한다.

이렇게 쓰지 않는다.

> `Draining` admission fence와 in-flight completion을 검증한다.

Public identifier, error 이름과 state 값은 선언된 철자를 유지한다. 그 옆에는 test가 관찰하는 의미를
쓴다.

Server 개발자가 일상적으로 사용하는 request, handler, timeout과 retry 같은 용어는 영어로 유지한다.
제품에만 있는 개념이나 문맥에 따라 뜻이 달라지는 용어는 독자가 이름을 외우기 전에 역할과 필요성을
이해할 수 있게 설명한다.

### 9.4 축약 영어 명사구

`restart convergence validation`, `owner failover evidence`처럼 영어 명사를 이어 붙이지 않는다.
무엇이 언제 바뀌고 어떤 값으로 확인되는지 문장으로 쓴다.

### 9.5 표와 diagram 사용 조건

역할, 상태별 기대 결과와 언어별 구현 범위는 표가 적합하다. 시간 순서와 경합 단계는 번호 목록이나
Mermaid sequence diagram이 적합하다. 짧은 scenario 하나를 표로 바꾸면 주체와 흐름이 오히려 끊기므로
산문을 사용한다.

Diagram은 정상 흐름 또는 특정 경합 하나만 보여준다. Diagram 아래에는 생략한 실패 경로와 판정 기준을
산문으로 설명한다. ASCII diagram을 사용하면 저장소의 영문 전용·고정 폭 규칙을 따른다.

## 10. 흔한 이탈과 수정 방법

| 이탈 | 문제 | 수정 방법 |
|---|---|---|
| 기능 목록만 나열한다 | 무엇을 실행하고 어떻게 판정하는지 알 수 없다. | 각 항목을 검증 질문·절차·검증으로 연결한다. |
| 절차가 `request를 보내 확인한다`로 끝난다 | 시작 상태와 evidence가 없다. | 주체, readiness, 입력, 관측 위치와 기대 값을 쓴다. |
| 검증이 `정상 동작한다`로 끝난다 | 자동화할 assertion을 정할 수 없다. | 값, 횟수, terminal 결과와 금지 결과를 쓴다. |
| 내부 Store record를 성공 기준으로 쓴다 | Public contract가 아니라 구현을 고정한다. | Client 결과, public status와 역할 server의 public application evidence로 바꾼다. |
| Sleep으로 race를 만든다 | 실행 환경에 따라 결과가 달라진다. | Readiness, handler-start marker나 barrier를 사용한다. |
| 실패마다 application retry를 넣는다 | Runtime 결함과 첫 시도 실패를 가린다. | 첫 operation의 terminal 결과를 먼저 판정하고 retry는 별도 scenario로 둔다. |
| 다른 언어 API를 근거로 기능을 추가한다 | E2E가 계약 출처가 된다. | Spec 근거를 확인하고 없으면 설계 gap으로 분리한다. |
| Scenario 하나가 여러 독립 실패를 주입한다 | 어느 조건이 결과를 만들었는지 알 수 없다. | 장애 조건과 terminal 결과별로 scenario를 나눈다. |
| 내부 counter나 로그 한 줄로 성공을 판정한다 | Public contract가 아닌 구현 상세를 고정하고 업무 결과도 확인할 수 없다. | Client 결과, public status와 public 역할 endpoint의 application evidence로 바꾼다. |
| 구현 class와 helper 이름으로 흐름을 설명한다 | 다른 언어에서 같은 의미로 옮기기 어렵다. | 역할, public operation과 관찰 결과로 설명한다. |

## 11. 작성 후 검증

초안을 다 썼다는 사실만으로 scenario가 완성되지는 않는다. 작성자는 아래 네 단계를 순서대로 다시
검토한다. 문장이 자연스러운지를 보기 전에, 정식 spec의 기능을 실제로 실행하고 그 결과를 판정할 수
있는지부터 확인한다.

### 11.1 정식 spec과 assertion 대조

Scenario가 주장하는 정상 결과, 실패 결과와 완료 시점을 관련 정식 spec에서 다시 찾는다. Spec 링크가
문서에 있다는 사실만 확인하지 않고, 각 assertion이 링크한 절의 문장으로 직접 뒷받침되는지 확인한다.

다음 표를 scenario별로 작성하면 빠진 근거를 찾기 쉽다.

| 항목 | 리뷰할 질문 |
|---|---|
| Spec의 기능 | 어떤 public 동작을 보장하는가? |
| 시작 조건 | 그 기능이 실행되는 정확한 조건을 만들었는가? |
| Public operation | 어느 public API 호출이 기능을 실제로 시작하는가? |
| 완료 조건 | Spec이 정의한 reply, error, status 또는 callback 중 무엇으로 끝나는가? |
| 금지 결과 | 자동 replay, 중복 실행이나 stale target처럼 나오면 안 되는 결과는 무엇인가? |

Spec이 `허용한다`, `보장하지 않는다` 또는 여러 terminal 결과 중 하나를 허용한다고 쓴 부분을 E2E가
하나의 결과로 좁히지 않는다. 반대로 spec이 반드시 보장하는 결과를 `A 또는 B`처럼 느슨하게 만들지
않는다. E2E가 spec보다 강하거나 약한 계약을 새로 만들면 리뷰 실패다.

### 11.2 Spec 기능 실행 여부

API를 한 번 호출했다는 사실만으로 해당 기능을 검증했다고 판단하지 않는다. Procedure가 검증하려는
분기를 실제로 지나야 한다.

예를 들어 automatic discovery를 검증한다면 consumer 구성에 provider endpoint가 없어야 하고, provider와
consumer는 서로 다른 process여야 한다. Timeout을 검증한다면 handler 시작을 확인한 뒤 deadline이 먼저
끝나는 조건을 만들어야 한다. Weight 분포를 검증한다면 두 target이 모두 ready인 상태에서 충분한 수의
operation을 보내고 의미 있는 허용 범위로 결과를 판정해야 한다.

다음 질문 중 하나라도 `아니요`이면 scenario를 다시 설계한다.

1. 시작 조건을 public 설정, 역할 server의 application state와 runner의 process 제어만으로 만들 수 있는가?
2. 검증할 기능이 실행됐다는 사실을 public status, public result 또는 application handler evidence로
   확인할 수 있는가?
3. 해당 기능을 고의로 깨뜨린 구현이라면 적어도 하나의 assertion이 반드시 실패하는가?
4. 다른 기능이 우연히 같은 결과를 만들어 scenario가 잘못 통과할 가능성이 없는가?
5. 모든 지원 언어가 private helper나 raw frame 없이 같은 조건과 판정을 구현할 수 있는가?

Public API로 기능 진입 여부를 확인할 수 없으면 내부 counter를 E2E에 추가하지 않는다. Contract test나
internal protocol test로 옮기거나, public 관측 계약이 필요한 별도 설계 문제로 분리한다.

Ready Actor·Spot·Instance Spot owner process가 종료된 경우에는 다른 node가 자동으로 새 owner가 된다고
가정하지 않는다. 정식 failover 정책이 자동 복원을 제공하지 않으면 E2E는 `Unavailable`과 handler 미실행을
확인하고, Application의 explicit recreate·rebind를 별도 operation으로 검증한다. `Creating` 상태의
recovery는 같은 generation을 계속하거나 취소할 수 있으므로 하나의 결과로 임의로 좁히지 않는다.

### 11.3 Flaky 조건

정상 구현이 같은 입력에서 반복적으로 같은 판정을 받는지 검토한다. 다음 조건에 의존하면 E2E가
간헐적으로 실패할 수 있다.

- 고정 sleep이 끝날 때까지 상태가 바뀔 것이라는 가정
- Thread scheduling, handler 실행 순서나 process 시작 순서
- Core·OS socket buffer 크기처럼 public contract가 정하지 않은 내부 capacity
- public 설정으로 노출되지 않은 pending waiter·queue limit을 정확한 경계로 사용하는 조건
- Public status에 없는 값이 전파됐을 것이라는 추정
- Message마다 정확히 번갈아 target을 선택한다는 가정
- Classic fanout에서 subscriber 간 동일한 event 순서나 lossless delivery를 가정하는 조건
- 표본 수가 너무 작거나 정상 변동보다 좁은 통계 허용 범위
- Log가 특정 시각에 flush되거나 특정 순서로 기록된다는 가정

시간이 아니라 사건의 순서가 중요하면 public status sequence, handler-start evidence와 bounded wait를
barrier로 사용한다. 통계적 분포를 검증할 때는 충분한 표본과 계약을 위반한 구현을 걸러낼 수 있을 만큼
의미 있으면서 정상 변동은 수용하는 범위를 함께 정한다. 실패할 때까지 같은 scenario를 반복 실행하여
우연히 통과한 결과를 채택하지 않는다.

내부 capacity를 알아야만 만들 수 있는 backpressure, race나 exact boundary는 E2E에 억지로 넣지 않는다.
Public 설정과 status로 조건 도달을 확인할 수 있는 범위만 E2E에 남기고 나머지는 통제 가능한 contract
test가 담당한다.

### 11.4 문서와 자동 검사

계약과 실행 가능성 리뷰를 통과한 뒤 다음 순서로 문서를 다시 읽는다.

1. 첫 설명만 읽고 사용자가 겪는 상황, 정상 동작과 실패했을 때의 문제를 말할 수 있는지 본다.
2. 제목과 검증 질문만 읽고 scenario 하나가 판정하는 결과를 설명할 수 있는지 본다.
3. 서버 구성과 절차만 읽고 필요한 process와 실행 순서를 재현할 수 있는지 본다.
4. 검증 항목만 읽고 자동 assertion과 필요한 public evidence를 정할 수 있는지 본다.
5. 공통 E2E README의 ID, priority, runner와 evidence 규칙에 맞는지 확인한다.
6. [기술문서 작성 원칙](documentation-principles.ko.md)에 따라 독자, 용어, 문체와 표 사용을 다시
   검토한다.
7. 상대 링크, Markdown 표, code fence와 diagram 문법을 확인한다.
8. `git diff --check`와 문서 링크 검사를 실행한다.

자동 검사가 통과해도 spec 기능을 실행하지 못하거나 public 결과로 판정할 수 없으면 완료로 보지 않는다.

## 12. 완료 점검표

### 범위와 계약

- [ ] 첫 단락에서 사용자가 겪는 상황과 Framework가 제공해야 하는 동작을 쉽게 설명했다.
- [ ] 동작이 깨졌을 때 사용자에게 어떤 문제가 보이는지 설명했다.
- [ ] 다루는 범위와 다른 config 또는 test가 소유하는 범위를 구분했다.
- [ ] 모든 public 동작에 정식 spec 근거가 있다.
- [ ] 각 assertion을 정식 spec의 구체적인 문장과 대조했다.
- [ ] Spec이 허용한 결과를 임의로 좁히거나 보장한 결과를 느슨하게 만들지 않았다.
- [ ] E2E나 다른 언어 구현만 근거로 새 public contract를 추가하지 않았다.
- [ ] E2E에서만 확인할 process, transport, store 또는 언어 경계가 분명하다.
- [ ] Framework 동작의 실행과 판정에 정식 public API만 사용한다.
- [ ] Internal helper, private protocol, Store record와 내부 counter를 통과 조건으로 사용하지 않는다.
- [ ] Ready owner crash를 자동 takeover로 해석하지 않고 정식 failover 결과와 explicit recreate 경계를
  분리했다.
- [ ] Public activation concurrency와 내부 pending capacity를 구분했다.

### 구성과 실행

- [ ] 역할별 수, public 기능과 분리 배치 이유를 설명했다.
- [ ] 외부 resource와 실행별 격리 방법을 설명했다.
- [ ] Readiness를 sleep이 아닌 관찰 가능한 조건으로 판정한다.
- [ ] Procedure가 이름만 언급하지 않고 검증하려는 spec 기능을 실제로 실행한다.
- [ ] 조건 도달을 public status, result 또는 application evidence로 확인한다.
- [ ] 공통 실행 모델과 scenario별 변형을 구분했다.
- [ ] 실패 시 남길 client 결과와 public application evidence를 정하고 진단용 log를 구분했다.

### Scenario

- [ ] 각 scenario에 ID, 제목, priority와 하나의 검증 질문이 있다.
- [ ] 검증 질문 전에 시작 상황, 정상 동작과 확인 이유를 쉬운 문장으로 설명했다.
- [ ] 절차에 시작 상태, 동작 주체, 입력, 순서와 barrier가 있다.
- [ ] 검증에 기대 값, 횟수, terminal 결과와 금지 결과가 있다.
- [ ] 성공과 실패를 public 결과와 역할 server의 public application evidence로 판정한다.
- [ ] `세부 동작`이 관련 정식 계약을 정확히 가리킨다.
- [ ] Timeout, cancellation과 retry의 적용 구간을 구분했다.
- [ ] Race scenario의 참여자, barrier와 terminal 수를 고정했다.
- [ ] 검증 대상 기능을 깨뜨린 구현이 이 assertion을 그대로 통과할 수 없는지 확인했다.
- [ ] 내부 buffer 크기, scheduler 순서와 status에 없는 전파 시점에 의존하지 않는다.
- [ ] 통계 scenario는 충분한 표본 수와 정상 변동을 수용하는 허용 범위를 사용한다.

### 언어 parity와 설명

- [ ] 언어별 API 모양과 공통 scenario 의미를 구분했다.
- [ ] 미구현 기능은 skip이 아니라 feature map에 gap과 후속 조건을 남긴다.
- [ ] 한 문단에는 주된 판단 하나만 있다.
- [ ] 주어, 상태 변경과 관찰 결과가 문장에 드러난다.
- [ ] 처음 나오는 용어는 하는 일을 설명한 뒤 이름과 근거 문서를 연결했다.
- [ ] 영어 명사를 이어 붙인 압축 표현과 구현 내부 이름을 제거했다.

### 최종 확인

- [ ] [기술문서 작성 원칙](documentation-principles.ko.md)에 따라 최종 문장을 다시 검토했다.
- [ ] 상대 링크가 실제 문서와 anchor를 가리킨다.
- [ ] 표, code fence와 diagram 문법이 올바르다.
- [ ] `git diff --check`가 통과한다.
- [ ] 처음 읽는 개발자가 문서만으로 무엇을 테스트하는지 먼저 설명할 수 있다.
- [ ] 같은 개발자가 이어서 실행 조건과 통과 기준을 설명할 수 있다.
