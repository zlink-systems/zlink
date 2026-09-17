# 사용자 가이드 전면 재작성 계획

> 이 문서 하나로 새 세션을 시작할 수 있게 쓴다. 어디서 작업하고, 무엇이 필요하고, 지금
> 무엇부터 할 수 있고, 무엇이 남았는지를 담는다. 작업이 진행되면 §6의 상태를 함께 고친다.
>
> **목차·층 정의·번호대는 이 문서가 소유하지 않는다.** `doc/site/GUIDE-STRUCTURE.md`가
> 소유한다. 문장과 형식 규칙은 `doc/principal/documentation/`의 두 문서가 소유한다(§4).
> 이 문서는 그 규칙을 이 작업에 적용할 때의 **순서와 상태**만 담는다.

---

## 1. 무엇을 바꾸는가

기존 가이드는 한 장이 처음 쓰는 독자와 이미 써 본 독자에게 동시에 말한다. 등록 코드 하나를
보려는 사람이 옵션 표와 실패 분기를 먼저 통과해야 한다. 장을 **기능별 가이드**와 **동작
원리** 두 층으로 나눈다.

두 층의 정의·상한·자르는 기준은
[`doc/site/GUIDE-STRUCTURE.md` §1](../site/GUIDE-STRUCTURE.md)이 소유한다. 장을 쓰기 전에
읽는다.

---

## 2. 작업 환경

### 2.1 worktree

**작업은 `D:\project\zlink-guide` 한 곳에서 한다.** 가이드 장, 작성 규칙 문서, 언어별
tutorial 프로그램, 샘플이 모두 그 안에 있다.

| 경로 | 브랜치 | 담는 것 |
| --- | --- | --- |
| `D:\project\zlink-guide` | `docs/guide-features-channel` | **가이드 작업 전부.** 이 계획 문서가 있는 곳 |
| `D:\project\zlink` | `main` | 정본. 다른 작업이 쓴다. 읽기만 한다 |

> **왜 한 곳인가.** 장 하나는 그 장이 인용하는 tutorial 코드가 있어야 검증된다. 둘을 다른
> worktree에 두면 "파일은 있는데 마커가 없다"가 조용히 생긴다. 실제로 tutorial 사본이 두
> 벌로 갈라져, 한쪽은 framework 0.14.0에 멈춰 있고 다른 쪽만 0.15.0으로 올라간 적이 있다.
> 가이드가 읽는 쪽은 낡은 사본이었다.

**가이드 worktree는 이 하나뿐이다.** `zlink-guide-writing`·`zlink-tutorial`·`zlink-guide-06-spot`을
모두 정리했다.

마지막 것은 `06-spot.ko.md`의 절 재배치(+348줄, 나머지는 그 재배치로 깨진 앵커 수정)를
담고 있었다. §6.2가 그 장을 해체할 예정이므로 **되살리지 않기로 했다.** 다만 해체할 때 그
재배치가 참고가 될 수 있으므로 패치를 남겨 두었다.

```
doc/plan/guide-06-spot-reorder.patch     # 기준 커밋 d4212f739f
```

되살리려면:

```bash
git worktree add -b docs/guide-06-spot-reorder <경로> d4212f739f
cd <경로> && git apply <이 저장소>/doc/plan/guide-06-spot-reorder.patch
```

**동시 작업.** 이 worktree를 다른 세션이 함께 쓸 수 있다. 검사기가 스니펫 마커 오류를 무더기로
내면 먼저 파일 mtime을 본다 — 편집 중인 파일을 읽어 생긴 일시적 결과일 수 있다. 판정은
편집이 멎은 뒤 다시 돌려서 한다.

### 2.2 필요한 도구

문서 작업만 할 때 필요한 것과, tutorial을 키울 때 필요한 것이 다르다.

**문서와 사이트 — 항상 필요하다.**

| 도구 | 버전 | 확인한 값 |
| --- | --- | --- |
| Python | 3.10 이상 | 3.12.10 |
| mkdocs · mkdocs-material · mkdocs-static-i18n · pymdown-extensions | `doc/site/requirements.txt` | — |

```bash
python -m pip install -r /d/project/zlink-guide/doc/site/requirements.txt
```

§2.3의 생성기, §2.4의 미리보기, §7.3·§7.4·§7.6의 검사가 모두 이 넷을 쓴다.

**tutorial을 키울 때 — 그 언어의 것만 필요하다.** 전제와 포트는 각 언어 tutorial README가
소유한다. 아래는 색인이다.

| 언어 | 전제 | 포트(Client · Server) | Redis |
| --- | --- | --- | --- |
| .NET | .NET SDK 8.0 이상 | 5080 · 5081 | **필요하다.** 방·큐·플레이어가 Location Store를 쓴다 |
| C++ | C++20 compiler(MSVC 19.44 확인) · CMake 3.20 이상 · `zlink_framework` CMake 패키지 | 5180 · 5181 | **필요하다.** Spot 단계가 쓴다 |
| Java | **JDK 25.** 0.15.0의 class file major version이 69다. 실행 시점 `JAVA_HOME`도 25여야 한다 | 5280 · 5281 | **필요하다.** Spot 단계가 쓴다 |
| Kotlin | Java와 같다. 같은 Gradle build를 쓴다 | 5380 · 5381 | **필요하다.** Spot 단계가 쓴다 |
| Node | **Node.js 22 이상. Linux(x64)에서 실행한다** — `@zlink-systems/zlink@1.2.0` tarball에 `prebuilds/linux-x64/`만 있다. 확인은 WSL2 Ubuntu-24.04에서 했다 | 5480 · 5481 | **필요하다.** Spot 단계가 쓴다 |

**Channel 메시징 네 가지만 볼 때는 다섯 언어 모두 Redis 없이 돈다.** Spot 단계부터 Location
Store가 필요하고, 언어마다 키 prefix가 다르다 — `zlink-tutorial`(.NET) ·
`zlink-tutorial-cpp:` · `zlink-tutorial-java:` · `zlink-tutorial-kotlin:` ·
`zlink-tutorial-node:`(§7.7).

### 2.3 가이드 소스와 생성물

```
zlink-guide/framework/doc/framework/
├─ common/guide/server/      ← 소스. 다섯 언어 탭을 한 파일에 담는다
└─ {dotnet,cpp,java,kotlin,node}/guide/server/   ← 생성물과 수작업 파일이 섞여 있다
```

**언어 디렉터리를 통째로 생성물로 보지 않는다.** 그 디렉터리의 파일은 세 종류다.

| 종류 | 판별 | 편집 |
| --- | --- | --- |
| 생성물 | 머리에 `<!-- generated:start -->` 블록이 있다 | 고치지 않는다. 공통 소스를 고치고 다시 생성한다 |
| 언어 전용 장 | 생성 표시가 없다. `11`·`13`·`16`과 C++의 `18`·`19`·`42-http-hosting`·`21`(§6.5) | 손으로 고친다 |
| `README.ko.md`·`README.en.md` | 생성 표시가 없다. 읽는 순서 표를 담는다 | 손으로 고친다(§5.4 4단계) |

소스를 고친 뒤 생성한다. 생성기는 `=== "라벨"` 블록에서 그 언어 탭만 남기고 들여쓰기를 푼다.

```bash
cd /d/project/zlink-guide/doc/site
PYTHONIOENCODING=utf-8 python scripts/generate_language_guides.py

# 고치지 않고 최신인지만 본다
PYTHONIOENCODING=utf-8 python scripts/generate_language_guides.py --check
```

생성기는 그 언어 README의 읽는 순서 표에서 장 목록을 읽어 앞뒤 장 nav를 붙인다. **README에
행이 없는 장은 nav 없이 생성된다** — §5.4의 순서를 지킨다.

### 2.4 사이트 미리보기

```bash
cd /d/project/zlink-guide/doc/site
PYTHONIOENCODING=utf-8 python -m mkdocs serve -a 127.0.0.1:8766
```

전체 빌드는 이 기계에서 41초 걸렸다. `doc/site/docs/`는 정본 트리를 모은 심링크 디렉터리다.

---

## 3. 목차

**정본은 [`doc/site/GUIDE-STRUCTURE.md`](../site/GUIDE-STRUCTURE.md)가 소유한다.** 층 이름과
nav 라벨, 파일 번호대가 그 문서에 있다. 장을 옮기거나 새로 만들 때 **그 문서를 먼저 고치고
아래 트리를 함께 맞춘다.**

```
Framework
│
├─ 시작
│   ├─ 개요                          ← 01 (그대로)
│   ├─ 핵심 개념                     ← 03 (그대로)
│   └─ Quickstart                    ← 02 §1을 흡수한 언어별 정본
│
├─ 기능별 가이드
│   ├─ Channel 메시징                ← 05 §1·2(최소형)·3·4
│   ├─ Spot                          ← 06 §1·2·3·4
│   ├─ Actor                         ← 07 §1·2·3
│   ├─ STREAM                        ← 09 §1·2·3·5·6
│   ├─ Session과 Actor 연결          ← 08 (전체)
│   ├─ Location                      ← 10 §0·1·2·5
│   └─ 모니터링                      ← 11 (그대로)
│
├─ 동작 원리
│   ├─ 실행 모델                     ← 06 §3.1 + 05 "종결자"
│   ├─ Backpressure                  ← 04 (그대로)
│   ├─ 활성화와 수명                 ← 06 §5.2
│   ├─ Actor membership              ← 07 §4·5
│   ├─ Timer와 worker                ← 06 §7
│   ├─ Relocation                    ← 06 §8 + 07 §7 + 12 §2·3
│   ├─ 메시지 filter                 ← 05 §5
│   ├─ 연결 제어                     ← 05 §6
│   ├─ 직렬화 codec                  ← 05 §7
│   ├─ Channel 수평 확장             ← 05 §8·9
│   └─ ZLink를 어디에 쓰나           ← 17 (기술 선택 근거)
│
├─ 운영
│   ├─ 운영과 lifecycle              ← 12 §1·4·5·6
│   └─ 옵션                          ← 16
│
├─ 샘플과 테스트
│   ├─ 샘플 고르기                   ← 14
│   └─ E2E 테스트                    ← 15
│
├─ 레퍼런스                           ← 현 reference/ + 13 타입 색인
└─ 스펙                               ← 그대로
```

이 작업에만 해당하는 것 둘만 여기 남긴다.

**손대지 않는 장** — 01 · 03 · 04 · 08 · 11 · 13 · 14 · 15 · 16 · 17.
**해체하는 장** — 02 · 05 · 06 · 07 · 12.

---

## 4. 규칙이 어디 있는가

모두 `zlink-guide` 안에 있다.

