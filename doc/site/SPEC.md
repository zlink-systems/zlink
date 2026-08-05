# zlink Guide Site Specification

## 목적

`doc/guide/`의 21개 가이드 문서를 기반으로, 바인딩 사용자가 자기 언어로
코드 예제를 볼 수 있는 정적 문서 사이트를 만든다.

참조 모델: https://zguide.zeromq.org (언어별 코드 탭 전환)

## 도구

| 항목 | 선택 | 이유 |
|------|------|------|
| 정적 사이트 생성기 | MkDocs Material | content tabs 내장, 마크다운 호환, Python 기반 |
| 언어별 코드 탭 | pymdownx.tabbed | MkDocs Material에서 기본 지원 |
| 다국어 | mkdocs-static-i18n (suffix 모드) | 기존 `.ko.md` 패턴 그대로 사용 |
| 호스팅 | GitHub Pages | GitHub Actions로 자동 빌드/배포 |

## 언어 탭 목록

| 순서 | 탭 라벨 | 코드 블록 태그 |
|------|---------|---------------|
| 1 | C | `c` |
| 2 | C++ | `cpp` |
| 3 | Java | `java` |
| 4 | Python | `python` |
| 5 | Node/TypeScript | `typescript` |
| 6 | C#/.NET | `csharp` |
| 7 | Rust | `rust` |
| 8 | Go | `go` |
- 탭 라벨은 모든 페이지에서 동일해야 한다 (`content.tabs.link`로 전역 동기화).
- 사용자가 한 번 "Python"을 선택하면 모든 코드 블록이 Python으로 전환된다.

## 디렉터리 구조

```
doc/site/
  SPEC.md                         ← 이 문서
  mkdocs.yml                      # MkDocs 설정
  requirements.txt                # pip 의존성
  docs/
    index.md                      # 영어 랜딩 페이지
    index.ko.md                   # 한국어 랜딩 페이지
    guide/
      01-overview.md              # 코드 탭이 적용된 가이드 (영어)
      01-overview.ko.md           # 코드 탭이 적용된 가이드 (한국어)
      02-core-api.md
      02-core-api.ko.md
      ...                         # 21개 × 2언어 = 42 파일
    assets/
      stylesheets/
        extra.css                 # 가독성 커스텀 CSS
.github/workflows/docs.yml       # GitHub Pages 자동 배포
```

- `doc/guide/` 원본은 수정하지 않는다.
- `doc/site/docs/guide/`에 변환본을 둔다.

## 소스 가이드 파일 목록

| 파일 | C 코드 블록 수 | 비고 |
|------|---------------|------|
| 01-overview.md | 1 | 아키텍처 개요 |
| 02-core-api.md | 9 | 핵심 C API |
| 03-0-socket-patterns.md | 2 | 소켓 패턴 개요 |
| 03-1-pair.md | 13 | PAIR 패턴 |
| 03-2-pubsub.md | 21 | PUB/SUB 패턴 (최대) |
| 03-3-dealer.md | 12 | DEALER 패턴 |
| 03-4-router.md | 11 | ROUTER 패턴 |
| 03-5-stream.md | 3 | STREAM 패턴 |
| 03-6-proxy.md | 4 | Proxy 패턴 |
| 04-transports.md | 14 | 트랜스포트 |
| 05-tls-security.md | 10 | TLS 보안 |
| 06-monitoring.md | 18 | 모니터링 |
| 07-0-services.md | 0 | 서비스 개요 (코드 없음) |
| 07-3-spot.md | 8 | SPOT |
| 08-routing-id.md | 14 | Routing ID |
| 09-message-api.md | 20 | Message API |
| 10-performance.md | 12 | 성능 |
| 11-thread-safety.md | 8 | 스레드 안전성 |
| 12-socket-options.md | 1 | 소켓 옵션 |
| **합계** | **199** | |

## 코드 블록 분류 및 처리

### 분류 A: API 예제 (약 100개)

socket 생성, bind/connect, send/recv, publish/subscribe 등 전체 API 흐름을
보여주는 블록. 7개 언어 탭으로 변환한다.

