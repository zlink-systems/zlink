# 검증 스크립트 needle 갱신표

검증 방법: `scripts/verify-framework-instance-spot-contracts.sh`가 쓰는 것과 동일한
whitespace-normalize 로직(`source.replace(/\s+/gu, ' ').trim()`)으로 새 문서 전체를
normalize한 뒤 `includes()` substring 검사를 Node로 재현해서 확인했다. 아래 "새 needle
원문" 블록의 11줄은 **이 파일에서 직접 읽어** 같은 검사를 통과시킨 것이라
복사-붙여넣기 대상이다(표 안의 문구는 참고용 요약이며, 백틱을 포함한 원문은 블록
쪽을 따른다).

**결론: 11건 전부 재서술 — 규칙 손실 없음(누락 0건). 문서 수정 없음.**

7~11번은 처음에 "새 문서에 없음"으로 보고됐으나, 이는 일반 `grep -n`이 줄바꿈으로
꺾인 문장(마크다운 하드랩)을 못 잡은 것이었다. 스크립트 자신의 whitespace-normalize
비교로는 옛 문장이 단어 하나 안 바뀌고 그대로 새 문서에 있다.

| # | 판정 | 새 문서 |
|---|---|---|
| 1 | 재서술 — backtick 대신 glossary 링크, 문장이 2·3번과 합쳐짐 | 03-spot-actor/02-spot-messaging.ko.md |
| 2 | 재서술 — 1번과 한 문장으로 합쳐짐(끝나며→절 연결), 순서만 바뀜 | 03-spot-actor/02-spot-messaging.ko.md |
| 3 | 재서술 — "reply" 부분은 삭제된 게 아니라 바로 아래 activation envelope 필드 표에 reply correlation 행으로 명시적으로 남아 있음(아래 검증 참고) | 03-spot-actor/02-spot-messaging.ko.md |
| 4 | 재서술(의도된 제목 개선, 되돌리지 않음) | 03-spot-actor/06-spot-address-messaging.ko.md |
| 5 | 재서술(의도된 제목 개선, 되돌리지 않음) | 03-spot-actor/06-spot-address-messaging.ko.md |
| 6 | 재서술 — 문장이 끝나지 않고 계속됨(최초 message가 항상 먼저 처리된다는 조건 추가, 규칙은 보존) | 03-spot-actor/06-spot-address-messaging.ko.md |
| 7 | 재서술(사실상 무변경) — 원문 그대로 존재. 하드랩(줄바꿈)만 다름, 일반 grep이 놓쳤을 뿐 | 03-spot-actor/05-spot-actor-membership.ko.md |
| 8 | 재서술(사실상 무변경) — 원문 그대로 존재. 하드랩만 다름 | 05-location-relocation/01-location-runtime.ko.md |
| 9 | 재서술(사실상 무변경) — 원문 그대로 존재. 하드랩만 다름 | 05-location-relocation/01-location-runtime.ko.md |
| 10 | 재서술(사실상 무변경) — 원문 그대로 존재. 하드랩만 다름 | 05-location-relocation/01-location-runtime.ko.md |
| 11 | 재서술(사실상 무변경) — 원문 그대로 존재. 하드랩만 다름 | 05-location-relocation/01-location-runtime.ko.md |

참고: 1·2번은 새 문서에서 한 문장으로 합쳐졌다. 스크립트 쪽에서 두 `required`
항목을 1번 새 needle(전체 문장) 하나로 합치는 편을 권장하지만(재-리플로우에 더
강건), 2번을 별도로 남겨도 지금은 통과한다(1번 새 needle의 부분 문자열이므로).
7~11번의 새 needle은 옛 needle과 글자 그대로 동일하다(스크립트 수정 불필요, 경로만
바뀌면 됨).

## 새 needle 원문 (기계가 읽는 블록, 한 줄에 하나, 번호 순서)

```
Spot direct call에 [Instance intent](../01-glossary.ko.md#instance-intent)(Spot이 없으면 새로 준비하라는 명시적 선택)가 없는데 target Spot이 존재하지 않으면 `NotFound`로 끝나며, Framework는 [Spot kind](../01-glossary.ko.md#spot-kind), stable type과 최초 배치 위치를 담은 생성 정보를 만들지 않는다.
target Spot이 존재하지 않으면 `NotFound`로 끝나며
Source가 target node를 선택하고 최초 application message와 Spot 생성에 필요한 정보를 하나의 [activation envelope](../01-glossary.ko.md#activation-envelope)로 함께 전달하는 절차, target의 생성 권한 확보, durable inbox 복원과 barrier 개방 순서는 [Spot 주소 메시징](06-spot-address-messaging.ko.md)이 정의한다.
## 3. User Spot 명시적 생성 — Create와 GetOrCreate
## 4. Cold activation — message로 Instance Spot을 처음 만드는 방법
생성 권한을 얻은 target만 자신을 owner로 기록하고 factory를 실행하며, 그 Spot이 처리하는 message 중 cold activation의 최초 message가 항상 먼저 처리된다.
Instance Spot은 source가 first-message activation envelope를 후보 target에 먼저 제출하고 target activation registry가 `Reserve`를 호출한다.
Instance Spot은 별도 생성 API를 사용하지 않는다.
Instance Spot 요청임을 표시했고 Spot이 없을 때만, message를 받은 target node가 Spot을 만든다.
동시에 여러 target이 시도해도 성공한 하나만 factory를 실행한다.
Record가 없을 때만 최초 message를 Relocation Store에 저장하고 `Creating` record와 수용 공간을 함께 확보한다.
```