| 문서 | 소유하는 것 |
| --- | --- |
| `doc/principal/documentation/documentation-principles.ko.md` | 모든 기술문서의 공통 원칙 1~11. 문체, 문장의 형태, 본문·표·callout의 자리 |
| `…/guide-writing-guide.ko.md` | 가이드 전용. 두 층, 코드 출처, snippet 참조, 언어 탭, **분량과 세는 방법**, 장의 형식, 파일 번호대, 완료 점검표 |
| `…/diagram-authoring-guide.ko.md` | 다이어그램 색 의미, 박스 제약, archify 변환 |
| `doc/site/GUIDE-STRUCTURE.md` | 목차, 층 이름과 nav 라벨, 각 장의 출처 |
| `doc/site/SPEC.md` | 사이트 빌드 설정 |

장을 쓰기 전에 앞의 둘을 읽는다. 장을 마칠 때 `guide-writing-guide.ko.md` §7 완료 점검표를
돌린다. 분량을 세는 방법은 그 문서 §3.2가 소유한다.

### 4.1 코드 예제의 출처

출처를 고르는 규칙은 `guide-writing-guide.ko.md` §4가 소유한다 — 기능별 가이드는 tutorial,
동작 원리는 샘플 우선, unit test와 e2e에서는 가져오지 않는다, 지원 언어의 소스 예제는 전부
snippet 참조다.

**여기서는 그 규칙이 가리키는 실제 경로만 적는다.**

#### tutorial 위치

모두 `framework/languages/` 아래다. **Kotlin은 Java와 한 Gradle build를 공유하므로
`java/tutorial/kotlin/`에 있다.** `guide-writing-guide.ko.md` §4.1은 이 자리를
`framework/languages/<언어>/tutorial/`로 한 줄로 적어 두었으나 Java·Kotlin에는 맞지 않는다.
아래 표가 맞다.

| 언어 | 경로 |
| --- | --- |
| .NET | `framework/languages/dotnet/tutorial/` |
| C++ | `framework/languages/cpp/tutorial/` |
| Java | `framework/languages/java/tutorial/java/` |
| Kotlin | `framework/languages/java/tutorial/kotlin/` |
| Node | `framework/languages/node/tutorial/` |

#### 샘플 위치

일곱 벌이 **다섯 언어 모두**에 있다 — `TicTacToe` · `ZoneWorld` · `Bingo` ·
`SupportChat` · `DeliveryDispatch` · `ShoppingMall` · `GameQuest`.

| 언어 | 경로 | 이름 |
| --- | --- | --- |
| .NET | `framework/languages/dotnet/samples/<이름>/` | 그대로. 공용 코드는 `Common/` |
| C++ | `framework/languages/cpp/samples/<이름>/` | 그대로 |
| Java | `framework/languages/java/samples/java/<이름>/` | 그대로 |
| Kotlin | `framework/languages/java/samples/kotlin/<이름>/` | 그대로 |
| Node | `framework/languages/node/samples/<이름>.Ts/` | **`.Ts` 접미사.** `ZoneWorld`만 예외로 접미사가 없다 |

snippet 참조는 이 경로를 그대로 쓴다.

```
--8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Api/ApiServer.cs:doc-manual-peer-connect"
```

**샘플의 마커 이름은 `doc-` 접두사를 쓴다. tutorial은 접두사 없이 쓴다.** 현재 위반 0건이다.

---

## 5. 작업 순서

### 5.1 지금 착수할 수 있는 것

§5.2의 표를 위에서부터 훑으면 1번에서 바로 막힌다 — 시작 층의 네 장은 mkdocs nav 재배치로만
옮기는데, 그 재배치는 가리킬 파일이 다 생긴 뒤에 걸기 때문이다(§6.4). **목차 순서는 완성
순서가 아니라 목적지다.**

**주제 하나를 끝까지 닫고 다음으로 간다.** 지금 열려 있는 주제는 Channel 메시징 하나이고,
장 셋(20·30·31)과 원본 `05`가 모두 거기 걸려 있다. 반쯤 닫힌 주제를 둔 채 Spot으로 넘어가면
`05`를 지우지 못하고, 그동안 독자는 같은 내용을 두 곳에서 본다.

#### 1단계 — Channel 메시징을 닫는다

| 순위 | 작업 | 막는 것 |
| --- | --- | --- |
| ~~1.1~~ | ~~30·31번의 자리 표시 36개를 채운다~~ | **끝났다** |
| ~~1.2~~ | ~~네 언어 tutorial에 Spot 최소형을 넣는다~~ | **끝났다.** 네 언어 모두 빌드하고 실행해 확인했다 |
| ~~1.3~~ | ~~20번의 자리 표시 4개를 채운다~~ | **끝났다.** C++ 탭이 25줄을 넘어 `spot-message-call`을 `spot-send-call`·`spot-request-call`로 나눴다 |
| ~~1.4~~ | ~~`.en.md` 세 벌을 쓴다~~ | **끝났다.** 영어 다이어그램 7개도 함께 만들었다. 다섯 언어 × 세 장이 모두 빌드된다 |
| ~~1.5~~ | ~~`05` 삭제와 참조 정리~~ | **끝났다.** 파일 12개를 지우고 링크 154곳을 다시 걸었다 |

> **1.2를 빠뜨리기 쉽다.** 남은 자리 표시 40개 중 **36개만 샘플에서 온다.** 20번은 기능별
> 가이드이므로 코드를 **tutorial에서 읽어야 하고**(§4.1), 그 4개가 있는 절은
> `### 3.7 Spot·Actor 호출하기`다. .NET 탭은
> `dotnet/tutorial/Client/Program.cs:spot-message-call`을 가리키는데, **네 언어 tutorial에는
> Spot이 없다** — `Server` 소스에 "Spot and Actor handlers are not covered" 주석만 있다.
> 샘플에서 가져다 채우면 §4.1의 출처 규칙을 어긴다.
>
> 필요한 것은 §5.3의 전면 확장이 아니라 그 부분집합이다 — **channel로 부를 수 있는 Spot 하나와
> 그것을 부르는 client 코드**(.NET 기준 `SendToSpot` · `RequestToSpot` 두 endpoint). 나머지
> Actor·Session·STREAM은 2단계로 미룬다.

#### 2단계 이후

| 순위 | 작업 | 막는 것 |
| --- | --- | --- |
| ~~2~~ | ~~**네 언어 tutorial을 나머지까지 키운다** — Actor · Session · STREAM (§5.3)~~ | **끝났다.** 다섯 언어 모두 `connected / round trip / bound player / pushed` 네 줄을 찍는다 |
| ~~3~~ | ~~**§5.2의 7·8·9·10·11번 장을 쓴다**~~ | **끝났다.** 기능 층의 다섯 장(21·22·23·24·25)이 모두 있다. 12번(`26-monitoring`)만 §6.5의 결정에 걸려 있다 |
| 4 | **나머지 장 해체** — 02 · 06 · 07 · 12 (§6.2) | 3번 |
| 5 | **mkdocs nav 재배치** — 여기서 §5.2의 "자리만 옮긴다" 항목이 함께 끝난다 | 4번 |

1.1과 1.2는 지금 바로, 서로 독립으로 할 수 있다.

### 5.2 목차 순서와 각 항목의 상태

"**자리만 옮긴다**"는 **파일 이름을 바꾸지 않고 mkdocs nav의 구획만 옮기는 것**을 뜻한다.
층을 넘어가면서 번호대가 달라지는 장만 번호를 새로 받는다(`04`→`33`, `11`→`26`). 운영 층은
`01`~`19`를 그대로 쓴다(`12`·`16`).

| # | 층 | 장 | 파일 | 출처 | 상태 |
| --- | --- | --- | --- | --- | --- |
| 1 | 원리 | ZLink를 어디에 쓰나 | `17-alternative` | 17 그대로 | **동작 원리 끝으로 옮겼다.** 기술 선택 근거라 읽는 순서의 첫 자리가 아니다 |
| 2 | 시작 | 개요 | `01-overview` | 01 그대로 | 자리만 옮긴다 |
| 3 | 시작 | 핵심 개념 | `03-concepts` | 03 그대로 | 자리만 옮긴다 |
| 4 | 시작 | Quickstart | `<lang>/quickstart` | 02 §1 + 기존 퀵스타트 | **합쳤다.** 02는 지웠다. 공통 소스가 없는 언어별 문서다 |
| 6 | 기능 | Channel 메시징 | `20-channel-messaging` | 05 §1·2(최소형)·3·4 | **작성됨.** 자리 표시 4개 · `.en.md` 없음 |
| 7 | 기능 | Spot | `21-spot` | 06 §1·2·3·4 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 8 | 기능 | Actor | `22-actor` | 07 §1·2·3 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 9 | 기능 | STREAM | `23-stream` | 09 §1·2·5 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 10 | 기능 | Session과 Actor 연결 | `24-actor-session` | 08 §1·2·4 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 11 | 기능 | Location | `25-location` | 10 §5 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 12 | 기능 | 모니터링 | `26-monitoring` | 11 그대로 | **자리 표시만 있다.** 11은 언어별 문서이고 §6.5의 결정에 걸려 있다 |
| 13 | 원리 | Channel 패턴 | `30-channel-patterns` | 05 §6·8·9 | **작성됨.** 자리 표시 24개 |
| 14 | 원리 | Handler와 dispatch | `31-handler-dispatch` | 05 §5·7 | **작성됨.** 자리 표시 12개 |
| 15 | 원리 | 실행 모델 | `32-execution-model` | 06 §2.1 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 16 | 원리 | Backpressure | `33-backpressure` | 04 그대로 | **옮겼다.** 파일·링크·nav·읽는 순서 표를 다시 걸었다 |
| 17 | 원리 | 활성화와 수명 | `34-activation-lifetime` | 06 §1·4.2 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 18 | 원리 | Actor membership | `35-actor-membership` | 07 §4·5·6 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 19 | 원리 | Timer와 worker | `36-timer-worker` | 06 §6 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 20 | 원리 | Relocation | `37-relocation` | 06 §7 + 07 §7 + 12 §2 | **작성됨.** ko·en 두 벌, 자리 표시 0개 |
| 21 | 운영 | 운영과 lifecycle | `12-operations` | 12 §1·4·5·6 | **해체.** 번호 유지 |
| 22 | 운영 | 옵션 | `16-options` | 16 그대로 | 언어별 문서다. §6.5의 결정에 걸려 있다 |
| 23 | 샘플 | 샘플 고르기 | `14-samples` | 14 그대로 | 자리만 옮긴다 |
| 24 | 샘플 | E2E 테스트 | `15-e2e-testing` | 15 그대로 | 자리만 옮긴다 |
| 25 | — | 레퍼런스 | — | 현 `reference/` + 13 타입 색인 | 마지막 |
| 26 | — | 스펙 | — | 그대로 | 손대지 않는다 |

목차의 `메시지 filter`·`직렬화 codec`은 14번이, `연결 제어`·`Channel 수평 확장`은 13번이
흡수했다. 그래서 목차의 동작 원리 항목 넷이 두 장으로 합쳐진다.

`05-channel-messaging`은 6·13·14번이 받고 **삭제했다**.

