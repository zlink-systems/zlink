# Agent Guidelines

이 문서는 저장소 전체의 에이전트/자동화 작업 규칙을 정의한다.
`CLAUDE.md`가 이 파일을 단일 기준(single source of truth)으로 참조한다.

---

## 작업 브랜치 규칙

사용자가 특정 branch 사용을 명시적으로 요청하지 않는 한 모든 조사 후 수정,
commit, push 작업은 `main` branch에서 수행한다.

- 작업을 시작하기 전에 `git branch --show-current`로 현재 branch를 확인한다.
- 현재 branch가 `main`이 아니면 파일을 수정하거나 commit·push하지 않는다.
- worktree가 clean하면 `main`으로 전환한 뒤 작업을 시작한다.
- worktree에 변경사항이 있으면 사용자의 변경을 보존한 상태와 범위를 먼저 보고하고,
  사용자 승인 없이 branch 전환, `reset`, `restore`, 강제 checkout 또는 삭제를 수행하지 않는다.
- 사용자가 특정 branch를 명시적으로 요청한 경우에만 해당 branch에서 작업할 수 있으며,
  작업 시작과 최종 보고에 그 branch 이름을 명시한다.
- branch를 만들거나 바꾸거나 merge하는 작업은 사용자의 명시적 요청 없이 수행하지 않는다.

## Chat Output Rules

사용자에게 보내는 진행 상황, 질문, 판단 근거와 최종 보고는
[`doc/principal/documentation/documentation-principles.ko.md`](./doc/principal/documentation/documentation-principles.ko.md)를
따른다. 이 문서를 채팅에 쓰는 한국어 기술 설명의 단일 기준으로 사용한다.

- 독자가 바로 판단하고 다음 행동을 정할 수 있도록 결과와 현재 조건을 먼저 설명한다.
- 검증한 사실, 남은 조건과 다음 작업을 구분하고 관련 파일·test·수치를 근거로 제시한다.
- 서버 개발자가 일상적으로 쓰는 영어 전문 용어와 코드 식별자는 그대로 쓴다. 처음 나오는 개념은
  하는 일을 먼저 설명한 뒤 이름을 붙인다.
- 명사를 이어 붙인 문장, 구어체, 의인화, 작성 태도에 대한 자평을 피한다. 동작과 원인이 드러나는
  표준 서술체로 쓴다.
- 진행 이력을 길게 나열하지 않는다. 현재 판단에 필요한 변화만 설명하고, 자세한 이력은 ledger나
  작업 log의 정확한 위치를 제시한다.
- 출력에 표나 목록을 사용하면 비교나 판단이 쉬워질 때만 사용한다. 짧은 사실은 산문으로 설명한다.

---

## Documentation Directory Purpose

`doc/` 아래 각 디렉토리는 독자와 목적이 다르다.
문서를 작성하거나 수정할 때 대상 독자에 맞는 디렉토리에 내용을 넣어야 한다.

| 디렉토리 | 대상 독자 | 목적 | 작성 기준 |
|----------|----------|------|----------|
| `doc/spec/` | 바인딩 개발자, API 계약 검토자 | **공개 API 계약** — 함수 시그니처, 반환값, 에러 코드, 소유권 규칙 등 정확한 계약 명세 | `core/include/zlink.h` 가 단일 기준. 계약에 없는 동작은 보장하지 않는다 |
| `doc/guide/` | 라이브러리 사용자 (애플리케이션 개발자) | **의도, 목적, 사용법** — 왜 이 API가 존재하는지, 언제 어떤 패턴을 쓰는지, 실전 예제 | 사용자가 "어떻게 쓰지?"라고 물을 때 답이 되어야 한다. 내부 구현 설명은 넣지 않는다 |
| `doc/internals/` | 라이브러리 유지보수자 (core 개발자) | **내부 구조 상세** — 소켓 배선, 데이터 흐름, 스레드 모델, 프로토콜 인코딩 등 구현 상세 | 코드를 읽기 전에 전체 그림을 잡을 수 있어야 한다. 다이어그램 중심으로 작성한다 |

핵심 원칙:
- **guide에 내부 구현을 넣지 않는다.** 사용자는 내부 소켓이나 inproc endpoint를 알 필요가 없다.
- **spec에 사용법 설명을 넣지 않는다.** spec은 계약만 정의한다.
- **internals에 사용법을 넣지 않는다.** internals는 유지보수자를 위한 구조 설명이다.
- guide에서 내부 동작을 참고해야 하면 internals 문서를 링크한다.

## 보호 문서 경로 — 사용자 명시 지시 없는 수정 금지

다음 정식 문서 경로는 사용자의 **명시적인 수정 지시 없이는 절대로 변경하지 않는다**.

- `core/doc/internals/`
- `core/doc/spec/`
- `bindings/doc/spec/`
- `framework/doc/framework/common/sample/`
- `framework/doc/framework/common/e2e/`
- `framework/doc/framework/common/spec/`
- `framework/doc/framework/common/internals/`

