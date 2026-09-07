# Agent Guidelines

이 파일은 저장소 전체에 적용하는 최소 공통 규칙이다. 특정 디렉터리의 상세 규칙은 그
디렉터리의 `AGENTS.md`가 소유한다. `CLAUDE.md`는 이 파일을 단일 진입점으로 참조한다.
사람용 절차와 위치(빌드, gate, 성능 판정, 릴리스, 판정 기록)는 [`CONTRIBUTING.ko.md`](./CONTRIBUTING.ko.md)가 소유한다.

## 1. 작업 시작과 변경 보호

- 작업 전에 `git branch --show-current`를 확인한다. 사용자가 다른 branch를 지정하지 않으면
  `main`에서만 수정, commit과 push를 수행한다.
- 다른 branch이고 worktree가 clean하면 `main`으로 전환한다. 변경이 있으면 먼저 범위를
  보고하며, 승인 없이 branch 전환, `reset`, `restore`, 강제 checkout 또는 삭제를 하지 않는다.
- 기존 변경과 untracked 파일은 사용자 작업이다. 요청 범위 밖의 변경을 수정하거나 정리하지 않는다.
- branch 생성·전환·merge와 commit·push는 사용자가 명시적으로 요청한 경우에만 수행한다.

## 2. 효율적인 조사와 실행

- 먼저 목표, 수정 범위, 완료 조건과 필요한 검증을 정한다. 사용자가 요구하지 않은 계획서,
  ledger, gap 문서를 만들지 않는다.
- 파일 검색은 `rg`와 `rg --files`를 우선한다. 관련 symbol과 호출 경로부터 좁히고, 근거 없이
  디렉터리 전체나 대형 파일 여러 개를 읽지 않는다.
- 이미 확인한 repository 구조와 실행 결과를 같은 작업에서 다시 조사하지 않는다. 재시도할 때는
  마지막 실패와 변경된 경계만 확인한다.
- 명령 출력은 필요한 범위로 제한한다. 전체 log 대신 첫 실패, 관련 구간과 최종 요약을 읽는다.
  전체 log가 필요하면 파일에 보관하고 필요한 부분만 조회한다.
- 독립 범위가 분명하지 않으면 agent를 추가하지 않는다. 같은 파일이나 같은 원인을 여러 agent가
  중복 조사하지 않는다.
- 가능한 가장 작은 검증부터 실행한다. 관련 test → subsystem suite → 최종 전체 gate 순서로
  확장하며, 원인 변화 없이 같은 전체 gate를 반복하지 않는다.
- lint, format, rename 같은 기계적 작업에는 사용할 수 있는 가장 가벼운 도구나 model을 사용한다.

### 2.1 Sub-agent model·추론 레벨

**모델은 작업의 모호성·중대성으로 고르고, 추론 레벨은 작업 난이도로 고른다. 둘은 별개다.**
약한 모델을 높은 레벨로 보상하려 하지 않는다 — 그 경우 모델 등급을 올리는 쪽이 맞다.
근거: 최신 모델의 낮은 레벨이 이전 세대의 high를 넘는 경우가 많고, 비용 캐스케이드를 짜기
전에 "가장 유능한 모델을 낮은 레벨로" 돌려보는 것이 먼저다.

#### 추론 레벨

`high`가 기본이다. 실측(26개 실제 코딩 작업)에서 레벨은 통과율만이 아니라 **결과물의 종류**를
바꾼다.

| 레벨 | 언제 | 실측 특성 |
|------|------|-----------|
| `low` | **쓰지 않는다** | 휴리스틱·부분 구현에 머물고 저장소·도메인 모델링을 못 한다 |
| `medium` | 일상적이고 범위가 잘 정의된 작업, 비용이 더 중요할 때 | 저장소 모델링은 되나 통합 세부가 자주 어긋난다 |
| `high` | **기본값.** 판단·구현·리뷰·원인이 좁혀진 진단 | 실용적 최적점. medium 대비 테스트 +15.4 %p, 비용 +43 % |
| `xhigh` | 측정된 실패가 있을 때, 또는 동시성·고위험 작업과 최종 릴리스 리뷰 | 의미적 완성도와 리뷰 통과율은 최고이나 비용이 크고 통과율은 비단조 |

- 올리는 기준은 **측정된 실패**다. 지레 올리지 않는다.
- 올려야 할 작업의 성격: 깊은 의존성 추적, 모호한 진단, 아키텍처 트레이드오프, 보안·릴리스
  최종 리뷰.

#### Codex (`codex exec -m <model id> -c model_reasoning_effort=<level>`)