> **흡수 확인에서 한 절이 걸렸다.** `05`의 "종결자"를 §5.2의 15번(`32-execution-model`)에
> 배정해 두었는데 그 장은 2단계 소속이라, §5.1이 1.5의 선행으로 적은 1.1·1.3만으로는 `05`를
> 지울 수 없었다. 작성 가이드 §1.2의 기준("이 문단을 빼면 독자가 코드를 실행하지 못하나")으로
> 보면 종결자는 기능 층이므로 **20번 §1.4로 옮겼다.** 15번은 실행 모델 자체만 담는다.

### 5.3 기능별 가이드는 tutorial 코드가 먼저 있어야 한다

기능별 가이드의 코드는 tutorial에서 읽는다(§4.1). 지금 **Spot·Actor·Session·STREAM
코드는 .NET tutorial에만 있다.**

| tutorial | 담는 것 | 마커 |
| --- | --- | --- |
| .NET | channel · Spot · Actor · Session · STREAM | 67 |
| C++ | channel 계열 · Spot · Actor | 48 |
| Java | channel 계열 · Spot · Actor | 49 |
| Kotlin | channel 계열 · Spot · Actor | 51 |
| Node | channel 계열 · Spot · Actor | 49 |

C++·Java·Kotlin·Node에는 `GetPlayerProfileHandler` · `RecordLoginHandler` ·
`IssueSessionTicketHandler` · `MaintenanceNoticeSubscriber` · `NodeStatusHandler` ·
`CallLogFilter`만 있다. .NET에만 `LobbySpot` · `GameRoom` · `Player` · `GameSession` ·
`MatchQueue`가 더 있다.

**어디까지 키우나.** .NET의 마커 61개를 그대로 복제하지 않는다. `guide-writing-guide.ko.md`
§4.1의 "tutorial은 기능마다 최소한만 담는다"를 따라, **그때 쓸 장이 인용할 코드까지만** 넣는다.
기준은 .NET tutorial의 해당 절이다 — 같은 기능을 같은 최소형으로 옮기고, .NET에만 있는
도메인 확장은 옮기지 않는다. 넣은 코드는 반드시 실행해 확인한다(§7.7).

**두 번에 나눠 넣는다.** Spot은 1단계에서 먼저 들어간다 — `20-channel-messaging` §3.7이 channel로 Spot을 부르는
코드를 인용하기 때문이다(§5.1의 1.2). Actor·Session·STREAM은 2단계에서 §5.2의 8~10번 장을
쓸 때 넣는다.

**동작 원리(13~20번)는 샘플에서 가져오므로 다섯 언어 모두 재료가 있다**(§4.1).

### 5.4 장 하나를 만드는 절차

1. **기능별 가이드면** 네 언어 tutorial에 그 장이 쓸 코드를 먼저 넣고 **실행해 확인한다**.
   구간 마커를 단다(§7.7).
2. `common/guide/server/`에 `.ko.md`를 쓴다. 다섯 언어 탭을 모두 둔다. 형태는 §5.5.
3. `.en.md`를 함께 쓴다. **없으면 사이트 메뉴에 뜨지 않는다**(§7.3).
4. 다섯 언어 README에 행을 넣는다 —
   `framework/doc/framework/{dotnet,cpp,java,kotlin,node}/guide/server/README.ko.md`
   생성기가 이 표에서 앞뒤 장 nav를 읽으므로 5단계보다 먼저 한다.
5. 생성기를 돌리고(§2.3) 렌더를 확인한다(§2.4).
6. 검사를 돌린다 — 저장소 검사기 셋(§7.3), 앵커(§7.4), 펜스(§7.6).
7. 완료 점검표를 돌린다 — `guide-writing-guide.ko.md` §7.

**mkdocs nav는 장마다 걸지 않는다.** 초안 동안에는 README 표만으로 도달하고, nav 재배치는
가리킬 파일이 다 생긴 뒤 한 번에 한다(§5.1 6번). 지금 `mkdocs.yml`은 20·30·31을 dotnet
아래에만 임시로 걸어 두었다.

### 5.5 Channel 메시징을 본으로 삼는다

이미 만든 세 장이 두 층의 형태를 정한다. 새 장은 이 뼈대를 따른다.

**기능별 가이드 — `20-channel-messaging.ko.md`**

````
# <장 제목>
!!! info "이 장을 읽고 나면"        ← 얻는 것 한두 문장 + 코드 출처 한 문장
<도입 산문 2~4줄>

## 1. <그 기능의 기본 계약>         ← 메시지 계약 → 받는 쪽 → 완료 시점
## 2. <종류 소개>                   ← 산문으로. 비교표를 두지 않는다
## 3. <종류 A>                      ← 종류마다 한 절
### 3.1 동작                        ← 다이어그램
### 3.2 받는 쪽 — <역할>            ← !!! success "받는 쪽 process"
### 3.3 호출하는 쪽 — <역할>        ← !!! info "호출하는 쪽 process"
### 3.4 호출
### 3.5 실행 결과                   ← 실제로 찍은 값
## 4. <종류 B>
## 5. <종류 C>
## 6. 관련 문서
````

핵심은 **한 절 안에서 그림과 코드가 이어지는 것**이다. 그림을 따로 모으지 않는다. 받는
쪽과 호출하는 쪽은 각각 한 절을 두고 소제목에 방향을 적는다.

**동작 원리 — `30-channel-patterns.ko.md`**

````
## 1. <패턴 비교>                   ← 여기서는 표를 쓴다
## 2. 물리 배선 — 무엇이 실제로 연결되는가
## 3. <방향·선택 같은 규칙>
## 4. <대상 선택>
## 5. <변형>
## 6. <연결과 discovery>
## 7. <경계와 실패>
## 8. 관련 문서
````

기능 층이 "기본값 하나"로 넘긴 것을 원리 층이 받는다 — 종류 비교표, 옵션, 실패 분기.

---

## 6. 지금 상태와 남은 작업

### 6.1 작성된 장

읽는 분량은 `guide-writing-guide.ko.md` §3.2의 셈법으로 센 값이다.

| 장 | 파일 | 읽는 분량 | 남은 자리 표시 탭 |
| --- | --- | --- | --- |
| Channel 메시징 (기능) | `20-channel-messaging.ko.md` | 254줄 / 상한 300 | **0개** |
| Spot (기능) | `21-spot.ko.md` | 106줄 / 상한 300 | **0개** |
| Actor (기능) | `22-actor.ko.md` | 110줄 / 상한 300 | **0개** |
| Location (기능) | `25-location.ko.md` | 65줄 / 상한 300 | **0개** |
| STREAM (기능) | `23-stream.ko.md` | 80줄 / 상한 300 | **0개** |
| Session과 Actor (기능) | `24-actor-session.ko.md` | 78줄 / 상한 300 | **0개** |
| Backpressure (원리) | `33-backpressure.ko.md` | 04를 옮긴 것이다 | **0개** |
| 실행 모델 (원리) | `32-execution-model.ko.md` | 62줄 / 상한 500 | **0개** |
| 활성화와 수명 (원리) | `34-activation-lifetime.ko.md` | 85줄 / 상한 500 | **0개** |
| Actor membership (원리) | `35-actor-membership.ko.md` | 91줄 / 상한 500 | **0개** |
| Timer와 worker (원리) | `36-timer-worker.ko.md` | 66줄 / 상한 500 | **0개** |
| Relocation (원리) | `37-relocation.ko.md` | 72줄 / 상한 500 | **0개** |
| Channel 패턴 (원리) | `30-channel-patterns.ko.md` | 320줄 / 상한 500 | **0개** |
| Handler와 dispatch (원리) | `31-handler-dispatch.ko.md` | 123줄 / 상한 500 | **0개** |

열네 장 모두 `.ko.md`와 `.en.md`가 있고, 다섯 언어 × 두 로케일로 생성되어 사이트에 뜬다.
§7.3의 검사기 셋과 §7.4·§7.6의 검사가 모두 통과한다.

자리 표시 탭은 `// 이 언어 탭은 아직 작성되지 않았다.` 한 줄로 되어 있다. 이 문자열로 센다.

### 6.2 해체 대기 중인 장

빈 줄을 뺀 파일 전체 줄 수다. 다섯 언어 탭을 모두 센 값이므로 §6.1의 "읽는 분량"과 셈법이
다르다.

| 장 | 줄 | 처리 |
| --- | --- | --- |
| ~~`05-channel-messaging.ko.md`~~ | ~~2,185~~ | **삭제했다.** 20·30·31이 받았다 |
| ~~`06-spot.ko.md`~~ | ~~2,066~~ | **해체했다.** §4.1은 `31`로, §5.1은 `30`으로 옮기고 나머지는 `21`·`32`·`34`·`36`·`37`이 소유한다. 파일 12개 삭제, 링크 90여 곳 재지정 |
| ~~`07-actor-spot.ko.md`~~ | ~~1,020~~ | **해체했다.** 산문 다섯 덩이를 `22`·`34`·`35`·`37`로 옮기고 지웠다. 파일 12개 삭제, 링크 17곳 재지정 |
| ~~`02-getting-started.ko.md`~~ | ~~940~~ | **끝났다.** 퀵스타트와 중복이라 §1을 그쪽으로 옮기고 지웠다 |
| ~~`12-operations.ko.md`~~ | ~~542~~ | **끝났다.** `37`이 소유하는 유지 항목 표와 일곱 단계 절차를 빼고 운영 호출만 남겼다. 329줄 |
| ~~`09-stream.ko.md`~~ | ~~537~~ | **해체했다.** 기능부는 `23`이 가졌고, 규칙부로 `38-stream-boundary`를 새로 썼다 |
| ~~`08-actor-session.ko.md`~~ | ~~411~~ | **해체했다.** 기능부는 `24`가 가졌고, 규칙부로 `39-session-binding`을 새로 썼다 |
| ~~`10-location.ko.md`~~ | ~~372~~ | **해체했다.** §0~3은 `30` §6.1~6.3, §4는 `12` §5, §5는 `25`가 가졌다 |
| ~~`11-monitoring`(언어별 5편)~~ | ~~141·119·138·68·125~~ | **합쳤다.** 다섯 판의 뼈대가 같아 공통 정본 `26-monitoring`으로 올렸다 |

**해체 대기가 비었다.** nav의 "재작성 대기" 구획이 사라졌다.

해체 대상에서 나오는 새 장의 예상 크기는 `GUIDE-STRUCTURE.md` §5가 소유한다.

### 6.3 언어별 tutorial

경로는 §4.1이, 담는 범위는 §5.3이 소유한다. **다섯 언어 모두 channel·Spot·Actor·Location·
Session·STREAM을 담는다.** C++·Java·Kotlin·Node에는 `StreamClient`가 별도 프로젝트로 붙었다.