이 규칙은 agent, 자동화 script, formatter, link checker, 문서 동기화 작업과 생성 단계에 모두
적용한다. 다음 행위도 수정으로 간주하므로 사용자 승인 없이 수행하지 않는다.

- 파일과 디렉터리의 생성·수정·삭제·이동·이름 변경
- 오탈자·문체·번역·목차·링크·anchor·front matter만 고치는 변경
- 구현 gap, test 실패, cross-language parity, release 준비, stale 내용 또는 CI 결과를 이유로 한
  임의의 계약·internals·sample·E2E 문서 갱신
- 자동 포맷팅, 줄바꿈 정규화, generated 문서 재생성 또는 다른 문서와의 내용 동기화

해당 경로는 검색·열람·비교·렌더링·검증만 허용한다. 구현이나 검토 중 수정 필요성을 발견하면
파일을 먼저 고치지 말고 정확한 `file:line`, 변경 이유와 제안 내용을 보고한 뒤 대기한다. 계획서,
ledger, test 결과, 다른 언어 구현 또는 일반적인 "문서 정리"·"스펙에 맞춰"·"모두 완료" 지시는
이 보호 경로에 대한 명시적 수정 승인을 대신하지 않는다. 사용자가 보호 대상 경로와 허용할
변경 범위를 분명히 지정한 경우에만 그 범위 안에서 수정하고, 범위를 넓히지 않는다.

### Framework 언어별 문서 위치

`framework` 아래 언어별 문서는 모두 `framework/doc/` 아래에서 작성하고 수정한다.
나중에 `.NET`, Java/Kotlin, Node.js, C++ framework, HTTP client, stream connector 문서를 고칠 때도
각 언어 디렉토리의 `framework/languages/<lang>/doc/`가 아니라 `framework/doc/` 아래의
컴포넌트별 위치에서 진행한다.

- framework 기반 계약(세 패키지 공통): `framework/doc/framework/common/spec/`
- framework(서버) 패키지 계약: `framework/doc/framework/common/spec/` (+ `server/languages/<lang>/`)
- HTTP client 패키지 계약: `framework/doc/framework/common/spec/http-client/` (+ `http-client/languages/<lang>/`)
- stream connector 패키지 계약: `framework/doc/framework/common/spec/stream-connector/` (+ `stream-connector/languages/<lang>/`)
- 언어별 사용 안내: `framework/doc/framework/<lang>/guide/` 아래의 `server/`,
  `http-client/`, `stream-connector/`. 유지보수자용 내부 설명은 같은 언어 디렉터리의
  `internals/`에 둔다. **계약은 패키지로, 사용 안내는 언어로 나눈다.**
- 언어별 server, HTTP client와 stream connector 사용 안내는 각각
  `framework/doc/framework/<lang>/guide/server/`,
  `framework/doc/framework/<lang>/guide/http-client/`,
  `framework/doc/framework/<lang>/guide/stream-connector/`에서 관리한다.
- `framework/doc/http-client/`와 `framework/doc/stream-connector/`에는 공통 안내와 계획만 두고,
  언어별 사용 안내를 추가하지 않는다.

새 언어별 문서를 `framework/languages/<lang>/doc/` 아래에 추가하지 않는다. 기존 문서를 수정해야
하면 `framework/doc/` 아래의 대응 위치로 옮기거나 그 위치에서 갱신한다.

언어별 public interface의 정식 시그니처는
`framework/doc/framework/common/spec/<package>/languages/<lang>/`에서 고정한다. 공통 동작과 언어별 표현,
정식 계약 변경 절차는
[`framework/doc/framework/common/spec/00-public-contract-governance.ko.md`](./framework/doc/framework/common/spec/00-public-contract-governance.ko.md)를
따른다.

### Framework public contract parity

framework의 public contract는 언어별로 임의로 달라지면 안 된다. 하지만 다른 언어에 기능이 있다는
이유만으로 새 public contract를 추가해서도 안 된다. public contract의 기준은 spec, 공통 framework
spec/guide처럼 저장소가 명시한 계약 문서다. spec 또는 공통 framework spec/guide에 없는 기능은,
다른 언어가 이미 제공하더라도 새 public API로 추가하지 않는다. 공통 E2E 문서는 구현 검증 기준이며,
누락을 찾는 입력으로 사용한다. 하지만 공통 E2E 문서나 다른 언어 구현만으로 새 public API를 추가하지
않는다. 다른 언어의 구현은 계약 여부를 확인할 때 참고하는 증거일 뿐, 그 자체가 계약의 출처가 아니다.