## 검증 — needle 3의 "reply" 보존 확인

옛 문장은 "Spot 생성·reply에 필요한 정보"를 하나의 전달 단위에 넣는다고 했다. 새
`03-spot-actor/02-spot-messaging.ko.md`(같은 파일, needle 3 문장 바로 아래)에
activation envelope가 보존하는 정보 표가 있고, 거기 다음 행이 명시적으로 있다:

```
| Reply를 request와 연결하는 값(`reply correlation`) | Request의 reply를 원래 호출과 연결한다. |
```

`06-spot-address-messaging.ko.md`에도 envelope 필드 나열에 `reply correlation`이
그대로 있다(300행 부근). 따라서 "reply" 부분은 누락이 아니라 표 형태로 재서술됐다.

## 15-spot-actor.ko.md 그룹 전수 확인(closing-lifecycle 6건 포함)

`15-spot-actor.ko.md`가 pin하는 나머지 6개 needle(OnClosing, ExplicitClose 등
closing 관련)도 전수 확인했다. 새 트리에 `03-spot-actor/09-object-lifecycle.ko.md`가
따로 있어 분리됐을 가능성을 의심했으나, 6건 전부 `05-spot-actor-membership.ko.md`
하나에 원문 그대로 있다(문서가 나뉘지 않음). 이 needle 그룹은 여러 파일로 쪼갤 필요
없이 `05-spot-actor-membership.ko.md` 단일 경로면 된다.

## 12-spot-messaging.ko.md / 16-spot-address-messaging.ko.md 그룹 전수 확인

각 파일이 pin하는 나머지 needle들도 모두 확인했다 — `### 3.2` 제목, 표 행
(`| Instance [Spot application queue]`), `Instance intent가 없는 Missing Spot
message가 ... 만들지 않는다` 계열 문장, `SpotHandle` 부재 문장, `Spot manager의
public Close` 문장 등은 전부 원문 그대로 새 문서에 있다. 두 그룹 모두 단일 새 파일
(`02-spot-messaging.ko.md`, `06-spot-address-messaging.ko.md`)로 옮기면 충분하다.

## 스크립트 수정 지침

`scripts/verify-framework-instance-spot-contracts.sh`의 `formalFixtures` 배열에서,
이동 시점에 각 항목의 `path`를 다음처럼 바꿔야 한다(이 세션에서는 스크립트를 건드리지
않았다 — 이동 단계의 몫):

| 옛 `path` | 새 `path` |
|---|---|
| `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md` | `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md` |
| `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md` | `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md` |
| `framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md` | `framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md` (그룹 전체가 한 파일에 있음 — 분할 불필요) |
| `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md` | `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md` (그룹 전체가 한 파일에 있음) |

각 그룹의 `required` 배열 안에서, 위 "새 needle 원문" 블록의 11줄만 번호 순서대로
교체하면 된다(1번째 줄→1번, ... 11번째 줄→11번). 나머지 needle(각 그룹에서 이번
조사로 이미 원문 그대로 확인된 것들)은 고칠 필요 없다 — 경로만 바뀌면 그대로
통과한다.

참고(범위 밖): `formalFixtures`의 다섯 번째 항목은
`framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md`를 연다. 이
캠페인이 이 파일도 옮긴다면 그 경로 역시 이동 시점에 갱신해야 하지만, 이번 작업은
지정된 11개 needle(12/16/15/21번 문서 기원)만 다뤘으므로 06-framework-api 쪽
needle은 검증하지 않았다.

## 복원한 규칙

없음 — 11건 모두 새 문서에서 규칙이 그대로 확인됐다(7~11번은 문구까지 완전히
동일, 1~6번은 문구만 재서술). 원문에서 빠진 규칙은 발견되지 않았으므로 가이드
§2.5에 따라 새로 지어 넣거나 복원한 문장이 없다. 4개 대상 spec 파일
(`03-spot-actor/02-spot-messaging.ko.md`, `03-spot-actor/06-spot-address-messaging.ko.md`,
`03-spot-actor/05-spot-actor-membership.ko.md`, `05-location-relocation/01-location-runtime.ko.md`)
전부 **수정하지 않았다**.