Framework는 다섯 언어 모두 **0.16.0**이다. 버전 고정은 손으로 고치지 않는다 —
`scripts/local-package/sync-version.py --write`가 각 언어 `VERSION`에 맞춰 세 파일
(`dotnet/tutorial/Directory.Packages.props` · `java/tutorial/gradle/libs.versions.toml` ·
`node/tutorial/package.json`)을 갱신하고, `--check`가 CI에서 그것을 지킨다.

Java·Kotlin은 0.15.0부터 `ZLINK_LIBRARY_PATH` 없이 실행된다 — binding `zlink:1.2.1` jar가
`native/windows-x86_64/zlink.dll`을 함께 싣는다.

### 6.4 남은 작업

| 작업 | 내용 |
| --- | --- |
| ~~자리 표시 탭 36개 (13·14번 장)~~ | **끝났다.** C++·Java·Kotlin·Node 샘플에 `doc-` 마커 24개를 달아 채웠다 |
| ~~자리 표시 탭 4개 (`20-channel-messaging` §3.7)~~ | **끝났다.** 네 언어 tutorial에 Spot 최소형을 넣고 실행해 확인한 뒤 채웠다 |
| ~~`.en.md` 세 벌~~ | **끝났다.** 다섯 언어 `README.en.md`에 행을 넣고 생성했다. `20-*-en.html`·`30-*-en.html` 다이어그램 7개도 만들었다(archify) |
| ~~`05` 삭제~~ | **끝났다.** 흡수 확인에서 "종결자" 하나만 갈 곳이 없어 20번 §1.4로 옮겼다(아래). 그 뒤 파일 12개 삭제 · 링크 154곳 재지정 · `mkdocs.yml` nav 15줄 교체 |
| ~~`mkdocs.yml` nav 재배치~~ | **끝났다.** 다섯 언어 nav를 시작·기능별 가이드·동작 원리·운영·샘플과 테스트·레퍼런스 구획으로 다시 짰다. 아직 해체하지 않은 `06`~`11`은 **재작성 대기** 구획에 남겨 두어 본문이 닿지 않는 자리가 생기지 않게 했다 |
| ~~C++·Java·Kotlin·Node tutorial 확장~~ | **끝났다.** Spot·Actor·Location 조회·Session·STREAM을 네 언어에 모두 넣고 실행해 확인했다. 네 언어 README에 단계와 마커 표를 갱신했다 |
| CI | `.github/workflows/framework-tutorial.yml`이 이미 있고 **.NET 전용이다**(빌드 → 마커 존재 검사 → 문서에 적힌 전 단계 실행). 네 언어 job을 같은 형태로 더한다 |
| `11`·`13`·`16` 처리 결정 | §6.5 |
| ~~C++ 번호 충돌 해소~~ | **끝났다.** C++ 전용 장 넷을 `40`~`43`으로 옮기고 `GUIDE-STRUCTURE.md` §4와 작성 가이드 §6의 번호대 표에 `40`~`49` 행을 넣었다 |
| ~~0.16.0 이후 재검증~~ | **끝났다.** 0.16.0으로 올리고 다섯 언어를 모두 다시 실행해 값을 갱신했다. **C++의 one-way send만 아직 `NotFound`다**(§8) |
| ~~`04` → `33` 이동~~ | **끝났다.** 제목의 옛 장 번호를 떼고, 열 개 README의 읽는 순서 표와 mkdocs nav에서 원리 구획으로 옮겼다 |
| ~~.NET 문서 회귀 테스트~~ | **끝났다.** `Regression.cs`의 공통 장 목록이 `05` 삭제 이후로 어긋나 있었다. 목록을 트리에 맞추고, 다시 쓰는 층의 장은 소유 스펙 머리말을 두지 **않는다**는 것을 테스트가 지키게 했다 |
| 나머지 장 해체 | §6.2 |

### 6.5 작성 원칙 리뷰에서 남은 것

`documentation-principles.ko.md`·`guide-writing-guide.ko.md`로 20·30·31을 대조했다. 기계로
세는 항목은 모두 통과한다 — 머리말 형식, 다섯 언어 탭, snippet 참조, 앵커, 분량, 기능 층의
비교표 0개, 동작 원리 층의 다이어그램. 7.10의 개수 세기 위반 6곳은 고쳤고, 31번에 없던
다이어그램(`31-filter-scope`)을 만들어 넣었다.

**남은 것은 두 문서가 충돌하는 자리 하나다.**

`documentation-principles.ko.md` 7.9는 제목을 **명사구**로 달라고 하고, 흔한 이탈로
`값을 정하지 않으면 연결 수에 맞춰 계산한다` 같은 서술어 종결 제목을 든다. 그런데
`guide-writing-guide.ko.md` 6.3이 제시하는 본보기가 바로 그 형태다.

```markdown
### 3.2 받는 쪽 — handler를 등록한다
### 3.3 호출하는 쪽 — 같은 채널 이름으로 부른다
```

7.9의 예외는 "이 디렉터리의 원칙 문서와 작성 가이드"에만 적용되고 "가이드·spec·internals의
본문 제목에는 적용되지 않는다"고 못박혀 있으므로, 가이드 장의 제목은 7.9를 따라야 한다.
`guide-writing-guide.ko.md` 머리말도 "문장의 형태는 기술문서 작성 원칙이 소유한다"고 적는다.
**7.9가 이기고 6.3의 본보기가 틀렸다.**

지금 이 형태를 쓰는 제목은 다음과 같다.

| 장 | 제목 |
| --- | --- |
| `20-channel-messaging` | `받는 쪽 — mesh에 바로 등록한다` · `호출하는 쪽 — mesh에 연결되어 있으면 된다` |
| `30-channel-patterns` | `2.1 RouteMesh — 연결 하나를 여러 channel이 공유한다` · `3.1 RouteMesh — role 등록이 방향을 정한다` · `3.2 ClientServer — 방향이 고정된다` |

**고치지 않고 남겼다.** 6.3을 먼저 고쳐야 하고, 제목을 바꾸면 앵커가 함께 깨져 20·30을
가리키는 링크를 모두 다시 걸어야 한다. 두 문서 중 어느 쪽을 고칠지는 이 계획의 범위 밖이다.

### 6.5 언어별로만 있는 장과 번호 충돌

공통 소스에 없고 언어마다 따로 관리되는 장이 있다. 내용도 언어마다 다르다.

**다섯 언어 공통으로 있는 것** — 빈 줄을 뺀 줄 수다.

| 장 | dotnet | cpp | java | kotlin | node |
| --- | --- | --- | --- | --- | --- |
| `11-monitoring` | 141 | 119 | 138 | 68 | 125 |
| `13-interface-catalog` | 248 | 156 | 172 | 69 | 122 |
| `16-options` | 256 | 167 | 189 | 없음 | 175 |

공통으로 올릴지 그대로 둘지 정해야 한다. Kotlin은 세 장 모두 다른 언어의 절반 이하이고
`16-options`가 아예 없다. **§5.2의 12번(`26-monitoring`)과 22번(`16-options`)이 이 결정에
걸려 있다.** 공통으로 올리면 §5.4의 절차를 그대로 쓰고, 그대로 두면 다섯 언어 파일을 손으로
각각 고친다(§2.3).

`<lang>/quickstart.ko.md`도 공통 소스가 없는 언어별 문서다(§5.2 4번).

**C++에만 있는 것 — `40`번대로 옮겼다.**

```
cpp/guide/server/40-di-container.{ko,en}.md
cpp/guide/server/41-configuration.{ko,en}.md
cpp/guide/server/42-http-hosting.{ko,en}.md
cpp/guide/server/43-execution-model.{ko,en}.md
```

원래 `18`~`21`이었고 `20-http-hosting`이 `20-channel-messaging`과, `21-execution-model`이
§5.2의 7번(`21-spot`)과 겹쳤다. **C++ 전용 장 넷을 옮기는 쪽을 골랐다** — 기능별 가이드
번호대를 옮기면 이미 그 대역을 쓰는 20·30·31과 `GUIDE-STRUCTURE.md` §4 전체가 따라 움직인다.
`GUIDE-STRUCTURE.md` §4와 `guide-writing-guide.ko.md` §6에 `40`~`49`(한 언어에만 있는 장) 행을
넣었고, 참조 27개 파일을 다시 걸었다.

---

## 7. 알아 둘 함정

### 7.1 snippet은 가이드 worktree에서 해석된다

`mkdocs.yml`의 `pymdownx.snippets.base_path`가 `.`과 `../..`이고, mkdocs는
`zlink-guide/doc/site`에서 실행된다. 따라서 `--8<-- "framework/languages/…"`는
**`D:\project\zlink-guide` 아래**에서 찾는다. tutorial과 샘플에 마커를 달 때 그 worktree에
단다 — 다른 worktree에 달면 파일은 있는데 마커가 없는 상태가 된다.

### 7.2 `.en.md`가 없으면 nav에 뜨지 않는다

`mkdocs-static-i18n`을 suffix 방식으로 쓰고 기본 로케일이 `en`이다. `.en.md`가 없는 장은
nav가 해석하지 못해 404가 된다. 빌드해 보면 `site/ko/<lang>/guide/server/<장>/`만 생기고
`site/<lang>/guide/server/<장>/`은 생기지 않는다. 초안 동안에는 README 표만으로 도달한다.

### 7.3 장을 마칠 때 돌리는 검사

**저장소가 가진 검사기를 먼저 쓴다.** 임시 스크립트보다 넓게 본다 — 다섯 언어 생성판과
`.en.md`까지 포함해 문서 939개·스니펫 440개·탭 그룹 289개를 본다.

```bash
cd /d/project/zlink-guide
PYTHONIOENCODING=utf-8 python doc/site/scripts/check_doc_tabs.py framework
PYTHONIOENCODING=utf-8 python doc/site/scripts/check_doc_links.py framework
PYTHONIOENCODING=utf-8 python doc/site/scripts/check_guide_identifiers.py
```

| 검사기 | 잡는 것 |
| --- | --- |
| `check_doc_tabs.py` | 다섯 언어 탭 누락, `--8<--` 경로·마커 미해석, 언어↔확장자 불일치 |
| `check_doc_links.py` | 상대 링크가 저장소의 실제 파일을 가리키는지 |
| `check_guide_identifiers.py` | 탭 코드가 실재하지 않는 메서드 이름을 쓰는지 |

`check_doc_tabs.py`가 스니펫 경로·마커 검사를 대신하므로 같은 일을 하는 임시 스크립트는 두지
않는다. 대응 검사기가 없는 것은 앵커(§7.4)와 중첩 펜스(§7.6) 둘뿐이다.

### 7.4 제목을 고치면 앵커가 조용히 깨진다

절 참조는 `(§3.7)` 같은 맨 번호가 아니라 앵커 링크로 쓴다. 앵커 id는 `mkdocs.yml`이 지정한
`pymdownx.slugs.slugify(case="lower", unicode=True)`가 만든다. **직접 짐작하지 말고 그
함수로 만들어 본다.** 규칙이 눈에 띄지 않는 자리가 있다.