```markdown
=== "C"

    ```c
    void *pub = zlink_socket(ctx, ZLINK_PUB);
    zlink_bind(pub, "tcp://*:5556");
    ```

=== "C++"

    ```cpp
    zlink::pub_socket_t pub(ctx);
    pub.bind("tcp://*:5556");
    ```

=== "Java"

    ```java
    PubSocket pub = new PubSocket(ctx);
    pub.bind("tcp://*:5556");
    ```

=== "Python"

    ```python
    pub = zlink.PubSocket(ctx)
    pub.bind("tcp://*:5556")
    ```

=== "Node/TypeScript"

    ```typescript
    const pub = new PubSocket(ctx);
    pub.bind("tcp://*:5556");
    ```

=== "C#/.NET"

    ```csharp
    using var pub = new PubSocket(ctx);
    pub.Bind("tcp://*:5556");
    ```

=== "Rust"

    ```rust
    let pub_sock = ctx.pub_socket()?;
    pub_sock.bind("tcp://*:5556")?;
    ```
```

### 분류 B: 부분 API (약 60개)

옵션 설정, 단일 함수 호출 등 간단한 블록. 마찬가지로 7개 언어 탭으로
변환하되, 고수준 언어에서는 더 짧을 수 있다.

### 분류 C: C 전용 (약 20개)

`typedef struct`, callback function pointer signature, `memcpy` 패턴 등
다른 언어로 직접 번역할 수 없는 블록. C 탭만 유지하고 admonition으로
고수준 언어에서의 대응을 설명한다.

```markdown
!!! note "C API — struct definition"

    ```c
    typedef struct {
        uint8_t size;
        uint8_t data[255];
    } zlink_routing_id_t;
    ```

    고수준 바인딩에서는 네이티브 타입으로 표현됨:
    Java `RoutingId`, Python `bytes`, C# `RoutingId`, Rust `RoutingId`
```

### 분류 D: 비코드 블록 (약 19개)

ASCII 다이어그램, bash 명령, text 블록 등. 변환하지 않고 그대로 유지한다.
식별 기준: ` ```c ` 태그가 아닌 모든 코드 블록.

## 코드 예제 작성 원칙

- **샘플 코드 직접 활용**: 각 바인딩의 `samples/` 디렉터리에 있는 실행 가능한
  샘플 코드를 가이드에 직접 사용한다. 별도의 문서 전용 코드를 만들지 않는다.
  - 샘플 코드는 실행 가능하고 CI로 검증되므로 문서와 코드의 동기화가 보장된다.
  - 샘플에서 사용하는 helper(`sample_common.hpp`, `SampleSupport.java` 등)는
    이름만으로 의도가 파악 가능하도록 설계되어 있다.
  - 가이드에서 helper를 처음 참조할 때 한 줄로 설명을 추가한다.
- **helper 의존은 최소화되어 있음**: 현재 샘플의 helper 의존:
  - `wait_connected()` — monitor 기반 connection handshake
  - `make_message()` — 문자열에서 Message 생성
  - `wait_future()` — callback 결과 대기
  - `wait_spot_ready()` — SPOT 서비스 handshake
  - 상수 (`k_pair_payload`, `PAIR_PAYLOAD` 등) — 통일된 메시지 내용
- **bidirectional vs one-way 구분**:
  - pair, dealer-router, stream → `send`/`recv` 용어
  - pubsub, spot → `publish`/`subscribe` 용어
- **TCP 프로토콜 통일**: 모든 샘플은 `tcp://127.0.0.1:0` (ephemeral port)을
  사용한다. `inproc://`는 사용하지 않는다.
- **monitor 기반 connection handshake**: `sleep` 대신 monitor 이벤트로
  connection readiness를 확인한다.
- **spot은 two-node 구조**: `SpotNode(pub)` ←TCP→ `SpotNode(sub)` 구조로
  location-transparent pub/sub의 의미를 보여준다.
- **통일된 출력 포맷**:
  - bidirectional: `[pattern/mode] send: "value" → recv: "value"`
  - one-way: `[pattern/mode] publish: "topic/payload" → subscribe: "topic/payload"`
- **통일된 메시지 내용**:
  - pair: `"hello-pair"`
  - dealer-router: request `"ping"`, reply `"pong"`
  - stream: `"hello-stream"`
  - pubsub: topic `"prices"`, payload `"101.25"`
  - spot: topic `"room:lobby"`, payload `"hello-spot"`