모델 id는 세대 접두를 그대로 쓴다(ChatGPT 계정 실측 2026-09-07: `gpt-6-sol` 같은 표기는 400으로 거절된다).

| 모델 | 모델 id | 등급 | 단가(입력/출력 per 1M) | 쓰는 일 |
|------|---------|------|------------------------|---------|
| `astra` | `gpt-6-astra` | GPT-6 최상위 | $10 / $50 | 가장 어려운 end-to-end 작업. 원인 가설이 없는 진단, 계약·사양 충돌 해석 |
| `sol` | `gpt-5.6-sol` | GPT-5.6 flagship | $5 / $30 | 모호하거나 중대한 작업. 계약·수명·에러 경로가 걸린 구현 |
| `terra` | `gpt-5.6-terra` | GPT-5.6 balanced | $2.50 / $15 | 일상 작업. 변경 지점이 정해진 구현, 전수 조사 |
| `luna` | `gpt-5.6-luna` | GPT-5.6 fast | $1 / $6 | 잘 정의된 고volume 작업. 단일 파일 수정, 정해진 패턴 적용 |

- **불확실하면 `sol` + `medium`에서 시작하고, 증거를 쌓아 비용 곡선을 내려간다** — 일상은
  `terra`, 잘 정의된 작업은 `luna`. 올리는 것은 측정된 실패가 있을 때만.
- `astra`는 입력이 `sol`의 2배, 출력 1.67배다. 처음부터 쓰는 경우는 원인 가설이 아예 없는
  진단뿐이고, 그 외에는 `sol`이 막힌 뒤에 올린다.
- **입력 272K 토큰을 넘으면 `astra`의 요금제가 바뀐다** — 입력 2배($20), 출력 1.5배($75).
  브리프의 범위와 읽을 파일을 좁혀 넘지 않게 한다.
- **캐시된 입력은 $1로 10배 싸다.** 연관된 job은 브리프 앞부분(원칙·필독 문서 목록)을 동일하게
  유지해 prefix가 재사용되게 한다.

#### Claude (`Agent` 도구의 `model`)

Claude도 추론 레벨을 정할 수 있으나 **호출 시점이 아니라 에이전트 정의에서** 정한다 —
`Agent` 도구 호출 파라미터에는 `model`만 있고, 그 에이전트 타입의 model·추론 레벨·tools는
`.claude/agents/*.md` frontmatter가 소유한다. 정의 파일이 없으면 세션 기본값을 상속한다.

| 모델 | 쓰는 일 |
|------|---------|
| `fable` | **감독 레벨 전용.** sub-agent에는 사용하지 않는다 |
| `opus` | sub-agent 기본값. 판단이 필요한 구현, 코드 리뷰, 원인이 좁혀진 진단 |
| `sonnet` | 변경 지점이 명확한 구현, 정해진 절차의 실행, 빌드·테스트 반복, 기계적 수집과 정찰 |

- **`fable`은 감독 레벨에서 쓰는 모델이다. sub-agent에 지정하지 않는다.** 예외는 특별한
  이슈가 있을 때뿐이며, 그 경우 사유를 결정 기록에 남긴다.
- **사용하는 모델은 위 세 개뿐이다.** `haiku`를 비롯한 다른 모델은 쓰지 않는다.

#### 어느 도구를 쓸 것인가

**Sub-agent는 codex를 우선 사용한다.** Claude sub-agent는 codex에 이슈가 있을 때 쓴다 —
쿼터 소진, 콘텐츠 필터로 job이 죽음, 반복 실패, 또는 codex가 접근할 수 없는 작업.
Claude로 대체했으면 그 사유를 결정 기록이나 작업 로그에 남긴다.

#### 공통 규칙

- 큰 작업은 단계로 나눠 정찰·수집을 가벼운 agent에 먼저 맡기고, 무거운 model은 판단과 설계가
  필요한 단계에만 투입한다.
- 지정하지 않으면 세션 설정을 상속한다. 기계적 작업이면 명시적으로 낮춘다.
- 이미 실행 중인 agent는 model을 바꾸려고 중단·재투입하지 않는다. 재작업 비용이 더 크다.
- 동시 실행은 codex job 최대 3개. Core LTO 빌드가 코어를 전부 쓰므로 빌드 중에는 perf·gate를
  돌리지 않는다.
- **sub-agent는 스펙 문서를 수정하지 않는다.** 스펙·정책 개정은 감독자가 직접 한다.
- sub-agent가 낸 발견은 인용한 사양·코드를 감독자가 직접 열어 재검증한 뒤에만 채택하고,
  채택/기각 사유를 결정 기록에 남긴다.