| 제목 | 앵커 |
| --- | --- |
| `3.7 Spot·Actor 호출하기` | `#37-spotactor-호출하기` — `·`는 사라지고 하이픈을 남기지 않는다 |
| `3.2 받는 쪽 — channel을 담당하는 node` | `#32-받는-쪽--channel을-담당하는-node` — `—` 자리에 하이픈이 **둘** |

**`framework/doc/framework/common/guide/server/`에서** 돌린다.

```bash
cd /d/project/zlink-guide/framework/doc/framework/common/guide/server
python - <<'PY'
import pathlib, re
from pymdownx.slugs import slugify          # mkdocs.yml 이 쓰는 것과 같은 함수

slug = slugify(case="lower", unicode=True)

for f in sorted(pathlib.Path(".").glob("*.md")):
    body, infence, fence = [], False, None
    for l in f.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(`{3,})", l)
        if m:
            if not infence:
                infence, fence = True, m.group(1)
            elif len(m.group(1)) >= len(fence):
                infence, fence = False, None
            body.append("")
            continue
        body.append("" if infence else re.sub(r"`[^`]*`", "", l))
    anchors = {"#" + slug(l.split(" ", 1)[1], "-") for l in body if l.startswith("#")}
    text = "\n".join(body)
    bad = [a for a in re.findall(r"\]\((#[^)]+)\)", text) if a not in anchors]
    if bad:
        print(f.name, bad)
PY
```

### 7.5 `05` 삭제는 참조 정리를 동반한다

파일만 지우면 링크가 깨진다. 공통 소스 `.ko.md`만 세어도 **9개 파일 43곳**이 참조한다.

| 파일 | 참조 수 |
| --- | --- |
| `01-overview.ko.md` | 22 |
| ~~`02-getting-started.ko.md`~~ | 지웠다. 참조는 `<lang>/quickstart`로 옮겼다 |
| `03-concepts.ko.md` · `33-backpressure.ko.md` · `14-samples.ko.md` · `17-alternative.ko.md` | 각 3 |
| `30-channel-patterns.ko.md` | 2 |
| `06-spot.ko.md` · `10-location.ko.md` | 각 1 |

여기에 같은 수의 `.en.md`와 다섯 언어 생성판이 더 붙는다. 그 밖에 `mkdocs.yml` nav 5곳,
`SPEC.md` 1곳이 있다. 정리한 뒤 §7.3의 `check_doc_links.py`로 확인한다.

### 7.6 중첩 코드 펜스

백틱 세 개로 연 `markdown` 블록 안에 같은 길이의 펜스를 넣으면 바깥이 먼저 닫히면서 뒤의
산문을 삼킨다. 바깥을 **백틱 네 개**로 연다. 실제로 이 사고로 작성 규칙 문서에서 절 하나가
통째로 코드 블록 안에 갇힌 적이 있다.

렌더해서 확인한다.

```bash
cd /d/project/zlink-guide/framework/doc/framework/common/guide/server
python - <<'PY'
import markdown, pathlib, re, sys

md = markdown.Markdown(extensions=["tables", "toc", "fenced_code"])
for f in sorted(pathlib.Path(".").glob("*.md")):
    h = md.convert(f.read_text(encoding="utf-8")); md.reset()
    t = re.findall(r"<code[^>]*>((?:(?!</code>).)*?#{2,3} [^\n<]+)", h, re.S)
    if t:
        print(f.name, "코드 블록에 갇힌 제목", len(t), "개")
PY
```

### 7.7 tutorial을 실행할 때

- **Windows에서 콘솔 창을 띄우지 않는다.**
  `Start-Process -NoNewWindow -RedirectStandardOutput <log> -RedirectStandardError <log>`를
  쓴다. Git Bash의 `... &`로 detach하면 창이 뜬다. 서브에이전트에게 일을 맡길 때 이 규칙을
  프롬프트에 함께 넣는다.
- 전제·포트·Redis 필요 여부는 §2.2의 표에 있고, 정본은 각 언어 tutorial README다.
- **Redis가 필요한 것은 .NET tutorial의 Spot·Actor 단계뿐이다.** 주소는 `127.0.0.1:6379`,
  키 prefix는 `zlink-tutorial`(`dotnet/tutorial/Server/Program.cs`). 강제 종료 뒤 RID claim이
  충돌하면 `zlink-tutorial*` 키를 지우고 다시 띄운다. Channel 메시징까지는 다섯 언어 모두
  Redis 없이 돈다.
- 빌드 전에 이전 실행이 남은 process를 정리한다. 실행 중이면 DLL이 잠겨 빌드가 막힌다.
- **Kotlin 실행 script는 Windows에서 실패한다.** `installDist`가 만든 `Server.bat`·`Client.bat`이
  classpath 길이로 cmd의 8191자 상한을 넘어 "The input line is too long"을 낸다.
  `java -cp "<install>/lib/*" <main class>`로 직접 띄운다. 이 문제는 아직 이슈로 걸려 있지
  않다.

---

## 8. 이 계획과 함께 진행 중인 것

**튜토리얼의 오류 응답 통일.** channel weight를 0으로 두면 담당 node가 후보에서 제외되어
호출이 실패한다. C++은 zlink가 HTTP server를 직접 소유하므로 프레임워크가 error kind를
상태코드로 옮긴다. **나머지 네 언어의 HTTP는 zlink 것이 아니다** — ASP.NET Core·Spring
Boot·NestJS의 것이다. 그래서 zlink가 상태코드를 정할 자리가 아니고, **매핑은 애플리케이션이
쓴다.** 네 언어 tutorial에 각각 넣었다.

> 이 매핑을 zlink 통합 패키지(`Zlink.Framework.AspNetCore` 등)로 올리지 않는다. 남의 HTTP
> 계층의 응답 형태를 zlink가 정하는 일이 된다.

| ErrorKind | HTTP |
| --- | --- |
| ProtocolError · TypeMismatch · InvalidOperation | 400 |
| NotFound | 404 |
| AlreadyExists | 409 |
| Rejected | 403 |
| NotConfigured · Unavailable · ShuttingDown | 503 |
| DeadlineExceeded | 504 |
| 그 밖 | 500 |

본문은 `{"error": "<이름>", "message": "<예외 메시지>"}` 형태이고, 이름은 `not_found`처럼
소문자 snake_case다. 기준은 C++ 프레임워크의
`framework/languages/cpp/framework/src/runtime/http/http_request_pipeline.cpp`에 있는
`status_for_error`와 `error_kind_name`이다.

가이드 작성과 파일이 겹치지 않는다. 다만 이 작업이 tutorial에 `error-mapping` 마커를
만들었으므로, 실패 응답을 다루는 장은 그 마커를 인용한다. 마커는 네 언어 Client에 모두 있다.

**정해졌다 — 후보가 비었을 때의 error kind는 `Unavailable`이다**
([#498](https://github.com/zlink-systems/zlink/issues/498), framework 0.16.0). 담당 node가
하나뿐이고 그 weight가 0이면 request와 one-way send가 **둘 다** `Unavailable`로 끝난다. 송신
경로와 연결은 그대로 있고 고를 대상만 없다는 뜻이라 `NotFound`가 아니다. 원인 분석과 언어별
옛 관측값은 #498이 소유한다. 여기서 다시 적지 않는다.

**다섯 언어를 0.16.0으로 올려 다시 실행했다**(2026-09-17). 네 언어는 request와 send가 모두
503 `unavailable`이다.

> **C++의 one-way send만 아직 `NotFound`(404)다.** #498이 고친 자리는
> `framework/languages/cpp/framework/src/runtime/channels/channel_outbound_exchange.cpp`의 두
> 곳인데, RouteMesh channel의 one-way send는 그 경로를 지나지 않는다 —
> `framework/src/runtime/host/app.cpp`의 `one_way_native_submit_result`가 `not_found`를 그대로
> `NotFound`로 옮긴다. C++의 request는 같은 단계에서 `Unavailable`이므로 **한 언어 안에서 두
> 호출이 어긋난다.** 0.16.0의 릴리스 노트와 C++ 단위 테스트가 모두 둘이 같아야 한다고 적고
> 있으나, 그 테스트는 매퍼를 직접 부르고 이 호출 자리를 지나지 않아 CI가 잡지 못했다.
>
> 관측값은 `30-channel-patterns.ko.md`와 C++ tutorial README에 적어 두었다. **이슈는 아직
> 열지 않았다.**

### 8.1 함께 드러난 것

| 항목 | 내용 |
| --- | --- |
| node-direct 오류 | `GET /ops/nodes/<없는 id>/status`가 **다섯 언어 모두 404**다. 매핑을 넣기 전에는 500이었다. .NET은 2026-09-17에 실행해 확인했다 — `{"error":"not_found","message":"Route channel 'game' does not know node 'no-such-node' for packet 'GetNodeStatus'."}`. 다섯 README와 CI(`framework-tutorial.yml`)를 모두 404로 고쳤다 |
| Server admin endpoint | 매핑을 Client에만 넣었으므로 admin 오류는 host 기본 500 그대로다 |
| Kotlin 실행 script | §7.7 |

**Java·Kotlin binding의 `boundSession()`은 묶인 연결이 없을 때 예외를 던진다**(2026-09-17).
Session·STREAM을 네 언어에 넣으면서 관측했다. 같은 호출이 .NET·C++·Node에서는 아무 일도 하지
않고 끝난다. tutorial의 `ChangeNickname` handler는 STREAM으로도 mesh HTTP로도 불리므로, 후자에는
묶인 연결이 없다 — JVM 두 언어에서만 그 호출이 handler 예외로 끝났다. 실패가 **동기 예외**라
`CompletionStage.exceptionally`로는 잡히지 않고 `try`/`runCatching`이 필요하다.

> tutorial은 그 실패를 잡아 두었고, 두 README가 이유를 적는다. **이슈는 아직 올리지 않았다.**
> 어느 쪽이 정본인지는 스펙이 정할 일이다 — 지금 스펙에 "묶인 연결이 없을 때"의 결과가 없다.

## 9. 작성 원칙 리뷰 (2026-09-17)

`documentation-principles.ko.md`와 `guide-writing-guide.ko.md`를 기준으로 읽기 전용
서브에이전트 여섯을 돌려 가이드 전체를 대조했다.

**앞서 적어 둔 "7.9와 작성 가이드 6.3이 충돌한다"는 틀렸다.** 원칙 7.9 본문에
`주제 — 결론` 형태를 허용하는 조항이 있다 — 앞이 명사구이면 뒤에 서술어로 끝나는 결론을
붙일 수 있다. 6.3의 예시 `### 3.2 받는 쪽 — handler를 등록한다`는 그 허용 형태다. 충돌은
없고, 의문형만 예외 없이 금지된다.

### 9.1 반영한 것