- **언어 관례 준수**: 각 언어의 네이밍, 에러 처리, 리소스 관리 관례를 따른다.
  - Python: context manager (`with`)
  - Java: try-with-resources
  - C#: `using` 선언
  - Rust: `?` operator
  - C++: RAII
  - Node: try/finally
  - C: 명시적 close

## mkdocs.yml 설정 요약

```yaml
site_name: zlink Guide
theme:
  name: material
  palette:
    - scheme: default        # light
    - scheme: slate          # dark
  font:
    text: Inter
    code: JetBrains Mono
  features:
    - navigation.tabs
    - navigation.sections
    - navigation.expand
    - navigation.top
    - navigation.footer      # 자동 이전/다음 링크
    - search.suggest
    - search.highlight
    - content.tabs.link      # 코드 탭 전역 동기화
    - content.code.copy      # 코드 복사 버튼

plugins:
  - search
  - i18n:
      default_language: en
      docs_structure: suffix  # .ko.md 패턴 사용
      languages:
        - locale: en
          name: English
          default: true
        - locale: ko
          name: 한국어

markdown_extensions:
  - pymdownx.highlight
  - pymdownx.inlinehilite
  - pymdownx.superfences
  - pymdownx.tabbed:
      alternate_style: true   # content tabs
  - admonition
  - tables
  - toc:
      permalink: true

nav:
  - Guide:
    - Overview: guide/01-overview.md
    - Core API: guide/02-core-api.md
    - Socket Patterns:
      - Overview: guide/03-0-socket-patterns.md
      - PAIR: guide/03-1-pair.md
      - PUB/SUB: guide/03-2-pubsub.md
      - DEALER: guide/03-3-dealer.md
      - ROUTER: guide/03-4-router.md
      - STREAM: guide/03-5-stream.md
      - Proxy: guide/03-6-proxy.md
    - Transports: guide/04-transports.md
    - TLS Security: guide/05-tls-security.md
    - Monitoring: guide/06-monitoring.md
    - Services:
      - Overview: guide/07-0-services.md
      - SPOT: guide/07-3-spot.md
    - Routing ID: guide/08-routing-id.md
    - Message API: guide/09-message-api.md
    - Performance: guide/10-performance.md
    - Thread Safety: guide/11-thread-safety.md
    - Socket Options: guide/12-socket-options.md
```

## pip 의존성

```
mkdocs>=1.6.0
mkdocs-material>=9.5.0
mkdocs-static-i18n>=1.2.0
pymdown-extensions>=10.0
```

## 다국어 처리

- `mkdocs-static-i18n` suffix 모드 사용
- 영어: `guide/01-overview.md`, 한국어: `guide/01-overview.ko.md`
- 코드 탭 내용은 영어/한국어 동일 (코드는 언어 무관)
- 산문(설명 텍스트)만 다름
- 사이트 헤더에 언어 전환 버튼 자동 생성

## 링크 처리

| 유형 | 원본 | 변환 |
|------|------|------|
| 언어 토글 | `English \| [한국어](01-overview.ko.md)` | 제거 (i18n 플러그인이 처리) |
| 가이드 간 | `[Core API](02-core-api.md)` | 유지 (같은 디렉터리) |
| 푸터 네비 | `[← Overview](01-overview.md)` | 제거 (`navigation.footer`가 자동 생성) |
| 외부 문서 | `../api/socket.md` | GitHub URL로 변환 |

## 문서 페이지 헤더 규약

가이드 문서(`framework/doc/framework/<lang>/guide/`)가 이미 갖춘 페이지 상단 구조를,
internals를 포함해 손으로 관리하는 모든 문서 묶음에도 같은 원칙으로 적용한다. 독자가
목차를 거치지 않고 이 페이지로 바로 들어와도 무엇을 다루는 문서인지, 정본 계약은
어디에 있는지 첫 화면에서 알 수 있어야 한다.

각 문서 페이지는 다음 셋을 이 순서로 둔다.

1. **front matter `title:`** — 브라우저 탭과 검색 결과에 뜨는 제목. H1과 같은 뜻이되
   부제까지 담을 수 있다. 없으면 페이지 제목이 파일명이나 첫 H1으로 대체되어 표기가
   흔들린다.

   ```yaml
   ---
   title: "4. Backpressure — 처리보다 도착이 빠를 때"
   ---
   ```