- 위 실측 근거는 GPT-5.5 세대 기준이다. 우리 job의 결과가 이와 다르면 관찰을 기록하고 기준을
  조정한다 — 이 표는 출발점이지 고정값이 아니다.

## 3. 구현 원칙

- 기존 public API, 표준 호출 경로와 abstraction을 먼저 사용한다. 같은 의미의 helper, DTO,
  adapter나 우회 경로를 추가하지 않는다.
- 실패 지점을 우회하지 말고 책임을 소유한 모듈에서 원인을 수정한다. codec, transport, retry,
  lifecycle과 registry 결정을 호출자나 sample로 밀어내지 않는다.
- 새 public API가 필요해 보이면 기존 계약 근거를 먼저 확인한다. 계약이 없으면 구현 전에
  설계 변경으로 분리해 사용자에게 보고한다.
- sample은 public 사용 예시다. test를 통과시키기 위해 sample에 internal helper, raw frame 해석,
  private policy 또는 임시 codec을 넣지 않는다.
- Framework message는 기본 typed JSON serializer 경로를 사용한다. 메시지별 codec 등록 API나
  호출부의 수동 encode/decode로 내부 연결 문제를 우회하지 않는다.
- 비자명한 설계는 최소 두 대안을 비교하되, 요청하지 않은 장문의 설계 문서를 만들지 않는다.
  인터페이스를 단순하게 유지하고 자료구조와 protocol 결정은 소유 모듈 안에 숨긴다.
- **계층 소유권(필수).** 어떤 결정(연결 선택·교체, reconnect, handover 수렴, completion drain,
  DONTWAIT 재제출, errno 분류, reply 라우팅, 재전송)을 코드에 넣기 전에 그 결정을 소유한 계층을
  spec 조항으로 먼저 확인한다. Core·binding이 소유한 결정을 Framework에서 다시 구현하거나 같은
  사실을 두 곳의 상태로 유지하지 않는다. 하위 계층이 spec과 다르게 동작하면 상위에서 보상하지
  말고 하위 계층의 버그로 보고한다(공개 API repro 포함).
- **금지 패턴.** 다음은 원인 수정이 아니라 우회이므로 하지 않는다: timeout·budget·retry 횟수 증가,
  "안전을 위한" 재시도나 순서 변경, monitor event로 만든 별도 pair·generation 상태로 수신 frame을
  걸러내기, 같은 socket에 두 번째 poller, 예외를 삼키는 catch-all, 실패를 없애기 위한 fixture 조건
  완화, 구현에 맞춘 assertion 변경.
- **교차언어 대조(필수).** Framework runtime 동작을 바꾸기 전에 같은 상황을 다른 언어 구현이 어떻게
  처리하는지 확인한다. 한 언어만 변경이 필요하면 그 이유가 구조적 차이인지 다른 root cause의
  증상인지 완료 보고에 적는다.
- **원칙이 단순해지는 근본 수정(필수).** 여기서 "단순"은 변경량이 아니라 **결과 설계의 규칙 수**다. 수정 뒤에 사실 하나에
  소유자 하나, 결정 하나에 규칙 하나, 특수 case·예외 경로·조건 분기가 줄어들어야 한다. 규칙을 줄이기 위한 큰 diff는
  허용하고, 규칙을 하나 더 얹는 작은 diff(새 상태·타이머·인덱스·헬퍼 계층·옵션·"이 경우만" 분기)는 금지한다. 두 대안이 있으면
  설명해야 할 규칙이 적은 쪽을 고르고, 완료 보고에 "수정 전/후 규칙 수"를 한 줄로 적는다. 같은 규칙·상태·헬퍼·매핑표를 두 곳에
  두지 않는다(중복 금지): 이미 있는 소유자를 재사용하고, 새로 만들어야 하면 기존 것을 그 자리로 옮겨 하나만 남긴다.
- **진단 먼저, 구현은 승인 뒤.** Framework runtime 수정은 두 단계로 한다. 1단계는 코드 변경 없이
  원인 `file:line`, 소유 계층과 spec 조항, 교차언어 대조, 변경 분류(A 계약 적응 / B 기존 결함 /
  C 우회 / D spec gap)를 보고한다. 감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다.
- POSDDD 원칙 문서
  [`doc/principal/dev/posddd.ko.md`](./doc/principal/dev/posddd.ko.md)와
  [`doc/principal/dev/zlink-system-design-principles.ko.md`](./doc/principal/dev/zlink-system-design-principles.ko.md)는
  요청과 무관하게 runtime 변경 시 항상 적용한다.

## 4. 검증과 완료 보고

- 수정 중에는 관련 test만 실행한다. 전체 test, E2E, sample과 benchmark는 사용자 요청 또는
  최종 검증 필요성이 있을 때 한 번 실행한다.
