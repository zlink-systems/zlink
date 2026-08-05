# 9. 에러 모델

> [공통 계약 목차](README.ko.md)

HTTP client는 자체 예외 계층을 만들지 않고 Framework 공용 오류를 사용한다.

## 9.1 공통 kind 집합 (계약)

| Kind | 상황 |
| --- | --- |
| `ProtocolError` | Builder 형식, body 소스 중복, typed decode, 압축 해제 또는 redirect 형식이 올바르지 않다. |
| `Unavailable` | Network, DNS, proxy CONNECT 또는 target 연결을 현재 사용할 수 없다. |
| `CapacityExceeded` | 설정한 response body byte 제한을 넘었다. |
| `DeadlineExceeded` | 시도당 timeout을 넘었다. |
| `InternalFailure` | Typed 제출의 HTTP status가 400 이상이거나 위 kind로 분류할 수 없는 실행 실패다. |

호출자 cancellation은 Framework 오류로 바꾸지 않고 각 언어의 cancelled awaitable로
전달한다. [Redirect와 retry](06-redirect-retry-cookie.ko.md)의 자동 retry 정책은 HTTP client가
설정된 한 operation 안에서 적용하는 동작이며 public 오류의 재시도 hint가 아니다.

## 9.2 언어별 표현

| 언어 | Kind | Timeout 표현 | 전달 형태 |
| --- | --- | --- | --- |
| C++ | Framework 공통 enum | `deadline_exceeded` | `result_t` 또는 예외 |
| .NET | `ZLinkFrameworkErrorKind` | `DeadlineExceeded`와 inner `TimeoutException` | `ZLinkFrameworkException` |
| Node.js | Framework 공통 kind | `DeadlineExceeded`와 `TimeoutError` cause | 예외 |
| Java | Framework 공통 enum | `DEADLINE_EXCEEDED`와 `HttpTimeoutException` cause | 예외 |
| Kotlin | Java 계약을 Kotlin 표기로 투영한다. | Java와 같다. | 예외 |

`closed`는 HTTP client error kind가 아니다. 응답 body stream이나 transport handle이
닫힌 상태는 해당 객체의 boundary 상태로 보고, 위 kind 가운데 실제 실패 원인에
맞는 kind로 변환한다.

## 9.3 자동 retry와 오류 표면의 분리

Public exception과 result에는 재시도 여부를 넣지 않는다. `retry(attempts)`가 설정된 operation은
전송 실패와 timeout만 내부에서 다시 시도한다. 모든 시도가 끝나면 마지막 실패의 `ErrorKind`를
반환하며, Application이 새 operation을 시작할지는 공통 오류 모델에 따라 판단한다.