2. **상단 이전·다음 네비게이션** — `navigation.footer`가 하단에 자동으로 만들어 주는
   것과 별개로, 상단에도 같은 링크를 둔다. 독자가 스크롤 없이 인접 장으로 옮겨갈 수
   있어야 한다.

   ```markdown
   [목차](README.ko.md) · [이전: 3. 핵심 개념](03-concepts.ko.md) · [다음: 5. Channel Messaging](05-channel-messaging.ko.md)
   ```

3. **여는 인용 상자** — H1 바로 뒤에 이 페이지가 답하는 질문과, 정본 계약을 소유하는
   문서를 명시한다. "이 문서가 다루지 않는 것(계약 자체)"과 "이 문서가 다루는 것(그
   계약을 설명·구현하는 방법)"을 첫 문단에서 가른다.

   ```markdown
   > **이 장이 답하는 것** — <이 페이지가 답하는 한 문장 질문>.
   >
   > **계약 소유** — <정본 계약 문서 링크>가 소유한다. 이 장은 그 계약을
   > <설명하는 / 만족시키는> <방법 / 구조>를 다룬다.
   ```

가이드는 `python3 doc/site/scripts/generate_language_guides.py`가 공통 소스
(`common/guide/`)에서 이 구조를 자동 생성한다(`<!-- framework-adapter-nav:start -->`
주석과 "이 장의 계약 소유 문서" 인용 상자). internals처럼 언어별 생성기가 없는 문서
묶음은 이 셋을 손으로 넣고 유지한다 — 새 문서를 추가하거나 절 제목을 바꿀 때마다
확인한다.

**적용 범위.** 왼쪽 목차(nav)에 오르는 모든 문서에 적용한다. 문서 성격에 따라 여는
인용 상자의 형태만 갈린다.

- **guide·internals·core doc·bindings doc처럼 정본 계약을 다른 문서가 소유하는
  문서**는 위 3단계 형태를 그대로 쓴다.
- **`spec/`의 정식 계약 챕터**는 자기 자신이 정본이므로 "계약 소유" 문구가 성립하지
  않는다. 이 경우 인용 상자는 소유 줄 없이 한 줄로 줄인다.

  ```markdown
  > **이 장이 정의하는 것** — <이 장이 정의하는 계약을 한 문장으로>.
  ```

  title front matter와 상단 이전·다음 네비게이션은 spec 챕터에도 동일하게 적용한다.

**검증.** `mkdocs build --strict`로 렌더 결과를 눈으로 확인한 뒤 완료로 본다.

## 밀도 높은 문서의 가독성 보강

spec·internals처럼 규칙을 촘촘히 나열하는 문서는 헤더 규약을 지켜도 본문 자체가 숨차게
읽힐 수 있다. 문장이 길게 이어지는 계약 조항, 서로 다른 소주제가 한 소제목 아래 섞인
절, 상태 전이나 경쟁 조건을 산문으로만 서술한 부분이 특히 그렇다. 이런 문서는 다음
넷을 필요한 곳에만 추가한다 — 문장 내용이나 계약 의미는 바꾸지 않고 표현 형식만
바꾼다.

1. **절 지도** — 서론 문단 뒤, 본문 시작 전에 절 제목과 앵커 링크, 한 줄 요약을 표로
   둔다. 독자가 전체 구조를 먼저 보고 필요한 절로 바로 건너뛸 수 있어야 한다.

   ```markdown
   | 절 | 다루는 내용 |
   |---|---|
   | [1.1 Submit, Async와 Yield](#11-submit-async와-yield) | terminator별 완료 의미, `Yield` 사용 가능 범위 |
   ```

2. **소주제별 소제목** — 한 소제목 아래 서로 다른 주제 여러 개가 문단 경계로만 나뉘어
   있으면 짧은 명사구 소제목(`###`/`####`)을 추가해 쪼갠다. 상위 헤딩 레벨을 건너뛰지
   않는다 — `###` 부모가 없는 절에서는 `####` 대신 `###`을 쓴다.

3. **규칙 나열 문장 → 불릿** — 독립된 규칙 서너 개 이상이 한 문단에 이어 붙어 있으면
   문장을 그대로 불릿으로 재분절한다. 문장 표현은 바꾸지 않는다. 여러 개의 독립된
   목록이 한 문서에 공존하면 번호 목록보다 글머리 기호(`-`)가 덜 산만하다 — 항목
   순서에 의미가 있을 때만 번호를 쓴다.