| 원칙 | 내용 | 대상 |
| --- | --- | --- |
| 7.7 | 2단계 표가 명시한 비유 낱말 — `부르는 쪽`·`갈린다`·`갈래`·`민다`·`들고 있다`·`붙는다` 등 | 새 장 14개 · `17` · `03` · `01` |
| 7.9 | 의문형·문장형·개수가 든 제목을 명사구나 `주제 — 결론`으로 | 한글 33개 · 영어 17개 |
| 7.10 | 바로 뒤 목록·표를 세어 둔 수와 단정 강조 | 30여 곳 |
| 7.8 | 자기 평가어 | `17` 6곳 · `01` 4곳 |
| 7.1 | 한글 음차를 영어로(`프로세스`→`process` 등) | `17` 43곳 |
| 11.2 | `!!! success`는 허용 표기가 아니다 → `!!! note`. blockquote 곁가지를 `!!!`로 | 17곳 |
| 11.3 | 두 행짜리 비교표를 산문으로 | 4곳 |
| 4.4 | 제품 규칙을 말하는 자리의 `방`을 `User Spot`으로 | 8곳 |
| 4.2 | 코드 출처를 `TicTacToe 샘플`·`Bingo 샘플`로 | 5곳 |
| 1.4 · 6.6 | 독자를 스펙으로 내보내는 문장 삭제 — "spec이 우선이다" 5줄, `spec/server` 링크 | `01` 10줄 · `03` 4곳 |
| 6.1 | 머리말을 `!!! info "이 장을 읽고 나면"` 형식으로 | `02` · `03` · `17` |

머리말을 새 형식으로 바꾼 장은 `Regression.cs`의 `RewriteLayerGuideDocuments`에 넣었다.
그 목록의 장은 "이 장의 계약 소유 문서" 머리말을 두지 **않는** 것을 테스트가 지킨다.

### 9.2 남은 것 — 재작성·재배치에 걸려 있다

