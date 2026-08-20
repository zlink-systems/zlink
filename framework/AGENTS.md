# Framework Guidelines

이 규칙은 `framework/` 아래 구현, test와 sample에 적용한다.

## Public contract parity

- Server public contract의 기준은 `framework/doc/framework/common/spec/server/`와 공통 guide다. HTTP
  client와 stream connector 계약은 각각 같은 spec root의 package 디렉터리가 소유한다. 다른 언어 구현이나
  E2E만 근거로 public API를 추가하지 않는다.
- 계약에 있는 공통 기능은 C++, .NET, Java, Kotlin과 Node.js에서 같은 사용자 동작을 제공해야 한다.
  즉시 구현할 수 없으면 정확한 이유와 사용자 차이를 implementation gap으로 남긴다.
- 계약에 없는 기능은 internal helper, private API, raw frame 또는 test adapter로 우회 구현하지 않는다.
- E2E는 계약 검증과 누락 탐지에 사용하지만 새 public API의 출처로 사용하지 않는다.
- 언어 차이 때문에 기능을 제외할 때는 사용자가 보는 차이와 대안을 문서화한다.

## 구현과 test

- 먼저 관련 공통 spec과 대상 언어 exact interface를 확인한다. 동일 개념의 이름과 error 의미를
  언어별로 임의 변경하지 않는다.
- Framework의 codec, transport와 lifecycle 책임을 application handler나 sample로 노출하지 않는다.
- Sample은 public API만 사용한다. Sample 전용 runtime helper나 private escape hatch를 만들지 않는다.
- 한 언어의 수정 중에는 해당 언어의 focused test를 먼저 사용한다. Cross-language 전체 suite와
  모든 sample은 계약 범위가 고정된 최종 단계에서 실행한다.
- E2E expectation을 구현 편의에 맞춰 낮추지 않는다. 계약과 충돌하면 구현과 spec 중 어느 쪽이
  잘못됐는지 먼저 보고한다.

## Binding package 사용

- Framework는 binding source를 직접 참조하지 않고 중앙에서 지정한 package version을 사용한다.
- Core나 binding을 수정한 뒤 Framework로 검증할 때는 `scripts/local-package/README.ko.md`의 sync,
  package 생성, version과 cache 절차를 먼저 확인한다. 이전 native library로 얻은 결과를 사용하지 않는다.
- Version 기준은 Java/Kotlin `gradle/libs.versions.toml`, Node.js `package.json`, .NET
  `Directory.Packages.props`, C++ `ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION`이다.

## 참조 문서 위치 (수정 전 반드시 확인)

Server public contract의 단일 규범 원천은 `framework/doc/framework/common/spec/server/`다.
같은 개념을 언어별로 임의 변경하지 말고, 아래 문서에서 계약을 먼저 확인한다. (`.ko.md`가 기준,
`.en.md`는 미러.)

- **용어·값 형태**: [`01-glossary`](./doc/framework/common/spec/server/01-glossary.ko.md) —
  특히 generation·token 계열의 **형태와 비교 규칙**(아래 "교차언어 불변식" 참고).
- **메시징·실행**: `04-message-model`, `05-async-execution-policy`, `08-channel-messaging`,
  `12-spot-messaging`, `14-actor-model`, `20-session-actor-dispatch`.
- **토폴로지·라우팅**: `13-mesh-node`, `21-location-runtime`, `45-internal-routing-and-cache`,
  `46-internal-dispatch-loop`, `50-internal-message-ownership`.
- **wire 프로토콜(규범)**: [`51-internal-service-wire-protocol`](./doc/framework/common/spec/server/51-internal-service-wire-protocol.ko.md)
  + [`service-wire-v1.schema.json`](./runtime/protocol/service-wire-v1.schema.json). **스키마가 유일
  규범 wire source**다. command ID·frame·body는 스키마의 닫힌 정의를 따르며, 생성기
  (`runtime/protocol/generate-service-wire-assets.mjs`)와 검증기(`validate-service-wire-schema.mjs`)를 통과해야 한다.
- **relocation**: `28-relocation-flow`, `30-host-relocation-flow`, `52-internal-relocation-handoff`,
  `23-relocation-store-redis`, `43-internal-completion`, `44-internal-relocation-continuity`.
- **liveness·admission**: `29-transport-liveness`, `49-internal-liveness-and-state`,
  `48-internal-session-binding`.
- **관찰·트레이싱·메트릭**: `24-runtime-monitoring`, `25-runtime-metrics`,
  [`26-message-flow-tracing`](./doc/framework/common/spec/server/26-message-flow-tracing.ko.md),
  `27-flow-correlation` (디버깅 시 먼저 켜는 대상).
- **언어별 exact interface**: `doc/framework/common/spec/server/languages/<lang>/`.
- **e2e 계약**: `doc/framework/common/e2e/config-*.md` (기대치를 구현 편의로 낮추지 않는다).
- **언어별 보완 규칙**: `framework/languages/<lang>/AGENTS.md`(예: `dotnet/AGENTS.md`),
  local package 절차 `scripts/local-package/README.ko.md`.

## 디버깅 (간헐 실패)

전체 규율은 루트 [`AGENTS.md` §4.1](../AGENTS.md)과
[`spec/server/README.ko.md` "디버깅 원칙"](./doc/framework/common/spec/server/README.ko.md) +
[`26-message-flow-tracing`](./doc/framework/common/spec/server/26-message-flow-tracing.ko.md)를 따른다.
요지:

