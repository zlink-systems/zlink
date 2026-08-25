# spec/server 재구성 캠페인 — 작업 지침

> 이 문서 하나로 새 작업자가 캠페인을 이어받아 진행할 수 있다. 배경, 규칙, 진행 상태,
> 다음에 할 일, 검증 방법을 담는다. 작업을 끝낼 때마다 **§8 진행 상태를 갱신한다.**
>
> 시작 시점 2026-08-25. 기준 commit `f1a2f416f6`.

## 1. 무엇을 하는 캠페인인가

`framework/doc/framework/common/spec/server`의 47개 문서(× ko/en)를 **주제별로 재구성하고,
[스펙 문서 작성 가이드](../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.**

목표는 "이해하기 쉬운 스펙"이다. 파일을 옮기는 것이 목적이 아니라, 독자가 한 주제의 질문에
답을 찾을 수 있게 문서를 다시 쓰고 그 결과를 주제 디렉터리로 배치하는 것이다.

### 왜 필요한가

- 번호가 작성 순서라 주제가 흩어져 있다. relocation만 21·22·23·28·30·31·44·45·52 아홉 문서,
  session은 19·20·48, liveness는 29·49.
- 주제 진입점이 없다. 한 주제를 이해하려면 여러 문서의 앞부분을 각각 읽어야 한다.
- `00`~`33`(계약)과 `40`~`52`(구현)를 번호대로 갈라 두었는데, 가이드 §4.4는 한 문서 안에 계약과
  구현을 함께 두고 방향만 구분하라고 한다.

### 사용자가 확정한 방향

1. **`40`~`52`는 "내부 설계(비규범)"가 아니라 구현 스펙이다.** 4개 언어(dotnet·jvm·cpp·node)가
   이미 이 문서대로 구현되어 있고 앞으로 7개 언어로 늘어난다. 모든 runtime이 따라야 하는
   규범이다. "계약이 우선한다"는 서열 대신 **"두 층이 충돌하면 결함"** 규칙을 쓴다.
   (README ko/en 머리말과 가이드 §4.4에 반영 완료)
2. **다시 쓰되 구현 gap을 만들지 않는다.** 주제마다 재작성 → 규칙 등가성 대조 → 4언어 구현 대조
   → 판정 루프를 돈다.
3. **실제 spec gap은 이번에 고치지 않는다.** [spec-gap 대장](spec-gap.ko.md)에 모으고 재구성이
   끝난 뒤 한 번에 처리한다. 코드는 이 캠페인에서 건드리지 않는다.
4. **ko를 먼저 전부 끝내고, gap 해소까지 마친 뒤에 en을 일괄 작성한다**(사용자 결정
   2026-08-25). gap 해소가 본문 문장을 바꾸므로 en을 먼저 쓰면 두 번 번역하게 된다. 파일
   이동도 en과 함께 맨 마지막에 한 번으로 처리한다 — en 참조 약 100개와 mkdocs en 트리가
   옛 경로를 가리키고 있어 ko만으로는 옮길 수 없다.
5. **site 작업은 맨 마지막에** 한 번에 한다(nav·redirect·검색).
6. **작성은 sonnet 에이전트가 하고 코디네이터는 감독·리뷰만 한다**(사용자 지시 2026-08-25).
7. **최대한 병렬로 진행한다**(사용자 지시). 문서·언어·주제 단위로 동시에 띄운다 — §3.4.

## 2. 결과물의 모양

```
framework/doc/framework/common/spec/server/
  README.ko.md / .en.md          전체 목차 1장 (초안: target-readme.ko.md)
  00-foundation/
    README.ko.md                 주제 진입 1장
    01-public-contract-governance.ko.md   (옛 00)
    02-glossary.ko.md                     (옛 01)
    …
  01-execution/  02-channel-transport/  03-spot-actor/
  04-session/    05-location-relocation/ 06-observability/
  languages/                     그대로 둔다
```

- 디렉터리는 `NN-주제/`, 문서는 **주제 안에서 `01`부터 다시** 번호를 센다. 주제 진입 문서는
  번호 없이 `README`.
- 옛 전역 번호는 파일 이름에 남기지 않는다. 목차에 `(옛 19)`로만 적는다.
- 문서 배치와 병합 계획: [topic-map.ko.md](topic-map.ko.md), 문서별 한 줄 소개와 새 경로:
  [target-readme.ko.md](target-readme.ko.md).

## 3. 작업 방식 — 누가 무엇을 하는가

**사용자 지시(2026-08-25): 스펙 재작성은 sonnet 에이전트가 하고, 코디네이터(메인 세션)는
감독·리뷰만 한다.** 코디네이터가 직접 문서를 쓰지 않는다. 다만 **판정·정정·커밋은 코디네이터
단독**이다 — 에이전트에게 스펙 변경 권한을 주지 않는다.

### 3.1 역할 분담

| 역할 | 하는 일 | 하지 않는 일 |
|---|---|---|
| 코디네이터 | 매핑표 작성, 에이전트 프롬프트 작성, 산출물 리뷰, 판정, 정정 지시, gap 대장 갱신, 커밋 | 스펙 본문 초안 작성 |
| 작성 에이전트 (sonnet) | 지정된 문서 1개 재작성 + 대장 작성 | 다른 파일 수정, 판정, 코드 수정, git 상태 변경 |
| 대조 에이전트 (sonnet, 언어당 1개) | 규칙 R#와 구현 대조, 사실만 보고 | 판정, 스펙·코드 수정 |

작은 정정(리뷰에서 나온 수정 지시)은 **같은 에이전트를 이어서** 처리한다. 새 에이전트를 띄우면
그 문서의 맥락을 다시 읽어야 해서 비용이 크고 일관성이 깨진다.

### 3.2 에이전트 프롬프트에 반드시 넣을 것

파일럿에서 빠뜨려 문제가 생겼던 항목들이다.

1. **읽을 자료를 전부 지정한다** — 가이드 전문, 매핑표, 옛 문서 전부, 인접 주제 문서(중복
   방지), 용어집. "필요하면 읽어라"가 아니라 "다 읽어라".
2. **절 제목을 고정 문자열로 준다.** 여러 에이전트가 병렬로 쓰면서 서로 링크하므로, 제목이
   흔들리면 anchor가 깨진다.
3. **건드릴 파일을 명시한다** — "출력 파일 2개 외에는 어떤 파일도 수정하지 말 것, git 상태를
   바꾸는 명령 금지". 파일럿에서 에이전트가 `.git/index.lock`을 만난 사례가 있다.
4. **원문에 없는 것을 만들지 말 것**(가이드 §2.5). 파일럿에서 실제로 나온 이탈: 번호만 있는
   `reserved command 45`에 이름 지어 붙이기, 원문의 이유 문장을 더 강한 주장으로 바꾸기,
   관계를 설명하려고 방향·개수 문장 새로 만들기.
5. **코드 test가 검색하는 문장은 원문 그대로 유지**하라고 지정한다(§7의 cpp needle).
6. **대장을 함께 내게 한다** — R# → 새 위치, 그리고 발견한 spec gap 후보(고치지 말고 보고만).
7. **보고 형식을 지정한다** — "최종 메시지는 리뷰어를 위한 데이터다. 산문 말고 파일 경로, 줄 수,
   배치하지 못한 R#, gap 후보만."

### 3.3 리뷰 방법 — 대장을 믿지 않는다

**에이전트가 "R21 배치 완료"라고 적어도 본문에 없을 수 있다.** 파일럿에서 대장에 배치됨으로
적힌 규칙 4건이 실제 본문에 없었다(envelope 보존 값 6개, "Current Spot은 authority 검증용" 문장
등). 따라서 리뷰는 다음 순서로 한다.

1. **규칙 실물 확인** — 대장의 각 R행에서 핵심 문구를 뽑아 새 문서에 `grep`한다. 이것이 리뷰의
   중심이고, 나머지는 보조다.
2. **추가된 것 확인** — 옛 문서에 없는 문장이 들어갔는지. 특히 관계·방향·개수를 새로 주장하는
   문장, 지어낸 이름, "유일한 방법이다" 같은 강화된 주장.
3. **구조 확인** — 절 순서가 독자 질문 순서인지, 검증 요구가 인터페이스 관찰인지, white-box
   조건이 검증 절이 아니라 규칙 문단에 있는지, 라벨("결정") 없이 굵은 규칙+이유 불릿인지.
4. **링크·anchor** — 상대 경로, 절 anchor 실재, 내비게이션 줄 상·하단.

정정 지시는 **번호를 붙여 한 번에** 준다(`F1 …`, `M1 …`). 파일럿에서는 한 문서당 6~12건이
나왔고, 에이전트가 번호별로 처리해 한 번에 반영했다.

### 3.4 병렬 실행 — 기본값이 병렬이다

**순차로 할 이유가 없는 일은 전부 동시에 띄운다.** 47개 문서를 하나씩 처리하면 캠페인이 끝나지
않는다. 에이전트 하나를 띄우고 결과를 기다리는 동안 아무것도 하지 않는 상태를 만들지 않는다.

무엇을 동시에 띄우는가:

| 단위 | 병렬 폭 | 조건 |
|---|---|---|
| 주제 안의 문서 재작성 | 문서 수만큼 (session은 2개 동시) | 서로 링크할 절 제목을 프롬프트에 고정 문자열로 박아 둔다 |
| 4언어 구현 대조 | 항상 4개 동시 | 서로 다른 언어 트리만 읽으므로 충돌 없음 |
| **서로 다른 주제** | 2~3개 주제 동시 | 주제끼리 문서가 겹치지 않을 때. `topic-map.ko.md`에서 병합 대상이 다른 주제에 걸치지 않는지 확인 |
| en 번역 | 문서 수만큼 | ko가 확정된 뒤 |
| 매핑표 작성 | 다음 주제의 매핑표를 현재 주제 진행 중에 미리 | 코디네이터가 직접 하거나 에이전트에 위임 |

한 번에 여러 에이전트를 띄울 때는 **한 메시지에 tool call을 모아서** 보낸다. 하나씩 보내면
직렬로 실행된다.

지켜야 할 것:

- **같은 파일을 두 에이전트가 건드리지 않는다.** 프롬프트에 "출력 파일 외 수정 금지"를 넣어
  강제한다. 문서가 서로 링크만 하는 관계면 병렬로 안전하다.
- **하위 에이전트를 띄우지 말고 직접 grep하라**고 지시한다. 파일럿에서 하위 에이전트를 쓴 언어가
  25분 걸렸고 보고서를 한 번 놓쳤다. 병렬은 코디네이터가 관리하고, 에이전트는 자기 일만 한다.
- **코디네이터는 대기하지 않는다.** 에이전트가 도는 동안 도착한 산출물을 리뷰하고, 대장을
  갱신하고, 다음 주제 매핑표를 만든다. 알림이 오면 그때 해당 결과를 처리한다.
- **순차로 묶어야 하는 것만 순차로 한다** — 재작성 → 리뷰 → 대조 → 판정은 한 주제 안에서
  순서가 있다. 그러나 주제 A가 판정 단계일 때 주제 B는 재작성 단계일 수 있다.
- **형식 피드백이 진행 중일 때만 예외**(§3.5). 형식이 바뀌면 병렬로 돌던 작업이 전부
  재작업이 되므로, 그때는 새 에이전트를 띄우지 않는다. 내용 피드백은 해당 주제만 멈춘다.

### 3.5 사용자 리뷰 — 비동기로 받고, 형식 피드백일 때만 멈춘다

문서 리뷰는 사용자가 직접 한다. **리뷰를 기다리며 멈추지 않는다.** 주제가 끝나면 "리뷰 준비됨"
으로 쌓아 두고 다음 주제를 계속 돌린다. 사용자는 편한 때 본다.

피드백은 두 종류이고 영향 범위가 다르다.

| 종류 | 예 | 영향 범위 | 처리 |
|---|---|---|---|
| 내용 | "이 절이 틀렸다", "여기에 그림이 필요하다", "이 표는 무슨 뜻인지 모르겠다" | 그 문서만 | 해당 주제 에이전트에 정정 지시. **다른 주제는 계속 진행** |
| 형식 | "이렇게 쓰지 말자", "이 형식으로 통일하자" | 이미 쓴 문서 전부 | **새 에이전트 투입을 멈춘다.** 가이드에 규칙으로 승격 → 이미 쓴 모든 주제에 일괄 반영 → 재개 |

형식 피드백에서 규칙 승격을 건너뛰면 다음 주제에서 같은 이탈이 반복된다. 파일럿에서 나온
피드백 6건은 전부 형식이었고 모두 가이드 규칙이 되었다(§6). **파일럿의 역할이 그것이었으므로,
이후 주제에서 형식 피드백은 드물어야 정상이다** — 자주 나온다면 가이드가 아직 부족하다는 뜻이다.

리뷰 부담 때문에 완료 보고는 **2~3개 주제씩 묶어서** 한다. 작업은 계속 돌리되 사용자에게는
그룹으로 내보낸다.

**결정이 필요한 gap은 모아서 한 번에** 올린다. 주제마다 묻지 않는다(§4.5, §8).

## 4. 주제 하나를 처리하는 절차

### 4.1 매핑표를 만든다

[topics/04-session/mapping.ko.md](topics/04-session/mapping.ko.md)가 양식이다. 담을 것:

- 대상 문서와 줄 수, 외부 anchor 링크 수, **코드에서 이 문서를 경로로 여는 곳**
- 독자 질문표(가이드 §1) — 주제 README가 답할 질문
- 새 문서 구성: 옛 절 → 새 절 대응, 서술 종류
- 구조 문제 목록(중복 서술, 문단 벽, 소유가 불분명한 규칙 …)
- **규칙 등가성 대장** — 옛 문서의 규칙·수치·상태·오류를 R1, R2 … 로 뽑아 표로. 재작성 뒤
  "새 위치" 열을 채워 누락 0·추가 보장 0을 증명한다.

### 4.2 재작성한다 (sonnet 에이전트)

에이전트에게 반드시 주는 것:

- 가이드 전문, 매핑표, 옛 문서 전부, 인접 주제 문서(중복 방지용), 용어집
- **절 제목을 고정 문자열로 지정**한다. 여러 에이전트가 병렬로 쓰면서 서로 링크해야 하므로.
- 원문에 없는 이름·수치·보장을 만들지 말 것(가이드 §2.5)
- 산출물: 새 문서 + 대장(R# → 새 위치)

### 4.3 리뷰한다 (코디네이터가 직접)

에이전트 대장을 믿지 않는다. **각 규칙을 새 문서에서 grep으로 실물 확인한다.** session
파일럿에서 대장에 "배치됨"으로 적혀 있던 규칙 4건이 본문에 없었다.

같이 볼 것: 원문에 없는 추가 주장, 지어낸 이름, 절 순서, 검증 요구가 인터페이스 관찰인지,
white-box 조건이 규칙 문단에 있는지.

### 4.4 4언어 구현과 대조한다

언어별 에이전트 4개를 병렬로 띄운다. 프롬프트 양식은 session 파일럿 그대로
([topics/04-session/gap-dotnet.md](topics/04-session/gap-dotnet.md) 등이 산출물 예).

- 입력: 새 문서 2~3개, 대장(R#), spec-gap 대장의 G행
- 과제: R행마다 `일치 / 불일치 / 스펙 미정 / 판단 불가` + 파일:줄. G행은 "이 구현이 실제로
  하는 것"
- 금지: 스펙 수정, 코드 수정, 판정

주의 — 이 단계는 오래 걸린다(session 파일럿에서 5분~25분). 에이전트가 하위 에이전트를
띄우면 더 늘어지므로 "하위 에이전트를 띄우지 말고 직접 grep하라"고 지시하는 편이 낫다.

### 4.5 판정한다 (코디네이터 단독)

[topics/04-session/judgment.ko.md](topics/04-session/judgment.ko.md)가 양식이다.

| 결과 | 뜻 | 처리 |
|---|---|---|
| 재작성 오류 | 옛 문서·4언어는 일치하는데 새 문서만 다름 | 새 문서 수정. 주제 안에서 끝냄 |
| 미지정 | 스펙이 값을 안 정해 언어별 발산 | 대장 등록. 4언어 동일하면 자동 판정, 다르면 추천안과 함께 결정 대기 |
| 모순 | 스펙 문서끼리 충돌 | 대장 등록. 소유 문서 기준 판정 |
| 불일치 | 스펙은 명확한데 구현이 다름 | 대장 등록. **코드는 고치지 않는다** — 구현 캠페인으로 |

판정 결과는 [spec-gap 대장](spec-gap.ko.md)에 행으로 남긴다. 대장은 `종류`, `언어별 상태`,
`추천`, `결정` 열을 갖는다. `결정`이 `대기`인 행만 사용자 답이 필요하고, 그 답은 **재구성이 다
끝난 뒤 모아서** 받는다 — 주제 진행을 막지 않는다.

### 4.6 주제를 끝낸 뒤 — 아직 옮기지 않는다

주제 하나가 끝나도 파일을 옮기지 않는다. 이동은 en과 함께 마지막에 한 번에 한다(§5).
주제 완료 시점에 하는 것은 두 가지다.

- 새 ko 문서를 `spec/server/NN-주제/`에 둔다. **아직 아무도 링크하지 않는다** — 옛 문서의
  링크를 바꾸지 않고, mkdocs nav에도 넣지 않는다. 계약 출처는 이동 전까지 옛 문서 하나다.
- 옛 문서 맨 위에 한 줄을 넣어 두 벌 상태를 밝힌다.

```markdown
> 재작성 중 — 이 문서는 `NN-주제/NN-slug.ko.md`로 다시 쓰는 중이며 완료 시 대체된다.
```

## 5. 마지막 단계 — en 작성과 이동

ko 7개 주제가 전부 끝나고 gap 결정까지 반영된 뒤에 시작한다.

1. **ko 확정** — 이 시점의 ko가 번역 기준이다. 이후 ko를 바꾸면 en도 같이 바꿔야 한다.
2. **en 47개 일괄 작성** — 문서 단위로 최대한 병렬. 번역이 아니라 같은 계약을 영어로 쓰는
   작업이며, ko의 절 제목·번호·내비게이션 구조를 그대로 따른다.
3. **이동 한 커밋** — 내용은 손대지 않는다.
   1. `git mv`로 ko/en을 함께 새 경로로
   2. 옛 경로를 참조하는 파일 일괄 치환 — 절 제목이 바뀌었으므로 **anchor 치환표**가 필요하다
      (문서 266개, anchor 링크 505개)
   3. 언어별 guide는 `doc/site/scripts/generate_language_guides.py`가 공통 guide에서 생성한다.
      공통 guide만 고치고 재생성한다
   4. 코드에서 스펙 파일을 경로로 여는 곳을 갱신한다(§7)
   5. `doc/site/mkdocs.yml` nav, redirect 표
4. **검증**(§10) 4종 그린.

**이동 리허설을 미리 해 둔다.** 47×2 파일을 한 번에 옮기는 것이 처음이면 위험하다. ko가
한두 주제 끝난 시점에 그 주제만으로 치환 스크립트·anchor 표·redirect를 한 번 돌려 보고
`git checkout`으로 되돌린다. 커밋하지 않으므로 두 벌 상태가 길어지지 않는다.

## 6. 문서를 쓸 때의 규칙 (가이드에서 자주 놓치는 것)

session 파일럿 리뷰에서 실제로 지적된 것들이다. 모두 가이드에 규칙으로 반영해 두었다.

- **"결정" 같은 라벨을 붙이지 않는다.** 스펙의 문장은 모두 결정이다. 구현이 따라야 하는 구조
  규칙은 **굵은 규칙 문장 + 이유** 불릿으로 쓰고, 표시가 필요한 쪽은 반대 —
  `**언어별 재량**`만 표시한다. (§2.4, §4.4)
- **검증 요구는 인터페이스 관찰로 쓴다.** 첫 문장에서 "무엇만으로 확인하는가"(공개 표면 목록)를
  밝히고, 주제별 굵은 소제목 아래 한 문장 불릿. 표의 열에 문장을 넣지 않고 "확인 방법" 열도
  두지 않는다. 내부 구조로만 확인되는 조건은 그 규칙 문단이 "내부 확인 조건"으로 소유한다.
  형식 예는 가이드 §9.3과 [Core message 스펙 §8](../../../core/doc/spec/core/02-message.ko.md#8-구현-및-contract-test-검증-요구). (§9.3)
- **선언의 옵션 설명은 표가 아니라 인라인 주석으로.** builder·interface method도 포함. 필수·선택과
  기본값을 주석에 넣는다. 옛 문서에 "축 | 의미" 표가 있어도 옮기지 않는다. (§8.3)
- **그림을 둘 자리**: 세 주체 이상의 순서, node·process 경계를 넘는 것, 정상/실패 분기. 물리 층과
  논리 층은 그림을 나누고 서로 어느 층인지 밝힌다. (§7.2)
- **어휘**: "정본" 같은 널리 안 쓰는 한자어 금지 → "이 규칙을 소유하는 문서", "단일 기준".
  (원칙 7.3)
- **내비게이션**: 제목 아래와 본문 끝에 같은 줄 — 주제 목차 · 스펙 목차 · 이전/다음. (§2.2)
- **원문에 없는 것을 만들지 않는다**: 번호만 있는 command에 이름 붙이기, 이유 문장을 더 강한
  주장으로 바꾸기, 관계를 설명하려고 방향·개수 문장 새로 만들기. (§2.5)

## 7. 건드리면 깨지는 것들

| 대상 | 내용 |
|---|---|
| 외부 참조 | 옛 `spec/server/NN-slug` 경로 참조는 저장소 전체에서 약 **7,500건**이다(markdown 외에 `.json`·`.sh`·`.ts`·`.java`·`.cpp`·`.cs`·`.py`·`.yml`·`.hpp` 포함). 이 중 anchor를 지정한 것이 **5,409건** — 그대로 사는 것 2,027, 절 제목이 바뀐 것 245, 1:N 문서로 가는 것 3,137. 실측과 표는 [anchor-map.ko.md](anchor-map.ko.md). 예전에 적혀 있던 "266개 파일·505개 링크"는 markdown만 센 값이라 실제와 다르다 |
| cpp contract test | `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`가 `14-actor-model.ko.md`, `06-framework-api.ko.md`, `20-session-actor-dispatch.ko.md`를 **경로로 열어 문장을 needle로 검색**한다. 해당 문서를 옮기면 경로와 needle을 함께 갱신해야 한다 |
| 소스 주석 | node `channel-socket-registry.ts`, java `ZLinkChannelRuntime.java`가 `08-channel-messaging.ko.md` 경로를 주석에 담고 있다 |
| testdata·스크립트 | `testdata/location/redis/*.json` 5개, `scripts/verify-framework-*.sh` 2개 |
| 파일 잠금 | `spec/` 트리는 원래 `r--r--r--`로 잠겨 있다. 사용자 승인으로 `chmod -R u+w framework/doc/framework/common/spec` 해제함. **캠페인 종료 시 다시 잠글 것.** 작업 중 다시 잠기는 일이 있었으므로 편집 실패 시 권한부터 확인한다 |

## 8. 진행 상태

| 단계 | 상태 |
|---|---|
| 선행: 가이드 §4.4·README ko/en 두 층 규칙 | **완료** |
| 선행: 가이드 보강(§2.2 번호·내비, §2.4, §2.5, §3.3, §7.2, §8.3, §9.3, §11 점검표), 원칙 7.3 | **완료** |
| 전체 목차 초안 `target-readme.ko.md` | **완료** (47개 행, 새 경로·번호 반영) |
| 04-session 재작성 (ko) | **완료** — `README.ko.md`, `01-stream-session.ko.md`, `02-session-actor-binding.ko.md`. 사용자 리뷰 반영 완료 |
| 04-session 4언어 대조 | **완료** — 4개 보고서 |
| 04-session 판정 | **완료** — 재작성 오류 0건. 자동 판정 4건 문서 반영 완료(J1·J2·J4·J13). 결정 대기 8행 |
| 04-session en 작성 | 보류 — 마지막 단계에서 47개 일괄(§5) |
| 04-session 이동·링크 치환·nav·cpp test | 보류 — 마지막 단계에서 한 번에(§5) |
| 04-session 옛 문서에 "재작성 중" 표시 | **완료** (19·20·48) |
| **7개 주제 ko 재작성** | **완료** — 48문서 23,116줄. 링크 검사 기준선(기존 실패 4건) 유지, 식별자 보존 누락 0, 용어집 anchor 256개 전량 보존 |
| 리뷰에서 잡은 규칙 누락 | 4건 복원(type 중복 계산, crash 복구 2건, `DeadlineExceeded` 의미) |
| 주제별 4언어 대조 | **완료** — 2차 대조 결과 dotnet 1건, jvm 2건, cpp 0건, node 0건. **재작성이 만든 불일치 0건** |
| gap 판정·반영 | **완료** — 해소 가능한 8건 반영(G2b·G7·G19·G20·G21·G23·G18·옛 출처 잔재) |
| 사용자 리뷰 반영(진행 중) | 형식 피드백마다 가이드 규칙 승격 + 전 문서 sweep. 승격한 규칙: §2.5 분할, §3.5 `exact`, §8.3 인라인 주석, §9.3 검증 요구, **§5.7 구체 예·"언제 쓰는가" 열**, **§7.1 결과 열** |
| gap 결정 D1~D10 | **완료** — 문서 수정 6건(D1·D2·D4·D5·D6·D7·D8), 구현 부채 등록 3건(D3·D10·G15 등), relocation 주제 이월 1건(D9). 대장에 `대기` 행 0 |
| 구현 부채 목록 | **완료** — [spec-gap.ko.md](spec-gap.ko.md) 말미 |
| 구현 부채 해소 | **거의 완료** — 배치 1~4 완료(§9.5). 사용자 결정으로 이 캠페인에서 코드까지 고친다 |
| 계약 판정 R1 | **완료** — seal identity에서 coordinator 제거. node·jvm 두 트리에 적용 |
| 전체 게이트 | **완료** — dotnet 1,879/1,879 · jvm BUILD SUCCESSFUL · cpp 44/44 · node 계약 전량 · 언어간 통신 passed |
| 7샘플 × 3트리 | **완료** — node 7/7, cpp 7/7(일괄 실패 2건은 단독 재현 4/4 통과), dotnet 6/7. dotnet ZoneWorld는 **HEAD에서도 실패**(HEAD 9건 vs 현재 1건)라 이번 작업의 회귀가 아니다 |
| 언어별 guide 재생성 | **완료** — `generate_language_guides.py`, 4개 갱신 |
| 최상위 README | **완료** — `exact` 제거, `archive/` 안내 추가 |
| `languages/` 트리 정리 | **완료** — 이동 안내 껍데기 18파일 삭제, 옛 평면 경로 835건 치환, anchor 10종 정정 |
| session 넘버링 | **완료** — `spec/server/session/` → `04-session/`, 참조 394건 |
| Spot timer 주제 이동 | **완료** — `01-execution/04-spot-timer` → `03-spot-actor/10-spot-timer`, execution 뒤 문서 2개 당김 |
| en 동기화 부채 정리 | **완료** — [en-sync-debt.md](en-sync-debt.md). 구조 갈라진 문서 1건(`04-interaction-model` §7)은 영어를 새로 써서 해소. **48쌍 전부 절 구조 일치.** 본문 내용 미반영은 2건 남음(wire-protocol byte 맵, gap 결정 7건) |
| en 47개 일괄 작성 | ko 확정 후(§5) |
| 이동 anchor 치환표 | **완료** — [anchor-map.ko.md](anchor-map.ko.md), 생성기 `build-anchor-map.py`. anchor 그대로 2,027건 / 자동 매핑 64건 / 손으로 정할 것 55건 / 1:N 라우팅 3,137건 |
| 이동 리허설 → 이동 한 커밋 | en 작성 후(§5) |
| site — guide 재생성·mkdocs 확인 | **완료** — 언어별 guide 4개 재생성, 빌드 경고 중 캠페인이 만든 3건(en→ko 링크) 수정. 나머지 128건은 site 발행 범위 문제로 캠페인 밖(§10) |
| redirect 표 | **불필요** — 옛 문서를 지우지 않고 `archive/`에 두었고 `mkdocs-redirects`도 설치돼 있지 않다. 옛 URL은 `archive/` 경로로 살아 있다 |
| `spec/` 트리 재잠금 | **보류** — 사용자 리뷰가 열려 있는 동안 잠그면 그 리뷰 반영을 막는다. 리뷰가 닫힌 뒤 `chmod -R a-w framework/doc/framework/common/spec` |

## 9. 다음에 할 일

ko 재작성·4언어 대조·gap 판정(D1~D10)·구현 부채 배치 1~4가 모두 끝났다. 사용자 리뷰는
계속 열려 있고, 구현과 병행한다(§9.5).

1. **사용자 리뷰를 계속 받는다.** 형식 피드백이 오면 (a) 지적한 자리를 고치고 (b) 같은 규칙을
   가이드에 승격하고 (c) 이미 쓴 전 문서로 sweep을 돈다(§3.5). 이 단계는 사용자가 리뷰를
   닫을 때까지 열려 있으므로 "전 문서 sweep 완료"를 완료 게이트로 삼지 않는다.
2. **남은 게이트 실패를 전부 닫는다**(§10.1). 현재 node dispatch 오류 telemetry 16건.
3. **en 동기화를 끝낸다** — [en-sync-debt.md](en-sync-debt.md). 자동 검사는 en이 낡아도
   통과하므로 이 문서가 유일한 기록이다.
4. **`languages/` 트리의 어휘·형식 정리** — `exact` 191곳, `정본` 5파일, 내비게이션 줄 없는
   문서. 공통 스펙 7주제에 적용한 규칙을 같은 수준으로 적용한다.
5. 이동 리허설 → 이동 한 커밋(§5) → site·최상위 README 축소 → `spec/` 트리 재잠금(§7).

### 에이전트를 띄우기 전에

§7의 §7 오복구 사고 이후 절차를 바꿨다.

- **스펙 트리 스냅샷을 먼저 뜬다** — `cp -a framework/doc/framework/common/spec/server <스크래치>`.
  새 트리는 아직 untracked라 `git diff`가 변경을 보여 주지 못한다. 스냅샷이 있어야 사고가
  복원이 아니라 diff가 된다.
- **스캔 단계는 편집 도구가 없는 에이전트로 띄운다.** "읽기 전용" 지시는 지켜지지 않았다.
- **편집 단계는 파일 목록을 명시해 겹치지 않게 나눈다.** 같은 파일을 둘에게 주지 않는다.
- **코디네이터가 지금 편집 중인 파일은 대상에서 뺀다.** 에이전트가 그 변경을 날조로 보고
  되돌린 사고가 실제로 있었다.

## 9.5 구현 부채 해소 — 문서 리뷰와 병행한다

사용자 결정(2026-08-25)으로 **문서 리뷰를 닫기 전에 구현 부채를 먼저 시작한다.** 문서 피드백
반영과 구현이 같은 기간에 돈다. 근거는 스펙이 이미 확정됐고(D1~D10 판정 완료), 부채 항목마다
**이미 맞게 구현한 언어가 있어** 새로 설계할 것이 없다는 점이다.

### 방식

| 항목 | 내용 |
|---|---|
| 구현 | codex terra high — `codex exec -C <repo> -m gpt-5.6-terra -c model_reasoning_effort=high -s danger-full-access --skip-git-repo-check` |
| 병렬 | 언어 트리별로 동시에. 서로 다른 트리는 파일이 겹치지 않는다 |
| 리뷰 | 코디네이터가 diff를 직접 읽고 스펙 정합을 확인한다. 에이전트 보고를 그대로 믿지 않는다 |
| 커밋 | 코디네이터 단독. 에이전트는 파일 수정까지만 한다 |
| 기준 언어 | 항목마다 이미 스펙대로 동작하는 언어를 기준으로 삼는다([정본-우선 포팅 정책](../../../CLAUDE.md) 취지) |

에이전트 프롬프트에 반드시 넣는 것.

- **스펙 문서 수정 금지.** 코드를 스펙에 맞추는 작업이지 그 반대가 아니다.
- **`git commit`·`add`·`stash`·`reset` 금지.**
- **새 스펙 경로만 읽을 것.** 옛 평면 문서(`spec/server/NN-*.ko.md`)가 아직 남아 있어 그쪽을
  읽으면 이전 판 문장을 본다. 이동 커밋 전까지 이 함정이 계속 있다.
- 변경 파일 목록과 각 변경의 스펙 근거(파일·절)를 요약할 것.

### 부채 목록

[spec-gap.ko.md](spec-gap.ko.md) 말미의 표가 원본이다. 12행이며 언어별로 dotnet 5·jvm 4·
node 4·cpp 4건이다. 대부분 상수·오류 코드·로그 문자열이고, 새로 만드는 것은 D4의 host 단위
검사와 D5의 public option 둘뿐이다.

### 배치

| 배치 | 트리 | 항목 | 상태 |
|---|---|---|---|
| 1 | dotnet | D6 `NotFound`, D10 `EMSGSIZE` 로그 | **완료** — 단위 테스트 6개 통과 |
| 1 | cpp | D7 재시도 제거, D5 5초 하드코딩 → `set_session_replacement_callback_timeout`(기본 30,000 ms) | **완료** — 게이트 44/44 |
| 1 | jvm | D6 `NOT_FOUND`, G22 `server_shutdown`·idle·heartbeat counter 계상 | **완료** — 지정 테스트 통과 |
| 1 | node | D6 `NotFound`, D7 backoff 루프 제거 | **완료** — m6b 104/104, m6c 112/112 |
| 1 | (코디네이터) | cpp layout contract needle 2개를 `exact` 제거 후 문구로 갱신 | **완료** — 재실행 통과 |
| 2 | cpp | D8 — **구현 수정 불필요**로 판명. `application_job_queue.hpp`가 이미 `receive_flow_sockets` 전체에 fan-out 한다 | **완료** — 회귀 테스트만 추가 |
| 2 | dotnet·jvm·node | D4 host 단위 검사 + D5 `SessionReplacementCallbackTimeout`(기본 30,000 ms) | **완료** — 3트리 모두 게이트 통과 |
| 3 | dotnet·cpp·node | D3 late 44 Warning. dotnet은 예외 throw도 제거(J12) | 진행 중 |
| 3 | cpp | D1 eager coroutine 첫 turn 정정 | 진행 중 |
| 4 | dotnet·jvm·node | G15 lane 정책 타입 | 대기 |
| — | — | D9는 relocation 주제에서 사실 확인 후 판정. 부채 아님 | 이월 |

### 사용자 리뷰로 바뀐 문서 배치

리뷰 중에 주제 배치가 두 번 바뀌었다. 둘 다 이동 커밋 전에 반영했다 — 이동 커밋에서 하면
경로 변경이 두 번 겹친다.

| 바뀐 것 | 이유 |
|---|---|
| `spec/server/session/` → `04-session/` | 주제 디렉터리 7개 중 이것만 번호가 없었다. 참조 394건(114파일)과 `mkdocs.yml` nav 3줄을 함께 치환했다 |
| `01-execution/04-spot-timer` → `03-spot-actor/10-spot-timer` | timer는 Spot이 등록하는 기능이므로 Spot·Actor 주제가 소유하는 것이 맞다. 뒤 문서 둘을 `04`·`05`로 당기고, 양쪽 주제 README의 목차·질문 표·읽는 순서·수치 표를 옮겼다 |

**주제 사이로 문서를 옮길 때 손대야 하는 것** — 파일 6개(ko/en), 저장소 전체 링크, 옮긴
문서 안의 옛 형제 링크(상대 경로가 깨진다), 앞뒤 문서의 내비게이션 줄 4개(본문 머리와 끝에
각각 있어 파일당 2곳), 두 주제 README의 목차 표·질문 표·읽는 순서·수치 요약표,
`mkdocs.yml` nav, `move-plan.ko.md`.

### 배치를 돌면서 드러난 것 — 1차 대조가 두 번 틀렸다

캠페인 §3.3의 "대장을 믿지 않는다"가 실제로 두 번 값을 했다.

- **G7**(abort 시 seal 해제와 held 제출 순서) — 1차 판정의 "52만 이탈"이 틀렸다. 28도 같은
  순서를 적고 있었다.
- **G12/D8**(admission 실패 시 receive 중단 범위) — 1차 대조의 "cpp만 연결별"이 오관찰이었다.
  `application_job_queue.hpp:782`가 상태 전이 때 `receive_flow_sockets` 전체를 담는다.
  4언어가 모두 같게 동작하고 있었고, 부족한 것은 스펙 문장의 범위 서술뿐이었다.

**대장의 언어별 상태 열은 근거가 아니라 단서다.** 부채를 코드 작업으로 넘기기 전에 해당
코드를 직접 열어 확인한다. 에이전트가 "이미 맞음"이라고 보고하면 그 말도 코드로 확인한다.

### 게이트

트리마다 다르다([환경 메모](../../../AGENTS.md) 참조).

| 트리 | 명령 |
|---|---|
| cpp | `ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'`. 빌드 직후 exit 86·SIGABRT(134)는 일시 현상이므로 한 번 재실행한다 |
| node | `npx tsc -b tsconfig.build.json` 먼저(테스트가 `dist/`에서 import한다) |
| dotnet | UnitTests csproj는 `EnableDefaultCompileItems=false`다. 새 테스트 파일은 `<Compile Include>`에 넣어야 인식된다 |

## 10. 검증

```bash
python3 doc/site/scripts/check_doc_links.py framework   # 상대 링크·anchor
python3 doc/site/scripts/check_doc_tabs.py framework    # 언어 탭·스니펫
cd doc/site && mkdocs build --strict                    # nav·로케일·발행 범위
git diff --check                                        # 공백
```

**`mkdocs build --strict`는 현재 통과하지 않으며, 이 캠페인이 만든 상태가 아니다.**
경고 128건은 전부 site docs 루트 밖 파일을 가리키는 링크다 — 저장소 루트 `README`,
`doc/principal`, `bindings/doc`, `doc/perf`, `bindings/*/src`. 저장소에는 실재하므로
`check_doc_links.py`는 통과하고, site에 실리지 않으므로 빌드는 경고한다. 없애려면 site
발행 범위를 바꾸거나 그 링크들을 외부 URL로 바꿔야 하며, 이는 별개 작업이다.

그러므로 이 캠페인의 site 게이트는 **"캠페인이 새 경고를 만들지 않았다"**이다. 확인 방법 —
빌드 경고 중 출처가 `common/spec/server/`인 것을 뽑아 site 루트 밖 링크가 아닌 것이 있는지
본다. 실제로 이 방법이 en 문서 3곳이 `.ko.md`를 가리키던 결함을 잡았다.

```bash
cd doc/site && python3 -m mkdocs build --strict 2>&1 \
  | grep "^WARNING" | grep "Doc file 'common/spec/server/"
```

기준선: `check_doc_links.py`는 현재 **기존 실패 4건**이 있다 —
`sample/zoneworld/README.{ko,en}.md`의 `15-spot-actor` anchor. 이 캠페인과 무관하며, 그 주제를
재작성할 때 함께 해소된다. 그 외 실패가 나오면 이번 작업이 만든 것이다.

### `mkdocs build --strict`가 잡는 것 — 링크 검사가 못 잡는 결함

`check_doc_links.py`는 **파일이 저장소에 실재하는지**만 본다. site 빌드는 그 위에 두 가지를
더 잡는다.

| 결함 | 왜 링크 검사가 놓치는가 |
|---|---|
| en 문서가 `.ko.md`를 링크 | 파일은 실재한다. site는 로케일별로 문서 집합이 갈리므로 영어 빌드에서 그 대상이 없다 |
| site docs 루트 밖을 링크 | 저장소에는 있지만 `doc/site/docs/`에 심볼릭되지 않은 트리(`doc/principal`, `bindings/doc`, `framework/runtime`)는 site에 실리지 않는다 |

실제로 en 동기화 뒤 en 문서 3곳이 `.ko.md`를 가리키고 있었고 링크 검사는 통과했다.
**문서를 옮기거나 en을 새로 쓴 뒤에는 반드시 site 빌드까지 돌린다.**

cpp layout contract test는 스펙 문서를 읽으므로 문서를 옮긴 뒤 함께 실행해야 한다.

```bash
ctest --test-dir framework/languages/cpp/build -R layout_contract --output-on-failure
```

이 테스트는 **문서의 한국어 문장을 그대로 grep한다.** 문구를 다듬으면 needle이 깨진다.
실제로 `exact` 제거 sweep이 needle 2개를 깨뜨렸고(`"Actor destroy는 exact \`ActorRef\`를
받는다"`, `"| exact ActorRef destroy |"`) 문구를 맞춰 해소했다. 문장을 고칠 때마다
[needle-repair.md](needle-repair.md)를 함께 본다.

용어 첫 등장 검사도 있다.

```bash
python3 doc/plan/spec-server-reorg/termscan.py
```

현재 기존 2건(`04-spot-timer.ko.md`의 `Owner`,
`05-application-job-queue-and-backpressure.ko.md`의 `Authority`)만 남아 있다. 그 외가
나오면 이번 작업이 만든 것이다.

ko/en 절 구조가 갈라졌는지도 확인한다 — 자동 검사가 잡지 못하는 항목이다.
명령은 [en-sync-debt.md](en-sync-debt.md) §3에 있다.

## 10.1 최종 게이트 — 구현 부채를 끝낸 뒤

사용자 요구(2026-08-25): **구현 부채 작업이 끝나면 4언어 전체 단위 테스트와 7샘플이 실패
없이 도는지 확인한다.** 배치별 게이트(해당 테스트만 돌림)로는 다른 곳이 깨졌는지 알 수 없다.

### 단위 테스트

| 트리 | 명령 |
|---|---|
| cpp | `ctest --test-dir framework/languages/cpp/build -L 'framework-(unit\|contract)' -LE 'e2e\|sample\|perf'` |
| dotnet | `dotnet test framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj` |
| jvm | `./gradlew --no-daemon :zlink-framework-core:test` (`framework/languages/java`에서) |
| node | `npx tsc -b tsconfig.build.json` 후 `npm run verify:m6a-runtime`·`verify:m6b-runtime`·`verify:m6c-runtime` |

### 언어간 통신 테스트

한 언어의 자체 테스트가 통과하는 것은 언어간 상호운용의 증거가 아니다. 실제 wire로 맞대어
보는 것은 cross-language smoke다.

```bash
cd framework/languages/cpp/cross-language
ZLINK_CPP_BUILD_DIR=../build ./run_cross_language_smoke.sh
```

C++ 패키지를 .NET·Node의 실제 패키지와 맞대어 producer/consumer 방향을 각각 한 스테이지로
나눠 검증한다 — messaging(request/reply·one-way 양방향), flow-wire(fanout
`<topic>:<value>`), codec(STREAM 프레임 + JSON / LZ4 압축 payload). 실행 로그를 남기려면
`ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`.

`framework/languages/{node,dotnet,java}/cross-language/`에도 각 언어의 peer host와 러너가
있다.

### e2e는 이 캠페인의 검증 대상이 아니다

e2e는 아직 완결되지 않은 작업이므로 **e2e 실행은 게이트에 넣지 않는다**(2026-08-25 사용자
지시). 다만 `test/contract/`의 e2e 관련 **계약 테스트**는 다르다 — 이들은 e2e를 실행하지
않고 문서의 시나리오 ID·제목과 구현 파일 목록이 맞는지만 대조하므로 문서 게이트에 속한다.

| 계약 테스트 | 무엇을 대조하는가 |
|---|---|
| `e2e-scenario-header-gate` | 문서의 `#### <ID> <제목>`과 시나리오 파일 첫 줄 주석이 글자까지 같은가, 문서의 모든 ID가 구현돼 있는가 |
| `e2e-config-9-10-scenario-layout-gate` | Config 9·10의 문서 ID 수와 시나리오 파일 수가 같은가 |

### 7샘플

샘플은 Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, TicTacToe, ZoneWorld다.
`dotnet`·`cpp`·`node` 트리에 각각 7개의 `samples/<이름>/run_sample.sh`가 있고 묶어 도는
`samples/run_samples.sh`가 있다. Java 샘플은 `framework/languages/java/samples/`의 gradle
프로젝트다.

```bash
bash framework/languages/dotnet/samples/run_samples.sh
bash framework/languages/cpp/samples/run_samples.sh
bash framework/languages/node/samples/run_samples.sh
```

**샘플 러너에는 자체 timeout이 없어 무한히 멈출 수 있다.** 그리고 `run_samples.sh`로
한 번에 돌리면 **하나가 멈추거나 죽는 순간 나머지를 못 본다** — 실제로 cpp는 TicTacToe에서
CPU 0으로 15분 행에 걸렸고 dotnet은 ShoppingMall native abort에서 러너가 종료됐다.

샘플별로 감싸서 돌린다.

```bash
for d in framework/languages/<tree>/samples/*/; do
  name=$(basename "$d")
  [ -f "$d/run_sample.sh" ] || continue
  if timeout 420 bash "$d/run_sample.sh" > "/tmp/sample-$name.log" 2>&1
  then echo "PASS $name"; else echo "FAIL $name rc=$?"; fi
done
```

### dotnet ZoneWorld — 남은 조사

7샘플 중 유일하게 통과하지 못한다. **이 캠페인의 회귀가 아니다** — dotnet 소스를 HEAD로
되돌려 돌리면 9개 시나리오가 실패하고(ZW-D1·E1·E2·E3·E4·E6·F1·F3·F4), 현재 상태에서는
1곳만 실패한다. node ZoneWorld(167초)와 cpp ZoneWorld(206초)는 같은 시나리오 정본으로
통과한다.

지금까지 좁힌 것.

| 사실 | 근거 |
|---|---|
| 실패 지점은 노드 교체 | `!! zone-node-replacement never logged 'topology=ready'` |
| 교체 노드는 zone 2개를 반드시 확보해야 한다 | `run_sample.sh`의 `zone-node-replacement` config는 `allowEmptyZoneSet`을 켜지 않는다. 켜는 것은 `zone-node-crash-replacement`뿐이다 |
| 확보 재시도 예산은 30초 | `BotSpawner.cs`의 `StartupRetryAttempts=120` × `StartupRetryDelay=250ms`. 넘으면 예외를 던져 프로세스가 죽는다 |
| 죽은 owner의 zone은 lease가 만료돼야 가져온다 | ZoneNode는 lease 설정을 하지 않아 framework 기본값(TTL 15초, 갱신 5초)을 쓴다. Ops만 TTL 3초로 줄인다 |
| 실패 시 관찰되는 것 | `live_row_filter rejected=owner_not_live`가 8개 owner에 반복되고 `actor_join_retry attempt=58`까지 간다 |

#### 원인 — dotnet만 브라우저 단계를 돌렸다

`run_sample.sh`의 playwright·browser 참조 수를 세면 **dotnet 31곳, node 0곳, cpp 0곳**이다.
dotnet ZoneWorld만 `shared_sample/zoneworld/client`의 Playwright UI 테스트를 추가로 돌린다.
같은 시나리오 정본을 검증하는 것이 아니라 **dotnet만 검증 범위가 넓었다.**

3회 실행에서 실패 지점이 Playwright → 노드 교체 → Playwright로 갈린 것도 이걸로 설명된다.

`--no-browser-smoke` 옵션은 원래 있었지만 **기본값이 켜짐(`BROWSER_SMOKE=1`)**이었다.
사용자 판단(2026-08-25)으로 **기본값을 끔으로 바꿨다** — 샘플 검증은 client connector
경로만 돌리고, 브라우저 smoke가 필요하면 `--browser-smoke`로 따로 켠다.

#### 언어별 설정 차이

같은 시나리오 정본인데 node는 통과하고 dotnet은 못 하므로 설정을 대조했다.

| 언어 | ZoneNode의 owner lease |
|---|---|
| node | `ownerLeaseTtlMs(3_000)`, 갱신 1초, fencing margin 500 ms — **Ops·Gateway·ZoneNode가 `Server/Configuration/location-store.ts` 하나를 공유** |
| dotnet | **설정하지 않는다** → framework 기본값 TTL 15초, 갱신 5초. Ops만 `Server/Ops/Program.cs`에서 TTL 3초로 줄인다 |
| cpp | ZoneWorld 샘플에 lease 설정이 없다 |

**dotnet이 ZoneNode에 lease를 설정하지 않는 것은 실수가 아니라 의도다.** Ops 코드의 주석이
밝힌다 — "Zone nodes keep the documented 30-second defaults, so crash scenarios still exercise
real lease expiry (§4.2 and §8.1)". crash 시나리오가 실제 lease 만료를 겪게 하려는 것이다.

다만 두 가지가 어긋난다.

- 주석은 "30-second defaults"라 하지만 framework 기본값은 **`OwnerLeaseTtl` 15초**다.
- `BotSpawner`의 재시도 예산 30초가 그 주석의 30초를 전제로 맞춰졌다면, 실제 15초 TTL +
  fencing margin과의 관계를 다시 봐야 한다.

즉 **lease 만료 대기와 재시도 예산이 빠듯한 구조**이고, WSL처럼 느린 환경에서 30초를 넘긴다.
다음 단계는 교체 노드가 던진 예외 원문을 확보해 "예산 부족"인지 "영영 claim 불가"인지
가르는 것이다. 증거는 `ZLINK_SAMPLE_EVIDENCE_DIR`를 주고 실행해야 남는다.

```bash
ZLINK_SAMPLE_EVIDENCE_DIR=<디렉터리> bash framework/languages/dotnet/samples/ZoneWorld/run_sample.sh
```

### 샘플 실행에서 배운 것

- **일괄 실행의 실패를 그대로 믿지 않는다.** cpp DeliveryDispatch(81초 connector timeout)와
  TicTacToe(teardown rc=1)는 단독 재현에서 각각 13초·15초에 통과했다. dotnet ShoppingMall은
  일괄에서 native abort(`fast_mutex.hpp:76`)로 죽었지만 단독에서 15초에 통과했다.
  **실행 시간 차이가 판정 근거다** — 81초 vs 13초면 로직이 아니라 자원 경합이다.
- **HEAD와 대조해야 회귀인지 알 수 있다.** dotnet ZoneWorld는 단독에서도 실패했지만, dotnet
  소스를 HEAD로 되돌려 돌리니 **더 많이**(9개 시나리오) 실패했다. 이번 작업의 회귀가 아니다.
- **트리를 동시에 돌리지 않는다.** cpp와 dotnet을 함께 돌린 회차에서만 실패가 나왔다.

### 게이트 실행 결과 (2026-08-25)

| 게이트 | 결과 |
|---|---|
| 언어간 통신 smoke | **passed** — spot-route 6조합, relocation 1, User-Spot Join 12조합(그중 4건은 canonical `actorJoin`(28) 경로) |
| cpp 단위·계약 44개 | **44/44** |
| cpp 전체 빌드 | **통과** |
| jvm `:zlink-framework-core:test` | **BUILD SUCCESSFUL** |
| dotnet 단위 1,879개 | **1,879 / 1,879** |
| node 문서·계약 게이트 | **통과** — `e2e-scenario-header-gate`, `e2e-config-9-10-scenario-layout-gate`, `contract-surface`, `sample-spot-lifecycle` |
| 문서 검사 5종 | **통과** — 링크·탭·instance-spot·submit-api·cpp layout contract |

이 과정에서 닫은 실패와 그 원인.

| 실패 | 원인 | 처리 |
|---|---|---|
| cpp 전체 빌드 깨짐 | 배치 1이 `stream_host_service_t` 생성자에 필수 인자를 추가하고 테스트 호출부 6곳을 안 고쳤다. 에이전트는 "이번 변경과 무관"이라고 보고했다 | 호출부에 명시값을 넣었다. 기본값을 숨겨 넣지 않았다 |
| cpp `layout_contract` | 문서에서 `exact`를 걷어내며 needle 2개가 옛 문구로 남았고, `04-session` rename으로 경로도 어긋났다 | needle 문구와 경로를 현재 문서에 맞췄다 |
| dotnet `FrameworkHostStartupFailureDisposesCreatedStreamRuntime` | D4 검사가 **정상 동작한 결과**. 테스트가 같은 session type을 두 node에 등록해 두어, 의도한 endpoint 실패 지점에 도달하기 전에 등록 검증이 거부했다 | 두 번째 node에 별도 session type을 주고 근거를 주석에 달았다 |
| node `contract-surface` | 배치 2가 추가한 `setSessionReplacementCallbackTimeout`이 public contract snapshot에 없었다 | snapshot에 등록했다. 공개 표면이 늘면 snapshot도 함께 간다 |
| node `sample-spot-lifecycle` | 테스트가 grace deadline 뒤 `close()` 1회를 기대했으나, 샘플은 스펙 근거를 달고 닫지 않고 있었다 | 스펙이 샘플 쪽이 맞다 — [Spot 모델 §5](../../../framework/doc/framework/common/spec/server/03-spot-actor/01-spot-model.ko.md) "Current Actor membership이 하나라도 남아 있으면 public close는 `false`로 끝나며". 기대값을 정정했다 |
| node `e2e-scenario-header-gate` | 시나리오 파일 첫 줄 주석이 문서 제목과 6건 어긋났고, 문서에 있는 `MON-A7`·`ST-C4`가 구현돼 있지 않았다 | 제목 6건을 문서 기준으로 맞추고 시나리오 2개를 구현했다 |
| node command 42/44 seal 대조 | 구현이 coordinator 5필드를 키에 넣었다 | 판정 R1 — 뺐다([spec-gap.ko.md](spec-gap.ko.md) R1) |

### 실패가 하나라도 남으면 끝난 것이 아니다

**전체 단위 테스트와 7샘플이 전부 통과할 때까지 고친다.** 원인이 이번 작업이든 트리에 남아
있던 다른 미완 작업이든 마찬가지다. "이번 변경과 무관하다"는 판정으로 실패를 남겨 두고
끝내지 않는다(2026-08-25 사용자 지시).

### 실패가 나오면 전부 새 실패다

**HEAD는 실패 없이 확인하고 push한 상태다.** 그러므로 실패가 하나라도 나오면 기존 실패로
넘기지 않고 원인을 찾는다. "알려진 flake"라는 이유로 넘어가지 않는다.

원인은 셋 중 하나다.

1. **이번 구현 부채 작업이 만든 것** — 해당 트리의 오늘 변경을 되돌려 확인한다.
2. **작업 트리의 다른 커밋되지 않은 변경이 만든 것** — 이 저장소에는 다른 캠페인의 미완
   작업이 커밋되지 않은 채 있다. 오늘 node `test/contract/actor-manager.test.js`의 4건이
   여기 해당한다. 오늘 고친 소스를 HEAD로 되돌려도 남았고, 원인은 canonical actorJoin
   authority fence 쪽 미완 작업이다. **넘길 항목이 아니라 원인을 찾아 닫을 항목이다.**
3. **환경 문제** — C++ 테스트 바이너리가 빌드 직후 exit 86 또는 SIGABRT(134)로 죽는 것은
   재링크 중 발생하는 것이라 한 번 재실행해 판단한다. 재실행해도 실패하면 1이나 2다.

되돌려 확인하는 방법: 문제의 파일을 스크래치에 복사해 두고 `git checkout -- <파일>`로
HEAD 상태로 만든 뒤 같은 테스트를 돌린다. 확인이 끝나면 복사본을 되돌려 놓는다.
`git stash`는 이 저장소에서 실패한 적이 있어 쓰지 않는다.

## 11. 이 디렉터리의 구조

캠페인 전체에 걸치는 문서는 루트에, 주제별 작업 산출물은 `topics/NN-주제/`에 둔다. 주제
하나가 8개 안팎의 문서를 만들므로 루트에 쌓지 않는다.

```
doc/plan/spec-server-reorg/
  README.ko.md          이 문서 — 작업 지침이자 캠페인 진입점
  topic-map.ko.md       47개 문서를 7개 주제로 나눈 표, 병합 계획
  target-readme.ko.md   재구성 완료 시점의 spec/server/README.ko.md 초안
  spec-gap.ko.md        spec gap 대장 — D1~D10 결정 목록, 23개 G행, 구현 부채 표
  move-plan.ko.md       옛 경로 → 새 경로 이동 계획 (1:1 39건, 1:N 8건)
  anchor-map.ko.md      이동 커밋용 anchor 치환표 — 실측 규모와 자동/수동 매핑
  build-anchor-map.py     └ 위 표를 다시 만드는 스크립트. 문서를 고치면 재실행한다
  en-sync-debt.md       ko 리뷰 반영이 en에 안 들어간 곳. 자동 검사가 못 잡는다
  needle-repair.md      스펙 문장을 needle로 grep하는 스크립트·테스트의 문구 갱신 목록
  termscan.py           용어 첫 등장 시 풀어쓰기 여부를 검사한다
  gap-round2-{dotnet,jvm,cpp,node}.md   재작성이 gap을 만들지 않았는지 2차 대조 보고서
  topics/
    04-session/         주제별 작업 산출물 (아래가 표준 구성)
      mapping.ko.md                    매핑표 — 새 문서 구성, 규칙 등가성 대장(R#)
      ledger-01-<slug>.md              문서별 R# → 새 위치 대장
      ledger-02-<slug>.md
      gap-dotnet.md / gap-jvm.md / gap-cpp.md / gap-node.md   4언어 구현 대조 보고서
      judgment.ko.md                   판정표 — 재작성 오류·미지정·모순·불일치 분류
```

주제 디렉터리는 7개가 모두 있다 — `00-foundation`, `01-execution`, `02-channel-transport`,
`03-spot-actor`, `04-session`, `05-location-relocation`, `06-observability`. 스펙 트리도
같은 이름을 쓴다. `spec/server/session/`은 번호가 빠져 있었고 2026-08-25에
`04-session/`으로 바꾸면서 저장소 전체 참조 394건(114파일)과 `doc/site/mkdocs.yml` nav
3줄을 함께 치환했다. **mkdocs nav는 `.md` 경로 치환 규칙에 걸리지 않으므로 디렉터리를
바꿀 때마다 따로 확인한다.**

주제를 새로 시작할 때 `topics/NN-주제/`를 만들고 위 구성을 따른다. 파일 이름에 주제 이름을
넣지 않는다 — 디렉터리가 그 역할을 한다. 대장 파일은 재작성하는 문서의 번호를 따른다.

**04-session이 모든 양식의 기준이다.** 새 주제를 시작하는 작업자는 그 디렉터리의 문서 4종을
먼저 읽고 같은 구조로 만든다.

## 12. 관련 문서

- [스펙 문서 작성 가이드](../../principal/documentation/spec-writing-guide.ko.md)
- [기술문서 작성 원칙](../../principal/documentation/documentation-principles.ko.md)
- [공개 계약 절차](../../../framework/doc/framework/common/spec/server/00-foundation/01-public-contract-governance.ko.md#5-공개-계약-절차)
- 저장소 규칙: `doc/AGENTS.md`, `framework/AGENTS.md`