한 언어의 framework에 존재하는 public API, 동작, E2E scenario가 다른 언어에 없으면 먼저 공통 E2E
문서, 해당 언어의 spec/guide 문서, 이미 구현된 다른 언어의 public surface를 비교한다. 비교 결과
spec 또는 공통 framework spec/guide에 근거가 있는 framework 공통 기능이면 모든 framework 언어가
같은 기능을 제공하도록 구현해야 한다. 특정 언어에서 바로 구현할 수 없으면 feature-map 또는 draft
문서에 누락 사유, 필요한 public API, 구현 계획을 명시하고 리뷰를 받은 뒤 gap으로 남긴다. gap
표기는 완료 판정이 아니라 후속 public contract parity 작업의 입력이다.

다른 언어에는 있지만 spec 또는 공통 framework spec/guide에 없는 기능은 바로 public API로 추가하지
않는다. 공통 E2E scenario가 그 기능을 요구하더라도, 새 public API가 필요하다면 구현하지 말고 먼저
draft/spec/guide 갱신이 필요한 설계 후보로 분리하고, 계약으로 받아들일지 리뷰를 받는다. 해당 기능은
계약이 확정되기 전까지 feature-map에 public contract gap으로 남긴다.

금지 사항:

- 한 언어에서만 public 기능을 제공하고 다른 언어의 누락을 단순 E2E 미구현으로 처리하지 않는다.
- spec 또는 공통 framework spec/guide에 없는 기능을 공통 E2E 문서나 다른 언어 구현만 근거로
  public contract에 추가하지 않는다.
- public contract 누락을 internal helper, private API, raw-frame 우회, 테스트 전용 adapter로
  메우지 않는다.
- 언어 특성 차이를 이유로 기능을 제외할 때는 사용자가 체감하는 public 동작 차이와 대체 계획을
  문서화한다.

기본 절차:

1. 공통 E2E 문서에서 요구 scenario를 확인한다.
2. spec, 공통 framework spec/guide에 계약 근거가 있는지 확인한다. 공통 E2E 문서는 새 public API
   추가 근거가 아니라 검증 요구와 누락 식별 기준으로 사용한다.
3. 이미 구현된 언어의 public API와 guide 예제를 참고해 계약 해석이 일관적인지 확인한다.
4. 계약 근거가 있으면 누락 언어에서 같은 수준의 public API로 구현 가능한지 확인한다.
5. 가능하면 해당 언어 framework에 public 기능과 E2E를 추가한다.
6. 계약 근거가 없거나 바로 구현할 수 없으면 spec/guide/feature-map에 사유를 명시하고 별도 설계
   이슈로 분리한다.

---

## Documentation Writing Rules

문서를 작성하거나 수정할 때는 아래 규칙을 따른다.

### 1. 금지 표현

- 문서 본문에서 `language-exchange` 표현은 사용하지 않는다.
- 문서 본문에서 "문서작성" 같은 붙여 쓴 표현은 사용하지 않는다.
- 필요하면 뜻이 분명한 쉬운 한국어로 풀어서 다시 쓴다.

### 2. 쉬운 설명 우선

- 문서는 항상 이해하기 쉽게 풀어서 설명한다.
- 짧게 쓰더라도 의미를 생략하지 않는다.
- 구현 배경, 목적, 제약이 있으면 독자가 바로 이해할 수 있게 문장으로 설명한다.
- 용어를 처음 꺼낼 때는 가능한 한 쉬운 말로 한 번 풀어 쓴다.

### 3. 문서 톤

- 독자가 처음 읽어도 따라올 수 있게 설명형 문장으로 작성한다.
- 내부 작성자만 아는 줄임말, 압축 표현, 맥락 없는 단정형 문장을 피한다.
- 규칙이나 제한 사항은 왜 필요한지 짧게라도 함께 적는다.
- **객체·연결·상태를 의인화하거나 구어체로 쓰지 않는다.** actor·session·연결 같은 사물을 사람이나
  생물의 동작(산다/살아 있다/살려 둔다/물려 있다/붙는다/돈다/든다 등)으로 표현하지 말고, 중립적
  기술 표현으로 쓴다.
  - actor 가 어느 Spot 에 **사는가** → 어느 Spot 에 **존재하는가**
  - 연결이 **물려 있는가** → 연결이 **설정되어 있는가**
  - client 가 **안 붙어도** / 다시 **붙으면** → client 가 **연결되지 않아도** / 다시 **연결하면**
  - actor 는 **살아 있고** / **살려 둔다** → actor 는 **유지되고** / **유지한다**
  - 두 가지 모습으로 **돈다** → 두 가지 모습으로 **동작한다**
  - 이동 후 다시 **살아난다** → 다시 **만들어진다**(또는 **materialize된다**)
  - STREAM 하나만 **들면 된다** → STREAM 하나만 **사용하면 된다**

### 4. 압축 영어 표현 금지

