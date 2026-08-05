# Python framework 개발 원칙

이 문서는 향후 Python framework를 구현할 때 적용할 개발 원칙을 정의한다. 현재 Python
framework의 공개 계약이나 구현 완료를 뜻하지 않는다. 공개 동작과 정확한 인터페이스는 구현 전에
[`framework/doc/framework/common/spec/`](../../doc/framework/common/spec/) 아래 정식 spec과 Python 언어별
인터페이스 문서에 먼저 기록한다. 사용자 guide와 internals도 `framework/doc/` 아래에 작성한다.

Python framework는 [Python 바인딩 구현 청사진](../../../bindings/doc/spec/python/README.ko.md)의
공개 API만 사용한다. 바인딩의 `_runtime`, `_native`, 네이티브 handle과 비공개 확장 객체에 직접
접근하지 않는다.

## 1. 기본 방향

Python framework는 엄격한 정적 타입을 제공하면서 Python의 간결한 표현과 동적 실행 모델을
유지한다. framework 구현과 공개 패키지는 타입 검사를 엄격하게 통과해야 한다. 사용자는 IDE의
자동 완성, 시그니처 안내와 정적 오류 검사를 받을 수 있어야 하지만, 타입을 제공하기 위해 다른
언어의 계층 구조와 호출 방식을 그대로 옮기지는 않는다.

엄격한 타입은 다음을 뜻한다.

- 모든 공개 함수, 메서드, 속성, decorator, handler, 콜백과 비동기 API의 인자와 반환
  타입을 선언한다.
- 공개 타입에서 암시적인 `Any`가 발생하지 않으며, 제네릭을 통해 메시지, 상태, 문맥과
  응답 타입이 호출 지점까지 보존된다.
- framework 내부도 엄격한 타입 검사 대상이다. FFI, serializer와 외부 패키지 경계에서 생기는
  동적 값은 좁은 어댑터 안에서 검증하고 구체 타입으로 변환한다.
- 배포 패키지에 `py.typed`를 포함하고, 설치된 wheel을 사용하는 외부 프로젝트에서도 타입
  정보가 유지된다.

타입 annotation은 런타임 입력 검증과 다르다. 네트워크 payload, 설정, metadata와 외부 저장소
결과는 annotation 유무와 관계없이 계약에 따라 검증한다. annotation을 handler 등록이나 serializer
선택에 사용하려면 그 의미와 누락 시 오류를 Python 정식 spec에 먼저 고정한다.

## 2. 지원 Python 버전

첫 구현의 최소 지원 버전은 **Python 3.12**로 고정한다. Python 3.12 이상에서 제공하는 현대적인
타입 문법을 사용하면서도 네이티브 wheel과 운영 환경의 범위를 지나치게 줄이지 않기 위한 기준이다.

- `pyproject.toml`에는 `requires-python = ">=3.12"`를 선언한다.
- CI는 Python 3.12와 당시 지원되는 최신 안정 버전을 포함한다. 중간의 지원 버전도 wheel
  또는 source 설치 검사에서 빠지지 않게 조합을 구성한다.
- formatter, linter와 정적 타입 검사기도 Python 3.12를 최소 분석 버전으로 사용한다.
- 최소 버전을 올릴 때는 패키지 metadata, wheel build 조합, 설치 검사, guide와 release
  note를 같은 변경에서 갱신한다.
- Python 3.12보다 낮은 버전을 위한 조건 분기, 호환 패키지와 별도 공개 API는 만들지 않는다.

## 3. 공개 타입 설계

공개 API는 타입 정보를 완성하되 호출 형태는 Python 관례를 따른다.

- 공개 이름은 `snake_case`, 타입 이름은 `PascalCase`를 사용한다.
- 입력 collection은 구현이 허용하는 가장 넓은 `collections.abc` 계약을 사용한다. framework가
  새 결과를 만들어 반환하면 `list`, `dict` 또는 명명된 구체 값 타입을 반환한다.
- 설정, 변경할 수 없는 snapshot, 메시지 DTO와 result는 dataclass, enum, `TypedDict` 또는 명명된 값
  타입으로 표현한다. 구조가 정해진 데이터를 타입 없는 `dict`로 전달하지 않는다.
- `Protocol`은 location store, codec, middleware, observer처럼 사용자가 구현을 교체할 수 있는
  확장 지점에 사용한다. framework가 직접 제공하는 모든 구체 class에 기계적으로 적용하지
  않는다.
