# zlink 소스 주석 원칙

> 이 문서는 zlink 코드 안에 작성하는 공개 API 주석과 내부 주석의 공통 기준을
> 정의한다. 언어별 문법은 달라도 주석이 설명해야 하는 내용과 피해야 하는 내용은
> 같아야 한다.

---

## 핵심 원칙

공개 API 주석은 계약 문구다. 호출자가 API를 올바르게 쓰기 위해 알아야 하는 공개
동작만 설명한다. 구현 세부 사항이나 현재 최적화 방식은 공개 계약처럼 적지 않는다.

주석은 코드를 반복해서 읽어 주는 문장이 아니다. 함수 이름, 매개변수 이름, 타입만
봐도 알 수 있는 내용은 쓰지 않는다. 대신 호출자가 틀리기 쉬운 계약, 수명, 오류,
대기 동작, 콜백 동작을 분명히 적는다.

## 공개 API 주석

public으로 보이는 type, function, method, property, field에는 해당 언어의 표준
API reference 주석을 사용한다.

| 언어 | 공개 API 주석 형식 |
|------|-------------------|
| C/C++ | header comment 또는 Doxygen 형식 |
| C#/.NET | XML documentation comment |
| Java | Javadoc |
| Node/TypeScript | TSDoc |
| Python | docstring |
| Go | godoc comment |
| Rust | rustdoc comment |

공개 API 주석은 아래 내용을 우선적으로 드러낸다.

- payload, buffer, handle, message의 소유권, 해제, 소유권 이동.
- 빌린 view와 복사된 buffer의 차이.
- blocking, non-blocking, timeout, cancellation 동작.
- callback 등록, 해제, 호출 시점, 재진입 가능 여부.
- boolean 반환값, result enum, error code, exception의 의미.
- staged operation builder처럼 다음에 호출할 수 있는 메서드가 계약 일부인 흐름.
- 호출자가 계약을 지키면 피할 수 있는 예외와 실패 조건.

## 주석 형태

- 공개 API reference 주석은 영어로 작성한다. 코드 생성 문서와 외부 사용자가
  같은 문구를 읽기 때문이다.
- summary는 짧고 호출자 관점으로 작성한다.
- 한 줄 summary에 담기 어려운 계약 세부 사항만 remarks, details, note 같은
  언어별 확장 구역에 적는다.
- `owns`, `borrows`, `copies`, `transfers`, `disposes`처럼 소유권을 명확히
  드러내는 표현을 우선 사용한다.
- overload 차이를 설명해야 하는 경우가 아니면 메서드 이름을 prose로 반복하지
  않는다.
- 현재 구현 최적화, 임시 우회 경로, private 자료구조를 공개 보장처럼 설명하지
  않는다.

## 내부 주석

내부 주석은 코드만으로 의도를 파악하기 어려운 결정에만 작성한다.

- 복잡한 동시성, 수명, 오류 마스킹, 프로토콜 경계처럼 잘못 바꾸기 쉬운 결정을
  설명한다.
- 코드가 무엇을 하는지 한 줄씩 반복하지 않는다.
- 내부 구현 설명은 public API 주석이 아니라 runtime 주석이나 `doc/internals/`에
  둔다.
- 임시 상태를 남기는 주석은 제거 조건이나 관련 이슈를 함께 적는다. 단순히
  `TODO: fix later`처럼 끝나는 주석은 남기지 않는다.

## Guide와의 분리

API reference 주석은 튜토리얼이 아니다. 각 type 또는 member의 정확한 계약만
설명한다.

사용 패턴, 예제, 목적 설명, 설계 배경은 `doc/guide/` 또는 해당 언어 guide 문서에
둔다. 공개 멤버에 긴 배경 설명이 필요하면 주석은 짧게 유지하고 guide 문서에서
자세히 설명한다.

## 리뷰 체크리스트

공개 API나 public contract가 바뀌면 코드와 함께 주석도 검토한다.

- 호출자가 반환된 resource, message, buffer의 소유자를 알 수 있는가?
- borrowed view와 copied buffer의 차이가 분명한가?
- timeout, cancellation, callback, blocking 여부가 설명되어 있는가?
- false, result enum, error code, exception 의미가 설명되어 있는가?
- runtime 내부 세부 사항을 공개 계약처럼 적지 않았는가?
- 사용법 설명이 길어졌다면 guide로 옮겼는가?
- 해당 언어의 API 문서 생성 또는 빌드 경고 검증을 통과하는가?