- `canonical caller-provided storage recv`처럼 영어 명사를 여러 개 이어 붙인 압축 표현을 쓰지 않는다.
- 이런 표현은 영어로 성립하더라도 독자가 바로 이해하기 어렵고, 의미가 내부 구현자에게만 분명해질 수 있다.
- 문서와 코드 주석에서는 동작을 문장으로 풀어 쓴다.
  - 나쁜 예: `Canonical caller-provided storage recv.`
  - 좋은 예: `Receives a message into the Received object supplied by the caller.`
  - 한국어 문서에서는 `호출자가 넘긴 Received 객체에 수신 결과를 채운다.`처럼 쓴다.
- `canonical`, `storage`, `surface`, `path`, `shape` 같은 단어는 의미가 꼭 필요할 때만 쓰고, 가능하면 무엇을 보장하는지 구체적으로 설명한다.

### 5. 임시 작업 문서를 공개 문서에서 참조하지 않는다

`framework/doc/plan/`, `bindings/doc/plan/`, `core/doc/plan/`은 작업이 끝나면 사라지는 임시
문서다. 공개 문서 트리는 이 문서들을 **링크로 가리키지 않는다.**

공개 문서 트리는 `framework/doc/framework/`(spec · internals · guide · perf),
`core/doc/`(spec · internals · guide), `bindings/doc/`(spec · guide)이다.

왜 금지하는지:

- plan 문서가 지워지면 공개 문서에 **죽은 링크**가 남는다.
- 계약을 확인하려는 독자가 **곧 사라질 문서**를 보게 된다. 계약의 출처는 정식 spec뿐이다.
- 사이트 배포 게이트가 `mkdocs build --strict`이고 plan은 사이트에 올리지 않으므로,
  링크 하나가 **배포를 막는다.**

갭이나 진행 상태를 알려야 하면 **사실만 본문에 문장으로 남기고 링크는 걸지 않는다.**
갭 목록의 항목 ID(`A1`, `D5` 같은 것)도 공개 문서에서 쓰지 않는다 — 그 체계는 plan 문서에만
있어서 독자가 해석할 수 없다.

커밋 전에 확인한다.

```bash
grep -rn "](.*plan/" --include='*.md' framework/doc/framework core/doc bindings/doc \
  | grep -v '/plan/'
```

출력이 있으면 그 링크를 걷어낸 뒤 커밋한다.

### 6. 구현 전 spec 작성 규칙

`framework/doc/framework/common/spec/`의 framework public contract는 모든 언어가
도달해야 하는 목표 계약을 먼저 정식 spec에 고정한다. 각 언어의 정확한 public
interface도 `framework/doc/framework/common/spec/<package>/languages/<lang>/`에 먼저 기록한다.
현재 구현과 다른 부분은 `90-implementation-gap.ko.md`와 언어별 interface의 구현 차이
표에 기록하고, 이후 구현과 contract test를 spec에 맞춘다. 구현이 없다는 이유로
공통 기능을 현재 언어들의 최소 공통분모로 축소하지 않는다.

RouteMesh 11.0.0의 Core service runtime 이관은 major version의 책임 경계를 다시 정하므로 Core에도
target-first 예외를 적용한다. `core/doc/spec/core/`의 정식 spec에 Core 11 raw-only 목표 계약을 먼저 기록하고,
public header·구현과의 차이는
`framework/doc/plan/v11.0/route-mesh-11.0.0-execution-ledger.ko.md`가 소유한다. Core 정식 spec에는 이전
service C ABI, 제거 이력, 진행 상태와 대안을 남기지 않는다. Service 의미는 Framework 공통 정식 spec과
다섯 언어 exact interface가 소유하며, Framework 전용 public·private Core C ABI를 새로 만들지 않는다.
실제 구현이 확정되기 전의 internals는 기존 구조를 목표 구조로 오인하게 설명하지 않으며, 구현 완료 뒤
Core 11에 남는 raw runtime 구조만 기록한다.

이 framework와 RouteMesh 11.0.0 Core 이관 예외 밖의 core, bindings와 일반
`doc/spec/` 작업은 아래 draft 규칙을
계속 따른다.

- 아직 구현되지 않은 API 계약이나 동작은 기존 정식 spec 문서에 바로 넣지 않는다.
- 구현 전 설계는 `doc/spec/draft/` 아래의 **별도 draft 문서**로 작성한다.
- draft 문서는 기능 단위로 작성한다. 여러 정식 spec 문서(예: socket, monitoring, discovery, spot)에 걸치는 내용이면 draft 단계에서는 한 문서로 함께 정리한다.
- draft 문서 파일명은 기능 이름이 드러나게 잡는다. 가능하면 `peer-admission.ko.md`처럼 주제를 직접 드러내고, 필요하면 `-draft` 접미사를 추가한다.
- draft 문서 첫머리에는 이 문서가 **구현 전 초안**이며 **현재 공개 계약이 아님**을 분명하게 적는다.
- 정식 spec 문서(`router.ko.md` 같은 기존 계약 문서)는 현재 구현과 공개 헤더에 존재하는 내용만 유지한다.
- 구현이 끝난 뒤에는 `core/include/zlink.h`, 관련 테스트, errno 문서, 바인딩 문서와 맞춘 다음 draft 내용을 적절한 정식 spec 문서들로 나누어 반영한다.
- 구현 전 단계에서 정식 spec 문서에서 draft 내용을 섞어 쓰지 않는다. 필요하면 정식 문서에서 별도 draft 문서를 짧게 링크만 한다.