- **먼저 이미 있는 message-flow 트레이싱과 파일 로그를 켜서 읽는다.** 임시 콘솔 로깅을 추가하고
  재현을 다시 돌리지 않는다(재현 사이클 하나를 예외 한 줄 보는 데 낭비하고, 이미 flow에 찍힌
  원인을 놓친다). 통과 케이스와 실패 케이스의 트레이스를 나란히 놓고 **어느 transition에서
  멈췄는지** 찾는다.
- message-flow는 dispatch diagnostics 레벨(`Normal`/`Detailed`)로 켠다. 크로스랭 dotnet TestHost는
  `<EventFilePath>.flow`에 기록하고, 시나리오 configurator가 listener를 안 건 경우 stream-raw 노드처럼
  거는 것도 "기존 기능 켜기"다. 보조 trace: cpp/.NET `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`,
  java/kotlin `ZLINK_JAVA_STREAM_TRACE=1`, node OTel flow exporter + `ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`.
  run dir 보존 `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`. **첫 재현부터 로그를 보존한다.**
- **주의: message-flow(spec 26)는 application dispatch 표면만 덮는다.** RouteMesh control-plane
  (admission `hello/admit/update`, liveness `probe/ack`, relocation control)은 spec 29/49 소관이며
  message-flow에 안 잡힌다. 이 경로는 언어별 stream/spot-discovery trace로 본다.
- **실패는 반드시 flow에 남긴다**(README §3). generic reason(예: `STORE_UNAVAILABLE`)이나 조용한
  `catch`로 원인을 삼키지 말 것 — 실제 예외 class/message를 같은 flow의 `errorType`/`errorMessage`로
  남긴다. 임시 로깅은 조사 후 삭제하고, 반복·중요 transition은 spec 26 단계로 정식 승격하되
  README §4 "비용 규칙"을 지킨다(트레이스 off일 때 무비용: hot path는 `if(enabled)` 게이트, rare는
  lazy/thunk).

## 교차언어 불변식 (자주 깨지는 지점)

- **4언어 동일 프로토콜**이 근간이다. 한 언어의 사설 dialect(예: private JSON fallback)로
  cross-language 경로를 우회하지 않는다 — 카논 service-wire(스키마)를 발행한다.
- **generation·token 비교 규칙(재발 버그류)**: `.NET`이 `ulong`로 보내는 값은 최상위 비트가 켜지면
  다른 언어(Java `long` 등)에서 **음수로 디코드**된다. 형태를 `01-glossary`에서 확인하고 구분한다.
  - **full-range opaque equality token**(Lifecycle/node generation, OperationId 등: "0이 아닌 opaque
    equality token, 숫자 크기로 순서 판단 안 함"): **오직 `== 0`/`!= 0`로만 검사**한다. `> 0`/`<= 0`/
    `< 1`/`>= 0`(sign 기반 presence 포함)은 **버그** — 정당한 음수-디코드 값을 거부하거나 fence를
    생략한다. presence는 sign이 아니라 별도 boolean/Optional로 표현한다.
  - **bounded counter**(ObjectGeneration·AuthorityOwnerGeneration·OwnerLeaseGeneration·
    DescriptorRevision 등: "1..long.MaxValue", wrap 대신 exhausted): 음수가 될 수 없으므로
    `<= 0`가 `== 0`과 동치라 유지해도 된다.
  - 신규 코드가 wire/store에서 디코드한 generation을 검사할 때는 반드시 이 구분을 적용한다.
- **spec 변경 정책**: 스펙은 오류·개선만 수정하며 구현 편의로 완화하지 않는다. 스펙 판정·수정은
  담당자(사람/조정자)가 하고, 필요 시 4언어에 일괄 전파한다. 계약과 구현이 충돌하면 어느 쪽이
  잘못됐는지 먼저 보고한다.

## 언어별 게이트·환경 주의

- **게이트 명령**: C++ `ctest`(빌드 디렉터리 재사용, `-L framework-unit`); .NET
  `dotnet test tests/Zlink.Framework.UnitTests/...`(전체 solution 빌드 금지 — e2e 프로젝트에 사전
  존재 빌드 이슈); Java/Kotlin `./gradlew --no-daemon :zlink-framework-core:test`; Node
  `npm test`(빌드→typecheck→lint→per-file `node --test`; lint에 기존 baseline 오류가 있으면 런타임
  테스트가 그 전에 멈출 수 있음).
- **크로스랭 하니스**(`framework/languages/cpp/cross-language/run_cross_language_smoke.sh`):
  run별 독립 redis + ephemeral 포트라 동시 실행이 서로 충돌하지 않는다. **단 java 호스트는
  자동 재빌드되지 않는다** — java를 바꿨으면 `cd framework/languages/java && ./gradlew --no-daemon
  -p cross-language :Host:installDist`로 먼저 재빌드한다. dotnet 호스트는 `dotnet run`으로 매번
  재빌드된다.
- **알려진 환경 flake**: C++ ctest 간헐 SIGABRT(exit 86/134)는 WSL 환경 flake이니 실패 전 1회
  재실행한다. 그 외 언어별 사전 존재 baseline은 회귀와 구분해 보고한다(신규 실패만 문제).
- **actor admission/join의 시간 예산**(예: 1초)은 실제 계약이다. 부하가 아니라 코드 경로가
  예산을 넘기면(무한 재전송·기아·늦은 resolve 등) 근본을 고친다 — 예산을 임의로 늘리지 않는다.
