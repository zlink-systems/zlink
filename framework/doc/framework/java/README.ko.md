# ZLink Framework for Java/Kotlin -- 문서

> 이 묶음은 `Java`, `Kotlin`, `Spring Boot`용 ZLink Framework 문서다. 이
> 디렉토리에는 `internals/`의 구현·검증 기준을 두고,
> 공개 계약은 [중앙 Java spec](../common/spec/server/languages/java/README.ko.md)에 둔다. 공통 의미는
> [공통 스펙](../common/README.ko.md)을 따르며, 여기서는 그 의미를
> Java/Kotlin 표면으로 구체화한다.

비동기 실행, `CompletionStage`, Kotlin coroutine wrapper의 공통 의미는
[비동기 실행과 coroutine 정책](../common/spec/05-async-execution-policy.ko.md)을 따른다.

Sample과 E2E의 설정 파일, 환경 변수 금지와 `@ConfigurationProperties` binding 기준은
[Sample/E2E 설정 정책](../common/sample-e2e-configuration-policy.ko.md)을 따른다.

> **Kotlin 사용자**는 [Kotlin 전용 guide](../kotlin/README.ko.md)를 본다.
> `zlink-framework-kotlin`은 이 런타임을 공유하는 얇은 coroutine idiom 레이어다.
> Java에서 그대로 사용하는 계약은 Java spec을 따르고, Kotlin 전용 `suspend`/`Flow`
> 계약은 [Kotlin spec](../common/spec/server/languages/kotlin/README.ko.md)에 따로 고정한다.
> Java 사용 guide는 11.0 public interface와 sample이 확정된 뒤 이 위치에 다시 작성한다.

서버 framework와 별도로 사용하는 client library의 사용법은
[HTTP client 가이드](guide/http-client/README.ko.md)와
[Stream connector 가이드](guide/stream-connector/README.ko.md)에서 확인한다.

## 2. 공개 계약 spec

정식 spec은 Framework 목표 public contract를 먼저 고정한다. 현재 Java/Kotlin 구현과의 차이는
`90-implementation-gap.ko.md`만 기록한다. 모든 framework 언어의 공개 계약은
`spec/<package>/languages/` 아래에서 함께 관리한다.

| 문서 | 범위 |
|------|------|
| [spec 목차](../common/spec/server/languages/java/README.ko.md) | Java/Kotlin 공개 계약 문서 목록 |
| [Java interfaces](../common/spec/server/languages/java/interfaces/README.ko.md) | 기능별 exact public signature와 Spring host lifecycle |
| [stream-connector](../common/spec/stream-connector/languages/java/03-stream-connector.ko.md) | client connector |

**기능의 의미와 동작 규칙은 [공통 스펙](../common/spec/README.ko.md)이 소유한다.** 언어별 문서는
그 의미가 Java/Kotlin에서 어떤 모양인지만 고정한다.

## 3. 내부 기준

`internals/`는 유지보수자를 위한 구현 구조, lifecycle, regression 기준을 설명한다.

| 문서 | 범위 |
|------|------|
| [backend-dependency-policy](internals/backend-dependency-policy.ko.md) | Java binding 의존 격리 |
| [공통 내부 구조](../common/internals/README.ko.md) | 네 언어가 공유하는 runtime 아키텍처 결정 |
| [regression-test-matrix](internals/regression-test-matrix.ko.md) | JVM contract, E2E와 performance smoke 기준 |

## 4. 샘플

샘플은 Java/Kotlin 양쪽에서 같은 scenario set을 제공한다. 정본 6종은 per-app 문서로,
기능 축 샘플은 별도 문서로 둔다.

정본 6종의 서버 역할, 메시지 계약, 상태 전이와 완료 기준은
[공통 샘플](../common/sample/README.ko.md)이 소유한다. Java 문서는 이 계약을 다시
서술하지 않는다.

| 문서 | 범위 |
|------|------|
| [samples README](../../../languages/java/samples/README.md) | Java/Kotlin sample 구조와 실행 방법 |