### 7. 산문 설명 + 코드 주석으로 API 매칭

- 어떤 API·함수가 각각 무엇을 하는지 알려줄 때, 산문 안에 함수 이름을 여러 개 나열하지 않는다. 나열은 문장 가독성을 떨어뜨린다.
- 산문에서는 원칙·의도·언제 쓰는지를 풀어서 설명한다. "어떤 API가 무엇을 등록/수행하는지" 같은 일대일 매칭은 바로 아래 코드 예제의 **해당 호출 옆 주석**으로 옮긴다.
- 이렇게 하면 산문은 읽기 쉽고, API 매칭은 실제 호출 위치에서 바로 확인된다. 코드가 없으면 짧은 예제를 함께 둔다.
  - 나쁜 예(산문에 나열): "`AddPacket<T>(...)` / `AddActorPacket<T, TActor>(...)` / `AddSubscribe<T>(...)` 와 `AddTimer<T>(...)` 로 등록한다."
  - 좋은 예(산문 + 코드 주석): 산문은 "spot 코드의 `Configure()`/lifecycle 에서 등록한다(어떤 API가 무엇을 등록하는지는 아래 코드 주석 참고)"로 두고, 코드에서 `Context.Handlers.AddPacket<T>(); // send/request packet handler 등록`처럼 호출 옆에 설명한다.
- **예제 코드는 그냥 코드만 두지 않는다.** 그 단락이 설명하려는 핵심 지점을 코드 안 주석으로 한 번 더, 코드와 함께 설명한다. 독자가 산문과 코드를 오가지 않고 코드만 봐도 "이 줄이 무엇을, 왜 하는지" 알 수 있어야 한다.
  - 모든 줄에 주석을 달라는 뜻이 아니라, 그 단락의 설명 포인트(핸들러 인자의 의미, 등록 API, 수명·순서·제약 등)가 닿는 줄에만 단다.
  - 주석은 코드를 그대로 옮겨 적는 게 아니라(`i++; // i 를 1 증가`처럼) 코드만으로 드러나지 않는 의도·계약을 적는다.

---

## Source Comment Rules

코드 안에 공개 API 주석이나 내부 주석을 작성할 때는 아래 문서를 따른다.

**[`doc/principal/source-comment-principles.ko.md`](./doc/principal/source-comment-principles.ko.md)**

핵심 원칙:
- 공개 API 주석은 계약 문구다. 호출자가 API를 올바르게 쓰기 위해 알아야 하는 공개 동작만 적는다.
- 사용법, 예제, 설계 배경은 주석에 길게 넣지 말고 `doc/guide/` 또는 언어별 guide 문서에 둔다.
- 내부 구현, 현재 최적화, 우회 경로를 공개 보장처럼 설명하지 않는다.
- timeout, cancellation, callback, ownership, disposal, error 같은 계약은 명확히 적는다.
- 코드가 무엇을 하는지 반복하는 주석은 피하고, 코드만으로 드러나지 않는 결정만 설명한다.

---

## ASCII Diagram Rules

문서 내 ASCII 다이어그램(stacked-layer, box diagram 등)은 아래 규칙을 따른다.

### 1. 영문 전용

- ASCII 다이어그램 내부 텍스트는 **영문만** 사용한다.
- 한국어 문서(`.ko.md`)에서도 다이어그램 내부는 영문으로 작성한다.
- 한국어 설명이 필요하면 다이어그램 아래에 별도 텍스트로 기술한다.

### 2. 고정 폭 정렬

- 모든 행의 **가시 폭을 동일하게** 맞춘다.
- 박스 테두리는 `+`, `-`, `|` 조합을 사용한다.
  - 상단/하단: `+---...---+`
  - 좌우 벽: `|`
  - 내부 구분: `+---...---+` 또는 `|---...---|`
- 각 행의 마지막 `|` 또는 `+` 뒤에 trailing space를 넣지 않는다.
- 탭 문자를 사용하지 않는다. 공백만 사용한다.

### 3. 폭 제한

- 한 행의 가시 폭은 **72자 이하**를 권장한다.
- 불가피하면 80자까지 허용하되, 초과하지 않는다.

### 4. 화살표

- 수직: `↓`, `↑` (유니코드) 또는 `v`, `^` (ASCII)
- 수평: `→`, `←` (유니코드) 또는 `->`, `<-` (ASCII)
- 유니코드 화살표를 사용할 경우 고정 폭 폰트에서 1칸으로 렌더링되는지 확인한다.