- 첫 실제 실패에서 원인을 분리한다. unrelated failure를 임의로 고치거나 expectation을 낮추지 않는다.
- Core를 바꾸고 Framework에서 검증할 때는 local Core library와 binding package가 실제로 갱신됐는지
  먼저 확인한다. 세부 절차는 `scripts/local-package/README.ko.md`를 따른다.
- 완료 보고에는 결과, 변경 파일, 실행한 test와 남은 실패만 적는다. 진행 이력을 반복하지 않는다.
- Runtime 변경의 완료 보고에는 소유 계층·spec 조항·교차언어 대조 결과·변경 분류(A/B/C/D)를 한 줄씩
  함께 적는다. 이 네 줄이 없거나 분류가 C/D인 변경은 감독이 커밋하지 않는다.
- 사용자와의 한국어 기술 설명은
  [`doc/principal/documentation/documentation-principles.ko.md`](./doc/principal/documentation/documentation-principles.ko.md)를 따른다.

### 4.1 간헐 실패 디버깅 (message tracking·file log)

[`framework/doc/framework/common/spec/server/README.ko.md`](./framework/doc/framework/common/spec/server/README.ko.md)의
"디버깅 원칙"과 [`26-message-flow-tracing`](./framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md)·
[`27-flow-correlation`](./framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md)을 따른다.

- **먼저 이미 있는 message tracking과 file log를 켜서 읽는다.** 임시 로깅부터 추가하고 재현을
  다시 돌리지 않는다 — 재현 사이클 하나를 예외 한 줄 보는 데 낭비하고, 이미 flow에 찍힌 원인을 놓친다.
- message tracking = framework message-flow 트레이싱(`runtime.Flow`/`ZLinkMessageFlowOutcome`).
  프로세스 경계를 넘어 `flow`·`corr`로 메시지를 잇고, 실패·거부·abort를 같은 `flow` 아래
  `message_flow_outcome=error`(errorType/errorMessage)로 남긴다. 통과 케이스와 실패 케이스의
  트레이스를 나란히 놓고 **어느 transition에서 멈췄는지** 찾는다. 카테고리를 노이즈로 필터링하지 않는다.
- 켜는 법: dispatch diagnostics 레벨을 `Normal`(필요 시 `Detailed`)로 올리고 flow listener를
  파일에 배선한다(크로스랭 dotnet TestHost는 `<EventFilePath>.flow`; 시나리오 configurator가
  listener를 안 걸었으면 stream-raw 노드처럼 걸어 켜는 것도 "기존 기능 켜기"다). 보조 trace:
  cpp/.NET `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`, java/kotlin `ZLINK_JAVA_STREAM_TRACE=1`,
  run dir 보존 `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`. **첫 재현부터 로그를 보존한다.**
- 임시 로깅은 필요하면 추가할 수 있으나 **조사 후 삭제한다**. 반복·중요 transition이면 임시로
  두지 말고 spec 26 message-flow 단계로 **정식 승격**하되, README §4 "Cost Rule"을 지킨다 —
  트레이스 off일 때 무비용(hot path는 `if (Flow.Enabled(...))` 래핑, rare는 lazy/thunk로
  event·string·lambda를 게이트 뒤에서만 생성). ungated `Console`/string-concat 로깅을 남기지 않는다.

## 5. 문서 보호

다음 경로는 사용자가 해당 경로와 변경 범위를 명시적으로 승인한 경우에만 생성·수정·삭제·이동한다.

- `core/doc/internals/`, `core/doc/spec/`
- `bindings/doc/spec/`
- `framework/doc/framework/common/{sample,e2e,spec,internals}/`

오탈자, link, formatting, generated 결과도 수정에 포함된다. 일반적인 “문서 정리”, 구현 완료나
test 실패는 승인으로 보지 않는다. 승인 없이 필요한 변경을 발견하면 `file:line`, 이유와 제안만
보고한다.

문서를 수정할 때는 해당 범위의 `AGENTS.md`를 추가로 따른다.

## 6. 범위별 규칙

- 문서 작성 전반: `doc/AGENTS.md`
- Framework와 public contract parity: `framework/AGENTS.md`
- Framework 문서 위치와 계약 작성: `framework/doc/AGENTS.md`
- .NET Framework의 binding 사용: `framework/languages/dotnet/AGENTS.md`
- Core·binding local package 작업: `scripts/local-package/README.ko.md`
- C binding benchmark: `bindings/c/perf/AGENTS.md`

하위 `AGENTS.md`는 자신의 디렉터리에서만 루트 규칙을 보완한다. 같은 내용을 루트에 다시 복사하지 않는다.