4. **표·다이어그램** — 이름→값, 상태→동작처럼 대응 관계를 서술하면 표로 옮긴다. 상태
   전이나 소유권 인수인계처럼 동시성 흐름을 산문으로 설명하면 mermaid
   `stateDiagram-v2`나 `sequenceDiagram`으로 옮긴다. 다이어그램 종류 선택은 원칙
   3(적층 구조는 ASCII, 흐름·시퀀스는 Mermaid)을 따른다. 기존 ASCII 상태 다이어그램이
   있다면 같은 정보를 유지한 채 mermaid로 바꾼다.

**예시.** `common/spec/05-async-execution-policy.ko.md`와
`common/internals/12-service-wire-protocol.ko.md`가 넷을 모두 적용한 결과다.

**적용 범위.** 원칙적으로 spec·internals 문서 전반에 적용할 수 있지만, 문장이 이미
짧고 절이 이미 잘게 나뉜 문서에는 추가하지 않는다 — 표·다이어그램·소제목이 늘어야
읽기 쉬워지는 곳에만 넣는다.

**검증.** 절 지도의 앵커가 실제 heading id와 일치하는지, 표·불릿 전환 전후로 핵심
식별자·수치가 그대로인지 `git diff`로 대조한 뒤 `mkdocs build --strict`로 렌더 결과를
확인한다.

## GitHub Pages 배포

`.github/workflows/docs.yml`:
- 트리거: `main` push + `doc/site/**` 경로 변경 시
- Python 3.12 + pip install
- `mkdocs build --strict`
- `actions/upload-pages-artifact` + `actions/deploy-pages`
- URL: `https://kairos-code-dev.github.io/zlink/`

## 로컬 개발

```bash
cd doc/site
pip install -r requirements.txt
mkdocs serve
# http://localhost:8000
```

## 구현 순서

### Phase 1: 사이트 기반 구축

1. `mkdocs.yml`, `requirements.txt` 생성
2. `docs/index.md`, `docs/index.ko.md` 랜딩 페이지 생성
3. `docs/assets/stylesheets/extra.css` 생성
4. `doc/guide/` → `doc/site/docs/guide/`로 42개 파일 복사
5. 빌드 확인: `mkdocs serve`

### Phase 2: 가이드 변환 (199개 C 코드 블록 → 7개 언어 탭)

| 배치 | 파일 | 블록 수 | 비고 |
|------|------|---------|------|
| 1 | 01-overview, 03-0-socket-patterns, 12-socket-options | 4 | 패턴 확립, 최소 블록 |
| 2 | 03-1-pair, 03-2-pubsub, 03-3-dealer | 46 | 핵심 패턴 |
| 3 | 03-4-router, 03-5-stream, 03-6-proxy | 18 | 패턴 계속 |
| 4 | 02-core-api, 04-transports, 05-tls-security | 33 | 인프라 |
| 5 | 06-monitoring, 07-0/1/3/4-services | 44 | 서비스 + 고급 |
| 6 | 08-routing-id, 09-message-api, 10-perf, 11-thread | 54 | 나머지 |

변환 절차 (파일당):

1. C 코드 블록 식별 (` ```c ` 태그)
2. 분류 A/B/C/D 판별
3. A/B → 7개 언어 탭으로 변환 (self-contained 스니펫)
4. C → admonition + C 코드만
5. D → 그대로
6. 영어 완료 후 한국어에 동일 코드 탭 적용
7. 링크 수정

### Phase 3: 배포

1. `.github/workflows/docs.yml` 생성
2. GitHub Pages 설정 (Settings > Pages > Source: GitHub Actions)

## 검증 체크리스트

- [ ] `mkdocs serve` 로컬 빌드 성공
- [ ] 사이드바 네비게이션 21개 가이드 표시
- [ ] 코드 탭 전환 동작 (Python 선택 → 모든 블록 Python)
- [ ] 코드 탭 전역 동기화 (페이지 이동 후에도 선택 유지)
- [ ] 한국어 전환 동작
- [ ] ASCII 다이어그램 깨지지 않음
- [ ] 다크모드 전환
- [ ] 검색 동작
- [ ] 코드 복사 버튼 동작
- [ ] `mkdocs build --strict` 경고/에러 없음
- [ ] GitHub Actions 배포 성공