### 5. 코드 펜스

- 다이어그램은 반드시 ` ``` ` (또는 ` ```text `) 코드 펜스 안에 넣는다.
- 코드 펜스 밖의 마크다운 렌더링에 의존하지 않는다.

### 6. Mermaid 구분

- 흐름도(flowchart), 시퀀스 다이어그램은 Mermaid를 사용한다.
- 계층 구조(stacked-layer), 메모리 레이아웃, 프레임 구조는 ASCII를 사용한다.

---

## 바인딩 사용 규칙 (.NET 프레임워크)

`framework/languages/dotnet/` 코드가 `bindings/dotnet/` 라이브러리를 사용할 때는
반드시 공개(public) API만 호출해야 한다.

### 금지 사항

- `System.Reflection` 으로 `internal` / `private` 멤버에 접근하는 것은 금지한다.
  - `GetMethod`, `GetField`, `GetProperty` 에 `NonPublic` 플래그를 사용하는 것도 금지한다.
  - `MethodInfo.Invoke`, `FieldInfo.GetValue` 등을 통한 내부 멤버 호출도 금지한다.
- 바인딩 어셈블리의 `InternalsVisibleTo` 를 임의로 추가해서 접근권을 우회하는 것도 금지한다.

### 필요한 기능이 바인딩에 없을 때

- 바인딩 라이브러리에 필요한 `public` API 가 없으면, 바인딩에 해당 API 를 추가한다.
- 프레임워크 안에서 임시로 리플렉션 우회 코드를 작성하지 않는다.
- 바인딩 API 추가 후 프레임워크에서 공개 API 를 직접 호출하는 방식으로 연결한다.

### 이유

리플렉션으로 내부 멤버에 접근하면 바인딩 라이브러리가 내부 구현을 바꿀 때 프레임워크가 조용히
런타임 오류를 낸다. 컴파일 타임에 오류를 잡을 수 없어 유지보수 위험이 크다.

---

## Bindings Local Package 배포 규칙

framework가 bindings 라이브러리를 참조하는 방식, local package 생성 위치, 언어별 버전 고정 지점은
**[`scripts/local-package/README.ko.md`](./scripts/local-package/README.ko.md)** 를 기준으로 한다.

bindings 배포, local package, framework의 bindings 참조 버전, WSL/Windows local package 경로를
수정할 때는 먼저 이 문서를 확인하고, 문서의 정책과 실제 스크립트가 어긋나지 않게 함께 갱신한다.

핵심 원칙:

- framework는 bindings 소스를 직접 참조하지 않고, 명시한 버전의 local package나 배포 package를 참조한다.
- bindings 새 버전을 local package 위치에 배포해도 framework 참조 버전을 바꾸기 전까지는 기존 버전을
  계속 사용해야 한다.
- framework 참조 버전은 언어별 중앙 지점에서 바꾼다. Java/Kotlin은
  `framework/languages/java/gradle/libs.versions.toml`, Node.js는
  `framework/languages/node/package.json`, .NET은 `Directory.Packages.props`, C++는
  `ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION`을 기준으로 한다.
- local package 생성 스크립트는 `scripts/local-package/` 아래에서 관리한다. 같은 목적의 wrapper를
  `bindings/<lang>/` 또는 다른 디렉터리에 다시 만들지 않는다.
- Core를 고쳤으면 `native/sync-local-core-libs.sh`와 언어별 package 생성까지 끝내야 framework가 그
  Core를 쓴다. 이 단계를 건너뛰면 실패하지 않고 **옛 라이브러리가 그대로 쓰인다.** .NET이면
  framework는 `~/.nuget/packages/systems.zlink/<ver>/runtimes/`에 이미 풀린 native를 읽으므로,
  같은 버전으로 다시 만들어도 추출된 캐시를 지우지 않으면 갱신되지 않는다. Core에 넣은 추적이
  아무것도 찍히지 않으면 코드가 안 도는 것이 아니라 배포가 안 된 것을 먼저 의심한다.

---

## Benchmark Build Rules

`bindings/c/perf` 를 기준으로 성능을 볼 때는 빌드 산출물 경로를 혼동하면 안 된다.
이 실수는 수치 해석 자체를 틀리게 만들기 때문에 문서와 자동화에서 같은 규칙을 유지한다.

- `bindings/c/perf/run_benchmarks_multi.sh` 의 core runtime 기준 경로는 기본값으로 `core/build` 이다.
- `build_cpp_release` 나 다른 임시 빌드 디렉토리 결과로 `bindings/c/perf` 수치를 판단하지 않는다.
- `core/src/` 또는 `core/include/` 를 바꾼 뒤에는 `cmake --build core/build` 로 실제 runtime 을 먼저 다시 만든다.
- perf 실행 전에 runner 가 실제 `libzlink.so` 경로를 출력해야 한다.
- perf 실행 전에 `core/build` runtime 이 source 보다 오래되면 바로 실패해야 한다. 오래된 runtime 으로 벤치를 계속 돌리지 않는다.
- 이 규칙은 루트 `README` 와 `bindings/c/perf/README.md` 에도 같은 뜻으로 유지한다.