| 항목 | 무엇에 걸려 있나 |
| --- | --- |
| ~~`01-overview` 523줄을 상한 안으로~~ | **철회했다.** 계획 146·305행이 정한 "01 그대로"를 따른다. §15.1 |
| ~~`01`의 `조립` 비유 19곳~~ | **끝났다.** 뜻을 유지한 채 `직접 구성한다` 등으로 바꿨다. `check_register` 통과 |
| `01`·`03`의 손으로 적은 코드를 snippet 참조로 | tutorial에 구간 마커를 다는 작업이 먼저다 |
| `03`의 channel 종류 비교표를 `30-channel-patterns`로 이동 | 목적지 절 번호가 확정된 뒤 |
| `35`·`36`·`37`에 다이어그램 | 작성 가이드 5.1이 원리 층에도 요구한다 |
| `06`·`07`의 절을 `32`·`34`·`35`·`36`·`37`로 이동 | 두 장이 각각 2,569줄·1,308줄이다. 이 이동이 상한을 맞추는 작업과 같다 |
| 볼드 범위(10.1 #9) | `01` 45곳, `30` 62곳. 일괄 판정 대상이 아니라 문단별로 본다 |

### 9.3 계획 자체에 남는 것

작성 가이드 §3.1의 분량 상한 표에 **시작 층 행이 없다.** 기능 층 300줄과 원리 층 500줄만
있다. 시작 구획이 목차에 생겼으므로 그 층의 상한을 정해 §3.1에 넣는다.

## 10. 02-getting-started와 Quickstart 합치기 (2026-09-17)

두 장이 같은 것을 다뤘다 — 설치 명령, 두 process가 서로 호출하는 최소 예제, build와 실행.
02는 손으로 적은 다섯 언어 탭이었고, 퀵스타트는 실제로 build되는
`framework/languages/<lang>/quickstart/` project를 스니펫으로 읽었다. 작성 가이드 4.1이
코드 출처를 정하므로 검증된 쪽을 정본으로 남겼다.

### 10.1 옮긴 것

| 02의 절 | 간 곳 |
| --- | --- |
| §1 설치 — 패키지 조합, 선택 패키지 표, toolchain, 라이선스 | 각 언어 퀵스타트 §1. 탭이 아니라 그 언어의 값만 남는다 |
| §2 첫 실행이 안 될 때 확인할 항목 | 각 언어 퀵스타트의 점검 절. 언어마다 증상 행을 더했다 |
| §3 다음으로 읽을 것 | 각 언어 퀵스타트의 마지막 절 |
| §2 최소 예제·§3 build와 실행(손으로 적은 판) | 퀵스타트가 이미 스니펫으로 담고 있어 버렸다 |

퀵스타트 열 편의 머리말도 `!!! info "이 장을 읽고 나면"` 형식으로 바꾸고, 독자를 스펙으로
내보내던 "이 장의 계약 소유 문서" blockquote를 뺐다(작성 가이드 1.4·6.6).

### 10.2 따라 고친 것

- `mkdocs.yml` 다섯 언어의 시작 구획에서 "Install and first run" 행 제거
- `framework/doc/framework/index`·`install`, 공통 `01-overview`, 언어별 `guide/server/README`,
  `13-interface-catalog`(Java·Node), `42-http-hosting`(C++)의 링크를 퀵스타트로
- `14-samples`의 "02 장이 이 샘플을 따라간다" — 옛 940줄 판의 사실이라 삭제
- `Regression.cs`의 `CommonGuideDocuments`·`RewriteLayerGuideDocuments`에서 02 제거
- 언어별 README의 "01 · 02 · 11 · 13 · 16장은 전용으로 쓴다"에서 02 제거

### 10.3 남은 것

`framework/doc/framework/install.ko.md`도 언어별 설치 명령과 등록 코드를 탭으로 담는다.
그 문서의 .NET 탭에는 아직 `dotnet add package Zlink` 줄이 남아 있다. 이 문서가
플랫폼·저장소·stream connector 패키지만 다루고 언어별 설치는 퀵스타트에 맡기도록 정리한다.

## 11. 07-actor-spot 해체 (2026-09-17)

07의 코드는 목적지 세 장이 이미 같은 스니펫 마커로 싣고 있었다(`doc-entry-spot`은 `34`,
`doc-join-defer`는 `35`, `doc-relocation-adapter`는 `37`). 빠진 것은 산문이었다.

| 옮긴 것 | 간 곳 |
| --- | --- |
| §1 relocation policy 세 가지 | `37` §2.1. `.NET` 메서드 이름 대신 원시 이름으로 다시 썼다(원칙 4.4) |
| §2 actor ref — 세대와 조회 당시 경로 | `22` §3의 `!!! note` |
| §3 Entry Spot에 Actor별 상태를 두지 않는다 | `34` §3.1 |
| §3 Actor 파기 — Entry Spot에서만, callback 재실행 없음 | `34` §3.2 |
| §5 결과를 받는 자리 — 받아들여짐은 간 쪽, 거절은 떠난 쪽 | `35` §2.3 |
| §6 Message Follow와 이동 중 request 완료 | `35` §6.1 |

나머지 절은 목적지에 이미 같은 내용이 있어 버렸다 — §1 등록·§2 만들기·§6 메시징은 `22`가
tutorial 코드로 검증한 판을 가지고 있고, §3 callback 표는 `34` §2가 넷에서 아홉으로
넓혀 놓았으며, §4·§5는 `35` §1~§5가 덮는다.

### 11.1 따라 고친 것

- `mkdocs.yml` 다섯 언어의 재작성 대기 구획에서 07 행 제거
- 공통 정본 12편, `bindings/doc/guide/{dotnet,java}/index`, `doc/README.ko.md`,
  `dotnet/guide/server/16-options`의 링크 재지정
- 언어별 `guide/server/README`의 읽는 순서 표에서 07 행 제거
- `Regression.cs`의 `CommonGuideDocuments`에서 07 제거
- **`generate_language_guides.py` 수정** — 읽는 순서 표가 `guide/server` 밖의 문서를
  담을 수 있게 됐는데(퀵스타트) 제목을 못 읽어 앞뒤 링크 라벨에 경로가 그대로 찍혔다.
  상대 경로를 먼저 풀도록 고쳤다.

## 12. 남은 해체를 모두 끝냈다 (2026-09-17)

### 12.1 옛 장 다섯 편

| 장 | 줄 | 어떻게 처리했나 |
| --- | --- | --- |
| `08-actor-session` | 411 | 기능부는 `24`가 이미 가졌고, 규칙부(개수·경로 갱신·끊김 통지·실패 표)로 **`39-session-binding`을 새로 썼다** |
| `09-stream` | 537 | 기능부는 `23`이 이미 가졌고, 규칙부(등록 검증·오류 귀속·응답 token·실행 방식)로 **`38-stream-boundary`를 새로 썼다** |
| `10-location` | 372 | §0·1·2·3은 `30` §6.1~6.3으로, §4 운영 조회는 `12` §5로, §5는 `25`가 이미 가졌다 |
| `11-monitoring` | 언어별 5편 | 다섯 판의 뼈대가 같아 **공통 정본 `26-monitoring`으로 합쳤다.** 언어 차이는 탭으로 담긴다 |
| `07-actor-spot` | 1,020 | §11이 적었다 |

재작성 대기 구획이 비어 nav에서 사라졌다.

### 12.2 새 장 셋

| 장 | 층 | 출처 |
| --- | --- | --- |
| `38-stream-boundary` | 원리 | `09` §1·2·3·5 |
| `39-session-binding` | 원리 | `08` 머리말·§1·2·3·5 |
| `26-monitoring` | 기능 | 언어별 `11` 다섯 편 |

### 12.3 분량

| 장 | 이동 전 | 이동 후 | 상한 |
| --- | ---: | ---: | --- |
| `01-overview` | 519 | ~~196~~ → **1,629** | 시작 250 — **초과를 받아들인다**(§15.1) |
| `03-concepts` | 203 | **218** | 시작 250 |
| `17-alternative` | 222 | **524** | 기술 선택 — 상한 없음 |
| `12-operations` | 360 | **329** | 운영 400 |

`01`의 §2(네 가지 상황 308줄)를 `17`로, §7(이름 표기 규칙)을 `03`으로 옮겼다. `12` §2에서
`37`이 소유하는 유지 항목 표와 일곱 단계 절차를 뺐다.

**작성 가이드 §3.1에 층별 상한 표를 다시 썼다.** 시작 250 · 기능 300 · 원리 500 · 운영 400 ·
기술 선택과 색인은 상한 없음이다. 값마다 근거를 한 칸에 적었다.

### 12.4 다이어그램

작성 가이드 5.1이 원리 층에도 요구하는 그림을 만들었다 — `35-actor-join`,
`36-timer-worker`, `37-relocation-move`를 한글·영문 두 벌씩 여섯 편이다. `35`는 승인 화살표가
예약 박스를 가로질러 검증에 걸려, 그 관계를 그림에서 빼고 카드로 옮겼다.

### 12.5 남은 것

| 항목 | 내용 |
| --- | --- |
| `01`의 `조립` 비유 | 장 전체의 서술 축이라 낱말 치환이 아니라 프레임을 다시 잡아야 한다 |
| `01`·`03`의 손으로 적은 코드 | snippet 참조로 바꾸려면 tutorial에 구간 마커를 먼저 단다 |
| `13`·`16`의 공통 정본 승격 | `11`은 합쳤다. 이 둘은 언어마다 표면 이름이 달라 판단이 남았다 |
| 볼드 범위(원칙 10.1 #9) | `01` 45곳, `30` 62곳. 문단별로 본다 |

## 13. 두 번째 작성 원칙 리뷰 (2026-09-17)

§9의 리뷰 이후에 새로 쓰거나 고친 것은 검토되지 않은 상태였다. 읽기 전용 서브에이전트 넷을
네 갈래로 돌려 대조했다 — 새 장 셋(`26`·`38`·`39`), 절이 옮겨 온 장 여섯(`22`·`34`·`35`·`37`·
`30`·`12`), 재배치한 셋(`01`·`03`·`17`), 합친 퀵스타트 열 편이다.

### 13.1 사실 오류 — 퀵스타트

문서가 적은 값이 근거 파일과 어긋난 것이 여섯이었다. 전부 근거를 다시 확인하고 고쳤다.

| 무엇 | 문서가 적던 값 | 근거 파일의 값 |
| --- | --- | --- |
| Java·Kotlin의 게시 버전 | 0.12.0 | `libs.versions.toml:3` `zlinkFramework = "0.16.0"` |
| C++ Conan 버전 | `zlink-framework/0.14.0` | `packaging/conan/conanfile.py:16` `version = "0.16.0"` |
| C++ MessagePack codec target | `zlink::framework_codec_..._msgpack` | `cpp/CMakeLists.txt:667` `..._messagepack` |
| C++ stream connector target | 이름 없이 "stream connector" | `cpp/CMakeLists.txt:474` `zlink::stream_connector` |
| .NET handler 등록 설명 | `AddHandlersFromAssemblyOf`가 찾는다 | `quickstart/Server/Program.cs:16-19` — 이 예제에는 쓰이지 않는다 |
| Kotlin keep-alive | `SpringApplication.setKeepAlive(true)` | `ServerApplication.kt:38` `app.isKeepAlive = true` |

**C++ 설치 경로는 더 큰 문제였다.** 문서가 vcpkg·Conan·GitHub Release 셋을 같은 값으로 제시했는데,
프로젝트의 `README.md`가 셋을 끝까지 돌려 본 결과 vcpkg와 Conan은 결함이 남아 설치되지 않는다.
§1을 다시 써서 GitHub Release만 확인된 경로로 적고, 나머지 둘에 `!!! warning`을 달았다.

### 13.2 작성 원칙

| 원칙 | 무엇을 고쳤나 |
| --- | --- |
| 7.1 | 한글 음차를 영어로 — `프레임워크`·`프로세스`·`클라이언트`·`노드`·`큐`·`런타임`·`아티팩트`·`타깃`·`패키지` 등 14개 파일 |
| 7.7 | `조립` 비유 23곳(`01`·`17`), `부르는 쪽`·`갈래`·`가져간다`·`떠안는다`·`만지다`·`밀어낸다`·`껍데기`·`내놓는다`, 영문의 `bolted on`·`gives up`·`pushes out of the way`·`ships`·`demands` |
| 7.8 | `충분히`, `just fine`, `significantly` |
| 7.9 | 제목의 개수(`세 갈래`·`네 가지 상황`·`Four Integration Axes`), 문장형 영문 제목 셋 |
| 7.10 | 바로 뒤 목록·표를 세어 둔 문장 15곳 |
| 10.1 #9 | 문장 전체에 건 볼드 6곳 |
| 11.3 | 두 항목 비교표 둘을 산문으로 |
| 4.4 | 공통 정본에 남은 `System.Diagnostics.Metrics.Meter`·`AddRouteMesh`, `26`의 탭이 언어마다 다른 대상을 보이던 것 |
| 1.4 · 6.6 | **`01`의 스펙 유출** — 소유 스펙 머리말, 다섯 탭의 스펙 링크, 읽는 순서의 스펙 항목. 영문판에만 다섯 번 있던 `If the two disagree, the spec wins.`는 1.4가 이름을 들어 금지한 문장이다 |
| 6.1 | `01`에 `!!! info "이 장을 읽고 나면"`이 없었다. `26`의 머리말에 코드 출처 한 줄 |
| 6.4 | 장 안을 맨 절 번호로 가리키던 곳과, 옛 장 번호가 링크 글자로 남은 곳 |
| 5.1 · 5.2 | `26`의 mermaid 블록을 iframe으로, `38`·`39`에 없던 다이어그램을 만들어 붙였다 |

`01`이 소유 스펙 머리말을 두지 않게 되어 `Regression.cs`의 `RewriteLayerGuideDocuments`에 넣었다.

### 13.3 절 번호 정합성

`01` §2를 `17`로 옮기면서 절 번호가 어긋난 참조가 아홉 곳 있었다. `17`이 자기 문서를
`17장 §6`·`§5.1`처럼 옛 번호로 가리키던 것을 포함해 전부 앵커 링크로 바꿨다.

### 13.4 검사기 버그 하나

`humanize_guard.py`가 Windows에서 **언제나 통과했다.** `subprocess`에 encoding을 주지 않아
`git show` 출력을 로캘 코덱으로 디코드하다 실패하고, 그 예외가 reader thread 안에서 나므로
모든 파일이 "신규 파일"로 건너뛰어졌다. `encoding="utf-8"`을 고정했다.

### 13.5 리뷰 지적 중 반영하지 않은 것

- **퀵스타트의 저장소 링크가 사이트에서 깨진다** — 실제로는 `repo_links` 플러그인이 빌드할 때
  GitHub URL로 바꾼다. 빌드 로그가 "대상이 없는 링크 0건"을 보고하고, 생성된 HTML이
  `https://github.com/zlink-systems/zlink/tree/main/...`을 담는다.
- **볼드 범위를 일괄로 줄이기** — 저장소의 원칙 문서 자신이 문단 첫 문장 전체에 볼드를 쓴다.
  일괄 판정 대상이 아니라 문단별로 본다.
- **`26`의 코드를 snippet 참조로** — 관측 표면의 최소 호출이라 tutorial이나 샘플에 대응하는
  코드가 없다. tutorial에 관측 코드를 넣는 것은 작성 가이드 4.1이 경계하는 방향이다.

## 14. 문장 다듬기와 표현 (2026-09-17)

§13의 리뷰를 반영한 뒤, 읽는 사람이 문서를 보며 표현을 하나씩 짚었다. 그 지적이 모두 원칙
7.7의 2단계 표에 이미 이름이 올라 있는 낱말이었다.

| 지적받은 표현 | 고친 것 | 7.7 표의 대체어 |
| --- | --- | --- |
| `함께 끌어온다` | 전이 의존으로 **포함한다** | `도입한다` |
| `쓸 때만 더한다` | 필요할 때 **추가하는** | `추가한다` |
| `쓴다` | **사용한다** | — (7.7은 허용하나 이 저장소가 `사용한다`로 정했다) |
| `올라간다` | **구성된다** | — |
| `고른다` | **선택한다** | — |
| `서로 부른다` | 서로 **호출한다** | `호출한다` |

### 14.1 구멍은 원칙이 아니라 리뷰 방식에 있었다

`끌어온다`는 7.7의 표가 이름까지 들어 금지하고 대체어를 적어 둔 낱말이다. 그런데도 문서에
남아 있었다 — **리뷰를 파일 목록으로 돌렸기 때문이다.** 목록에 없던 파일에서 같은 낱말이
살아남았고, 사람이 읽어 잡는 방식으로는 이 구멍이 계속 생긴다.

그래서 검사기를 만들어 남겼다.

```
doc/site/scripts/check_register.py
```

7.7의 두 표가 이름을 든 낱말 서른 몇 개를 목록으로 들고 한글 산문 402편을 훑는다. 낱말마다
대체어를 함께 찍는다. 코드 블록·인라인 코드·링크 타깃은 보호하고, `덮어쓰기`처럼 뜻이 다른
복합어와 `"~라고 부른다"`(이름 붙이기)는 대상에서 뺀다. 원칙 문서와 작성 가이드는 이 낱말을
예시로 실으므로 제외한다. `--all`을 주면 spec·internals·e2e까지 본다.

### 14.2 `쓴다` → `사용한다` 일괄 적용

한글 문서 **392개 파일, 1,891줄**을 바꿨다. 위험이 둘 있었고 둘 다 처리했다.

- **보호 토큰** — 코드·식별자·링크 타깃·앵커가 함께 바뀌면 빌드가 깨진다. 치환기가 그
  구간을 통째로 보호했고, `humanize_guard.py`로 확인했다.
- **제목이 바뀌며 깨진 앵커** — `## 8. 라이선스 — 쓰는 데 드는 비용`처럼 제목 안의 낱말이
  바뀌면 그 앵커를 가리키던 링크가 전부 깨진다. 다섯 종을 찾아 가리키는 쪽을 함께 고쳤다.

### 14.3 install 문서

읽는 사람이 "framework 패키지와 client stream connector의 구분이 밋밋하다"고 지적했다.

| 고친 것 | 내용 |
| --- | --- |
| 지원 범위를 앞에 | "만드는 것 → 설치하는 것 → 지원 범위" 표를 문서 머리에 두었다 |
| connector를 탭으로 | framework 절만 탭이고 connector는 표여서 대비가 컸다. 엔진·빌드 타깃별 다섯 탭으로 바꾸고 각 탭에 실제 설치 명령을 넣었다 |
| 절 제목에 색 띠 | 서버 쪽 teal, client 쪽 amber, 부가 구획 회색. `extra.css`의 `.zlink-band` |
| 아이콘 | `pymdownx.emoji`를 켜고 제목에 `:material-server:` 계열을 달았다 |
| 다이어그램 | 처음엔 넣었으나 바로 아래 표와 같은 내용을 두 번 말해 **뺐다** |

connector 탭은 언어가 아니라 엔진으로 고르므로 `check_doc_tabs.py`의 언어 라벨 검사에
걸린다. 라벨 중 하나가 표준 언어 이름이면 그 그룹을 언어 탭으로 보는 규칙이라, `Java` 탭을
`Java client`로 적어 검사 대상에서 빠지게 했다. 실제로도 그 탭은 Java **client**를 다룬다.

### 14.4 언어 전환 탭

라벨이 본문 글씨와 같은 크기여서 지금 무슨 언어를 보고 있는지 놓치기 쉬웠다. 라벨을 키우고
(0.82rem·굵게), 고른 탭에 테마 색과 바탕을 넣고, 탭 묶음 아래 바탕선과 내용 왼쪽 선을 넣었다.

### 14.5 사이트가 옛 내용을 내던 원인

`install` 문서를 고쳐도 사이트가 계속 옛 내용을 냈다. 원인은 문서가 아니었다.

`doc/site/docs/install.ko.md`가 **심볼릭 링크가 아니라 복사본**이었다. Windows에서 개발자
모드나 관리자 권한 없이는 파일 심링크가 만들어지지 않고, `ln -s`가 조용히 복사만 한다.
정본을 아무리 고쳐도 사이트는 9월 16일자 복사본을 읽고 있었다.

```
doc/site/hooks/mirror_files.py
```

빌드마다 정본을 `docs/`로 복사한다. 심링크가 살아 있는 환경에서는 그대로 둔다.
**디렉터리 심링크 열 개는 정상이고, 파일만 이 문제를 겪는다.**

같은 증상을 저장소 전체에서 찾아 다섯을 훅에 넣었다 — `index.ko.md`·`index.en.md`·
`install.ko.md`·`install.en.md`·`assets/korean.css`. 홈 페이지도 같은 이유로 경로 한 줄만
렌더하고 있었다. `assets/stylesheets/extra.css`는 심링크가 아닌 정본이라 그대로 고친다.
`common/bench/with-grpc-local.*.md` 둘도 같은 상태이지만 nav에 없어 사이트에 실리지 않는다.

**복사본은 `.gitignore`가 받지 못한다.** 이 경로들은 symlink(mode 120000)로 tracked이기
때문이다. 처음에 `.gitignore`에 넣었다가 무효한 항목이라 되돌렸다. 커밋 전에 되돌리는
명령을 훅 머리말에 적어 두었다.

```bash
git checkout -- doc/site/docs/index.ko.md doc/site/docs/index.en.md                 doc/site/docs/install.ko.md doc/site/docs/install.en.md                 doc/site/docs/assets/korean.css
```

### 14.6 검사기 현황

| 검사기 | 무엇을 본다 |
| --- | --- |
| `check_doc_tabs.py` | 탭 언어 완전성·스니펫 존재·확장자 일치 |
| `check_doc_links.py` | 상대 링크와 앵커 |
| `check_guide_identifiers.py` | 탭 예제가 실재하는 표면 이름만 쓰는가 |
| `check_prose_neutrality.py` | 공통 정본 산문이 언어 고유 이름을 담지 않는가 |
| `check_register.py` | **새로 만들었다.** 7.7이 이름을 든 낱말이 산문에 있는가 |
| `humanize_guard.py` | 산문을 다듬을 때 보호 토큰이 보존되는가 |

여섯 모두 통과한다. `.NET` 문서 회귀 테스트 21건도 통과한다.

### 14.7 활용형까지 잡도록 고친 것

홈 페이지 본문을 눈으로 확인하다 `Spring MVC가 얹히듯`을 발견했다. 검사기가 `얹는다`·
`얹으면` 두 활용형만 보고 있어 `얹히듯`·`얹은`·`얹어`·`얹는`을 놓쳤다. 어간 `얹-`으로
바꿔 잡도록 고치고, 걸린 15개 파일을 문맥에 맞춰 정리했다 — `더해진 편의 계층`·
`적용하는 경계`·`추가하는 얇은 레이어`처럼 자리마다 다른 말이 된다.

**낱말 목록을 활용형으로 적으면 같은 구멍이 다시 생긴다.** 어간으로 잡고 대체어는 여러 개를
적어 두는 편이 낫다.

## 15. 개요 복원과 메뉴 재구성 (2026-09-17)

### 15.1 `01-overview`는 원본을 유지한다

읽는 사람이 축소본을 보고 "기존 내용이 더 좋다"고 했고, 계획 146·305행이 이미 **"01 그대로
— 자리만 옮긴다"** 로 정해 둔 항목이었다. 1,560줄을 775줄로 줄인 것은 그 결정을 어긴
것이므로 되돌렸다.

되돌린 뒤 손댄 것은 **되돌리면 깨지거나 규칙을 어기는 부분뿐**이다.

| 손댄 곳 | 이유 |
| --- | --- |
| 옛 장 번호 링크 110곳 | `05`~`11`·`16`이 `20`~`26` 체계로 바뀌어 대상 파일이 없었다 |
| §8 "가이드 읽는 순서" | 사라진 `02-getting-started`·`13-interface-catalog`를 가리켰다. 퀵스타트와 현재 목차로 다시 썼다 |
| spec 안내 블록 11곳 | 작성 가이드 1.4·6.6이 금지한다. 진입점 문장은 남기고 spec 링크와 "spec이 우선이다"만 걷어냈다 |
| `backend-dependency-policy` 링크 10곳 | 공통 경로에 없다. 탭별 언어 디렉터리로 지정했다. Kotlin은 자체 internals가 없어 Java 문서를 가리킨다 |
| 낱말 19곳 | `조립`·`스며든다`·`떠안`·`밀어줘`·`한 몸에`·`더하는` — 원칙 7.7 위반 |
| 앵커 `#23-계층-구조와-등록-지점` 6종 | 원본에서 그 절은 **3.3**이다. `03`·`17`·언어별 `16-options`에서 고쳤다 |

**분량 상한을 넘긴다.** 시작 층 상한은 250줄인데 1,629줄이다. 읽는 사람의 결정이므로
§3.1 상한보다 이 결정이 앞선다.

**중복은 그대로 둔다.** `01` §2(네 가지 사용 상황 308줄)가 `17` §2와, `01` §7(이름 표기
규칙)이 `03` §8과 겹친다. 앞서 `01`에서 옮겨 둔 것이 복원으로 되살아났기 때문이다. 읽는
사람이 "1장과 17장은 그대로 두라"고 했으므로 지금은 두 곳에 둔다. **원칙 2.2(한 내용은 한
문서가 소유한다)를 어기는 상태이므로 언젠가 한쪽을 지워야 한다.**

### 15.2 메뉴를 얕게

nav가 505줄에 깊이 5였다. `Framework > Guide > .NET > 기능별 가이드 > 문서`이고, **같은
23개 장이 언어마다 한 벌씩 다섯 번** 실렸다. 그것이 깊이의 원인이었다.

최상위를 언어로 바꾸고, 사이드바에서 그 한 겹을 걷었다.

| | 전 | 후 |
| --- | --- | --- |
| 경로 | `Framework > Guide > .NET > 기능별 가이드 > 문서` | `기능별 가이드 > 문서` |
| 사이드바 1차 메뉴 | Framework · Bindings · Core | 시작 · 기능별 가이드 · 동작 원리 · 운영 · 샘플과 테스트 · 레퍼런스 · Samples · Spec · Bindings · Core |
| 사이드바에 보이는 언어 | 다섯 묶음 모두 | 고른 하나 |
| 언어 고르기 | 사이드바에서 그 언어 묶음을 펼친다 | 헤더의 선택기 |

nav에 실린 문서 388건은 그대로다(이전 388 = 이후 388).

### 15.3 언어 선택기

```
doc/site/overrides/partials/zlink-lang.html
doc/site/docs/assets/javascripts/zlink-lang.js
```

로케일(한국어/English) 선택기와 **같은 `md-select` 마크업**이라 생김새와 동작이 그것과 같고,
바로 옆에 선다. 주소의 언어 구간만 바꾸므로 **언어를 바꿔도 보던 장에 머문다.**

스크립트는 사이드바에서 지금 언어의 묶음 한 겹을 걷고 나머지 넷을 감춘다. nav 자체는 그대로
두므로 이전·다음 장 링크와 활성 표시가 살아 있다. Spec·Bindings·Core·Samples는 언어가
아니므로 건드리지 않는다 — 1차 메뉴에 그대로 남는다.

`navigation.tabs`는 켜지 않았다. 최상위를 헤더 탭으로 빼면 Spec·Bindings·Core가 사이드바에서
사라지기 때문이다.

### 15.4 사이트가 20초마다 다시 빌드하던 문제

`mkdocs serve`가 유휴 상태에서도 20초마다 재빌드를 돌아, 언제 접속해도 빌드 중이라 응답이
막혔다. §14.5에서 만든 `mirror_files.py`가 원인이었다 — `on_pre_build`에서 정본을 `docs/`로
복사하고, 그 복사가 mtime을 바꾸고, 감시기가 재빌드를 걸고, 훅이 또 복사했다.

내용이 같으면 쓰지 않도록 고쳤다. 유휴 45초 동안 빌드 횟수가 3에서 그대로다.

### 15.5 connector 가이드의 사실 오류

검증 서브에이전트 넷의 보고 중 확인된 것을 고쳤다.

| 고친 것 | 근거 |
| --- | --- |
| `Dispatch.Immediate` → `DispatchMode = ZlinkStreamDispatchMode.Immediate` | `Dispatch`는 펌프를 실행하는 lifecycle call이고, 모드를 정하는 것은 옵션이다 |
| dispatch 대기 queue 상한 1024와 폐기 규칙을 여섯 문서에 | `MaxPendingDispatchCallbacks` 기본 1024, 넘치면 오래된 것부터 버린다 |
| Java `registration.close()`를 try/catch로 | `on(...)`의 반환형이 `AutoCloseable`이라 checked 예외를 던진다 |
| "connector 실행 비대상" → "제품 실행 환경이 아니다" | 스펙 §2의 문구다. 저장소가 도는 Node tutorial client를 함께 배포하므로 앞의 표현은 모순으로 읽힌다 |
| Kotlin `zlink-framework-kotlin`이 `zlink-framework-core`도 포함한다는 경고 | `build.gradle.kts:11`의 `api(project(":zlink-framework-core"))` |
| UPM 태그 `v0.14.0` → `v0.16.0` (다섯 곳) | 어댑터 패키지와 번들 원본 모두 0.16.0이다 |
| `INDEX`의 "`.NET` framework 가이드 09 — STREAM" → "공통 서버 가이드 23 — STREAM" | 옛 번호 체계의 라벨이었다 |

Node connector 가이드의 "이 가이드는 최신이 아니다" 배너는 뗐다. 검증에서 표면 이름과
transport 표가 구현과 일치함을 확인했기 때문이다. **나머지 열 편(C++ connector·네 언어
http-client)의 배너는 남겼다** — 그쪽은 확인된 오류가 그대로 있다.

### 15.6 install 문서

| 지적 | 반영 |
| --- | --- |
| 대표 의존성 하나만 | 탭마다 설치 줄 한 줄. host 패키지가 framework와 Core를 포함한다 |
| framework가 Core를 포함 | 다섯 탭 모두 Core binding을 굵게 표시한 행으로 첫머리에 둔다 |
| Kotlin과 Java가 같아야 한다 | Kotlin 탭이 Java 표를 그대로 싣고 coroutine 관련만 더 갖는다 |
| 데스크톱·서버 → e2e | C++만 전용 타깃 `zlink::stream_e2e_client`를 둔다 |
| `들어온다`·`따라온다`·`데려온다` | 전부 `포함한다`. 표 머리는 `포함 패키지 \| 역할` |
