# ZLink Framework for Node.js -- 문서

> 이 묶음은 `Node.js`, `NestJS`용 ZLink Framework 정식 문서다. 이 디렉토리에는
> `internals/`(구현·검증 기준)를 두고, 공개 계약은
> [중앙 Node.js spec](../common/spec/server/languages/node/README.ko.md)에 둔다. 공통 의미는
> [공통 스펙](../common/README.ko.md)을 따르며, 여기서는
> 그 의미를 Node.js와 NestJS 표면으로만 구체화한다. 공개 계약은 중앙 언어별
> spec과 공통 framework spec이 소유하며, 다른 언어 구현은 계약 해석을 비교하는
> 참고 자료로만 사용한다.

비동기 실행, `Promise`, helper 동기 함수의 공통 의미는
[비동기 실행과 coroutine 정책](../common/spec/05-async-execution-policy.ko.md)을 따른다.
Node framework 의 서버와 client network API 는 `Promise` 기반 비동기 함수로 투영한다.
`Async` suffix 는 옮기지 않고, `connect()`, `close()`, `submit()`, `waitFor()`,
`start()`, `stop()`, `handle()` 처럼 동작 이름과 `Promise<T>` 반환 타입으로 비동기
계약을 드러낸다. codec 변환, packet name 계산, 값 객체 생성처럼 network I/O 를 하지
않는 순수 helper 는 동기 함수일 수 있다.

Sample과 E2E의 설정 파일, 환경 변수 금지와 NestJS typed configuration provider 기준은
[Sample/E2E 설정 정책](../common/sample-e2e-configuration-policy.ko.md)을 따른다.

## 1. 사용 안내

Node framework의 공개 API와 동작은 아래 정식 spec에서 확인한다. 실행 가능한 사용 예시는
[공통 샘플](../common/sample/README.ko.md)과
[Node 샘플](../../../languages/node/samples/README.ko.md)에서 확인할 수 있다.

서버 framework와 별도로 사용하는 client library의 사용법은
[HTTP client 가이드](guide/http-client/README.ko.md)와
[Stream connector 가이드](guide/stream-connector/README.ko.md)에서 확인한다.

## 2. 공개 계약 spec

NestJS 표면의 **정식 계약**이다. 현재 Node 코드와 regression test에 존재하는 public
API만 설명한다.

| 문서 | 범위 |
|------|------|
| [system-structure](../common/spec/server/languages/node/01-system-structure.ko.md) | 패키지 구조, NestJS 등록, lifecycle과 startup validation |
| [인터페이스 목차](../common/spec/server/languages/node/interfaces/README.ko.md) | 범주별 interface·decorator·context·options 카탈로그 |

**기능의 의미와 동작 규칙은 [공통 스펙](../common/spec/README.ko.md)이 소유한다.** 언어별 문서는
그 의미가 Node/NestJS에서 어떤 모양인지만 고정한다.

## 3. 내부 기준 (`internals/`)

`internals/`는 유지보수자를 위한 backend 의존, 내부 lifecycle과 회귀 기준을
정의한다. 공개 API와 허용 조합은 spec에서 확인한다.

| 문서 | 범위 |
|------|------|
| [backend-dependency-policy](internals/backend-dependency-policy.ko.md) | backend 교체 가능성, public surface 격리 |
| [공통 내부 구조](../common/internals/README.ko.md) | 네 언어가 공유하는 runtime 아키텍처 결정 |
| [regression-test-matrix](internals/regression-test-matrix.ko.md) | 회귀 테스트 기준 |

## 4. 공통 샘플

정본 6종의 서버 역할, 메시지 계약, 상태 전이와 완료 기준은
[공통 샘플](../common/sample/README.ko.md)이 소유한다. Node.js 문서는 이 계약을 다시
서술하지 않는다.

## 5. 회귀 테스트

이 README 는 아래 문서 회귀 테스트와 함께 유지한다.

| 테스트 | 확인 기준 |
|--------|-----------|
| `documentation-regression.test.js › node README does not link removed legacy guide chapters` | 삭제한 이전 guide 링크를 다시 추가하지 않는다. |
| `documentation-regression.test.js › node documentation relative markdown links resolve` | 문서 간 상대 링크가 깨지지 않는다. |
| `documentation-regression.test.js › node interface specification documents the current execution-turn APIs` | 실행 turn API가 정식 interface spec과 맞는지 확인한다. |
| `sample-regression.test.js › node samples define required files and use only common sample documents` | 샘플 구현이 공통 sample 문서만 참조하고 삭제한 언어별 sample README를 다시 만들지 않는지 확인한다. |