---

## 기존 설계 우선 규칙

작업을 시작할 때는 먼저 현재 코드베이스가 이미 정한 설계 의도, 공개 인터페이스,
표준 사용 패턴을 확인한다. 요청을 만족시키기 위해 새 helper, 새 abstraction, 새 public API,
우회 경로를 만들기 전에 기존 표면으로 해결할 수 있는지 검증한다.

### 1. 기존 표면 우선

기능이 실패하거나 샘플이 깨져도, 먼저 기존 public API와 표준 사용 패턴을 유지한 채
원인을 찾는다.

금지되는 접근:

- 기존 표준 호출 경로를 우회하는 helper 추가
- 특정 샘플이나 특정 실패만 통과시키는 adapter 추가
- public API 사용 예시를 바꾸는 임시 workaround
- 기존 추상화가 있는데 병렬 추상화를 새로 만드는 것
- 타입, codec, transport, framework 경계를 호출자 코드로 밀어내는 것

기존 표면으로 해결할 수 없다고 판단되면 바로 구현하지 말고, 왜 불가능한지 근거와 함께
별도 설계 이슈로 분리한다.

### 2. 증상 해결보다 책임 경계 유지

버그를 고칠 때는 "어디에 코드를 넣으면 통과하는가"보다 "이 책임이 원래 어느 모듈에
있어야 하는가"를 먼저 판단한다.

호출자가 몰라도 되는 정보가 새로 노출되면 설계 후퇴로 본다.
예: encoding 방식, transport detail, retry 내부 정책, lifecycle 내부 순서,
registry lookup 방식, generated/internal type 선택 등.

### 3. 새 코드 추가 전 질문

새 함수, 새 클래스, 새 옵션, 새 public method, 새 helper를 추가하기 전에 아래를 확인한다.

- 이미 같은 목적의 API나 helper가 있는가?
- 이 변경이 호출자에게 내부 결정을 노출하는가?
- 다른 언어, 샘플, 문서의 표준 사용 패턴과 충돌하는가?
- 이 코드가 실패 원인을 고치는가, 아니면 실패 지점을 우회하는가?
- 같은 문제가 다른 샘플에서도 반복될 때 이 방식이 표준으로 받아들일 만한가?

하나라도 불확실하면 구현 전에 조사 결과와 선택지를 먼저 정리한다.

### 4. 샘플은 public contract다

샘플 코드는 단순 테스트 코드가 아니라 사용자가 따라 할 공개 API 예시다.
샘플을 통과시키기 위해 샘플에 내부 helper, 임시 변환, private policy,
low-level escape hatch를 넣지 않는다.

샘플에서 보기 이상한 코드는 대부분 public surface 설계 문제이거나 runtime 연결 문제다.
샘플 코드를 복잡하게 만들어 해결하지 말고, 원래 책임이 있는 계층을 고친다.

### 5. 우회 구현 금지

빌드나 테스트를 통과시키기 위한 임시 우회는 완료로 보지 않는다.

아래 패턴이 보이면 작업을 중단하고 설계를 다시 확인한다.

- raw bytes/buffer를 업무 코드에서 직접 해석
- `then(decode...)`, `parse(...)`, `serialize(...)`가 호출부에 갑자기 추가됨
- handler마다 같은 option/helper를 반복 주입
- 기존 타입과 같은 의미의 새 DTO/wrapper가 생김
- 한 언어만 다른 호출 표면을 사용함
- 문서의 표준 예시와 실제 샘플 코드가 달라짐

### 6. Codec 책임 경계

framework message codec은 메시지 타입마다 호출자가 등록하는 확장 지점이 아니다.
기본 직렬화 방식은 framework가 제공하는 typed JSON serializer 경로를 우선 사용한다.
사용자 코드, 샘플, E2E, 언어별 framework 구현에서 메시지별 codec 등록 함수를 새로 만들거나
되살리거나 흉내 내지 않는다.
이미 제거된 메시지별 codec 등록 함수는 호환 계층이나 사용자 확장 지점으로 간주하지 않는다.
배포된 framework 라이브러리의 사용자도 별도 등록 없이 기본 JSON codec 경로로 메시지를 주고받아야 한다.

금지되는 접근:

- `MessageA`는 JSON, `MessageB`는 다른 codec처럼 메시지마다 codec을 등록하는 public API 추가
- 샘플이나 handler마다 codec option, serializer registry, encoder 함수를 반복해서 넘기는 방식
- 기본 JSON 직렬화 실패를 호출부의 `encode`, `decode`, `serialize`, `parse` 호출로 우회하는 방식
- 제거된 메시지별 codec 등록 함수를 compatibility helper, adapter, test utility로 다시 도입하는 방식