- 제네릭은 handler 문맥의 상태, request와 reply처럼 호출자에게 실제 추론 결과를 제공할 때만
  사용한다. 타입 변수 전달만 반복하는 얕은 wrapper는 만들지 않는다.
- `Any`는 raw payload나 타입을 알 수 없는 외부 패키지 경계로 제한한다. 모든 값을 받을 수 있지만
  구체 동작을 호출하지 않는 인자에는 `object`를 우선한다.
- `cast()`와 `# type: ignore`는 정상 제어 흐름을 대신하지 않는다. 필요한 경우 이유와 무시할
  진단 코드를 좁은 범위에 기록하고, CI에서 사용하지 않는 ignore를 오류로 처리한다.

## 4. Handler와 비동기 API

네트워크와 Actor 작업처럼 대기가 포함되는 framework 공개 API는 `async`/`await`를 기본 형태로
제공한다. `async def`의 반환 annotation에는 coroutine 객체가 아니라 `await` 결과 타입을 적는다.
취소와 timeout은 Python Task 의미와 framework의 공통 계약을 함께 만족해야 하며, 취소를 일반
오류로 바꾸거나 조용히 무시하지 않는다.

Handler의 context, message와 reply 경계는 명시적으로 타입을 선언한다. framework가 annotation을
읽어 handler 종류나 message 타입을 정하는 경우에는 다음을 지킨다.

- 등록 시점에 annotation 누락과 지원하지 않는 타입을 명확한 오류로 거부한다.
- 정적 타입 정보만으로 네트워크 payload가 안전하다고 간주하지 않고 decode 결과를 검증한다.
- forward reference, 제네릭의 구체 타입 지정과 decorator가 정적 타입 검사기에서도 같은 시그니처로
  보이게 한다.
- 콜백 기반 바인딩 완료는 framework 내부에서 event loop의 awaitable 결과로 변환한다. 네이티브
  콜백 thread에서 `Future`나 `Task`를 직접 변경하지 않고 event loop의 thread-safe 진입점을
  사용한다.

## 5. Python 관례 유지

엄격한 타입을 이유로 Python 코드를 다른 언어의 형태로 바꾸지 않는다.

- 모든 구체 class에 인터페이스를 하나씩 대응시키지 않는다.
- 단순한 attribute를 getter/setter 메서드 쌍으로 감싸지 않는다.
- overload 수를 늘리기 위한 메서드 이름 변형이나 형식적인 builder 단계를 추가하지 않는다.
- 지역 변수의 타입을 추론할 수 있으면 같은 타입을 반복해서 적지 않는다.
- 타입 체계로 표현하기 어렵다는 이유만으로 framework 내부 책임을 사용자 옵션이나 helper로
  이동하지 않는다.
- decorator, context manager, async iterator, dataclass와 pattern matching은 공개 계약을 더 간단하게
  만들 때 사용한다. 문법 사용 자체를 목표로 삼지 않는다.

## 6. 구현과 리뷰 gate

Python framework 구현을 시작하기 전에 정식 spec에 정확한 Python interface와 타입 변수를 먼저
기록한다. 구현 완료 여부는 다음 gate로 판정한다.

- `pyright`를 strict mode로 실행해 framework source와 typing test가 통과한다.
- 공개 API의 모든 인자와 반환값이 알려진 타입이며 타입 완전성 검사에서 누락이 없다.
- `py.typed`가 wheel과 sdist에 포함되고, 깨끗한 virtual environment에서 설치한 패키지를 대상으로
  외부 프로젝트 타입 검사가 통과한다.
- Python 3.12부터 당시 최신 안정 버전까지 unit test와 package smoke가 통과한다.
- 공개 handler, decorator와 제네릭 API에 대해 정상 추론과 의도한 오류를 `assert_type` 기반
  타입 검사 전용 test로 검증한다.
- `Any`, `cast()`, `# type: ignore`, `runtime_checkable` 사용을 리뷰하고 각 사용이 필요한 경계에만
  남아 있는지 확인한다.
- 타입 annotation과 런타임 validation 결과가 충돌하지 않는지 잘못된 payload와 잘못된 handler
  등록 test로 검증한다.
- binding 비공개 모듈 import와 native handle 접근이 없음을 검색한다.

리뷰는 공개 API 사용성, 타입 정확성, 런타임 동작의 세 축으로 진행한다. 타입 검사가 통과해도
호출 형태가 불필요하게 복잡하거나 Python 관례와 맞지 않으면 완료하지 않는다. 반대로 실행 test가
통과해도 공개 타입에 암시적인 `Any`가 남거나 설치 패키지에서 타입 정보가 사라지면 완료하지 않는다.