typed JSON serializer 경로로 해결되지 않으면 호출자 코드에 codec 책임을 밀어내지 말고,
framework 내부의 기본 직렬화 연결이 끊긴 지점을 고친다. 정말 새 codec 정책이 필요하면
먼저 spec 또는 draft에서 공개 계약으로 설계할지 분리해 검토한다.

---

## POSDDD 설계 원칙

이 저장소의 코드 설계 판단 기준은 John Ousterhout의 *A Philosophy of Software Design*
(**POSD**)과 Eric Evans의 *Domain-Driven Design* (**DDD**)을 하나로 통합한 **POSDDD**
원칙을 따른다. 두 책은 "경계를 어디에 긋고, 그 경계 뒤에 무엇을 숨길 것인가"라는 같은
문제를 다른 고도에서 다루므로, DDD는 별도 부록이 아니라 해당하는 POSD 원칙 안에 엮여
있다.

원칙 전문은 아래 문서에 있다:

- **[`doc/principal/dev/posddd.ko.md`](./doc/principal/dev/posddd.ko.md)** — 프로젝트에
  무관하게 적용하는 범용 원칙(영문판: `posddd.md`).
- **[`doc/principal/dev/zlink-system-design-principles.ko.md`](./doc/principal/dev/zlink-system-design-principles.ko.md)**
  — ZLink 프로젝트 전체가 공통으로 따르는 고유 원칙(아키텍처 구조, 도메인 어휘, 코드·테스트
  규칙). POSDDD와 충돌하지 않고 그 위에 선다(영문판: `zlink-system-design-principles.md`).

에이전트는 코드 작성·수정·리뷰 시 이 문서들을 판단 기준으로 사용한다.
단순히 "동작하는 코드"를 만드는 것이 목표가 아니라, **복잡성을 줄이는 설계**가 목표임을 항상 염두에 둔다.

### "POSD 기반으로 해줘" / "POSDDD 원칙 적용해줘"의 의미

사용자가 **POSD**, **POSDDD**, **POSD 기반**, **POSDDD 원칙**을 언급하면,
아래 설계 기준을 코드에 명시적으로 적용하라는 요청이다.

1. **깊은 모듈** — 인터페이스를 단순하게, 구현은 내부에서 흡수한다.
   인터페이스 복잡도 ≥ 구현 복잡도면 모듈이 존재 이유를 잃은 것이다.

2. **정보 은닉** — 설계 결정(자료구조, 프로토콜, 파일 형식 등)은 하나의 모듈 안에 가둔다.
   같은 지식이 두 곳 이상에 반영되어 있으면 정보가 누출된 것이다.

3. **복잡성을 아래로** — 호출자가 알아야 할 것을 최소화한다.
   설정 파라미터, 사전 조건, 예외 처리를 호출자에게 떠넘기지 않는다.

4. **오류를 정의로 없애라** — API 의미를 재정의해서 예외 자체가 발생하지 않도록 만든다.
   어쩔 수 없는 예외는 하위에서 마스킹하거나 상위 한 곳에서 집약 처리한다.

5. **두 번 설계하라** — 비자명한 설계 결정은 반드시 두 가지 이상 대안을 검토한 뒤 선택한다.
   첫 번째 아이디어를 바로 구현하지 않는다.

6. **위험 신호 제거** — 아래 패턴이 보이면 설계 문제의 증거다. 우선 해결한다:
   - 패스스루 메서드 (인자를 그대로 전달만 함)
   - 시간적 분해 (실행 순서 기준으로 클래스를 나눔)
   - 얕은 모듈 (인터페이스 ≈ 구현)
   - 특수·범용 코드 혼합
   - 코드를 반복하는 주석

7. **경계는 도메인 의미로도 잡는다** — bounded context(같은 단어가 같은 모델을 가리키는
   경계), ubiquitous language(같은 개념은 코드·문서·리뷰 전체에서 같은 이름), aggregate
   (불변 조건을 함께 지키는 경계)로 이름과 경계를 점검한다. 전체 목록은 `posddd.ko.md`
   1부를 참고한다.

### POSDDD 리팩토링 수행 절차

"POSD 기반 리팩토링"을 요청받으면 아래 순서로 진행한다:

1. 대상 코드에서 위험 신호(red flags) 목록을 먼저 열거한다.
2. 각 위험 신호마다 어떤 POSD 원칙에 위배되는지 근거를 명시한다.
3. 수정 방향을 두 가지 이상 제시하고, 더 나은 쪽을 선택하는 이유를 설명한다.
4. 변경이 인터페이스에 영향을 주면, 호출자 관점에서 복잡성이 줄었는지 확인한다.
5. 수정 후 위험 신호가 해소되었는지 다시 점검한다.
