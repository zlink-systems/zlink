---
title: "오류 처리 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/http-client/11-error-handling.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 오류 처리

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 응답 처리 규칙](10-response-rules.ko.md) | [다음: Kotlin 코루틴 통합](12-coroutines.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/11-error-handling.ko.md) · [C#/.NET](../../../dotnet/guide/http-client/11-error-handling.ko.md) · [Java](../../../java/guide/http-client/11-error-handling.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/http-client/11-error-handling.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    HTTP client 실패에서 kind를 읽고, 자동 retry가 끝난 뒤 새 operation을 시작할지 판단할 수 있다.

오류 kind는 실패한 요청을 어떻게 복구할지 구분하는 닫힌 분류다. kind는 HTTP status 자체가 아니라
요청 구성, 전송, timeout, 응답 검증에서 실제로 실패한 원인을 나타낸다. 호출자 cancellation은 이
분류로 바꾸지 않고 각 언어의 cancellation 결과로 전달된다.

## 1. kind와 실패 상황

| 상황 | kind |
|---|---|
| builder 형식, body source 중복, typed JSON decode, 압축 해제 또는 redirect 형식이 올바르지 않다 | `protocolError` |
| network, DNS, proxy CONNECT 또는 target 연결을 현재 사용할 수 없다 | `Unavailable` |
| 설정한 response body byte 제한을 넘었다 | `Rejected` |
| 시도당 timeout을 넘었다 | `DeadlineExceeded` |
| typed 응답의 HTTP status가 400 이상이거나 다른 kind로 분류할 수 없는 실행 실패다 | `InternalFailure` |

언어별 enum 표기는 C++가 `protocol_error`·`unavailable`·`rejected`·`deadline_exceeded`·`internal_failure`,
.NET과 Node/TypeScript가 PascalCase, Java와 Kotlin이 `PROTOCOL_ERROR`·`UNAVAILABLE`·`REJECTED`·
`DEADLINE_EXCEEDED`·`INTERNAL_FAILURE`다.

## 2. 실패를 받고 kind를 읽는 방법

tutorial은 typed 요청의 오류 status와 닫힌 port의 연결 실패를 각각 잡아 kind를 출력한다. 종결자는
request builder가 실제 전송과 응답 대기를 시작하는 마지막 호출이며, 그 형태는 언어마다 다르다.
실패를 잡은 뒤 kind로 분기하는 판단은 같다.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-error-kinds"
```

```console
error kinds: bad request INTERNAL_FAILURE connection refused INTERNAL_FAILURE
# 0.19.0부터 connection refused는 UNAVAILABLE
```

## 3. retry 뒤에 판단할 것

자동 retry는 `Unavailable`과 `DeadlineExceeded`를 낳는 전송 실패를 설정한 operation 안에서만 다시
시도한다. 모든 시도가 끝나면 마지막 kind가 호출자에게 전달된다. `protocolError`, `Rejected`, HTTP
4xx·5xx status는 retry하지 않으며 streaming 요청도 retry 대상이 아니다.

예외와 결과에는 재시도 가능 여부를 나타내는 hint가 없다. 따라서 오류를 받은 application은 kind만으로
같은 operation을 무조건 다시 시작하지 않고, 요청이 중복 실행되어도 안전한지와 서버가 이미 처리했을
가능성을 함께 판단한다.

## 4. 자주 발생하는 문제

| 증상 | 원인 |
|---|---|
| 400 응답을 `protocolError`로 처리했다 | typed 경로의 status 400 이상은 `InternalFailure`다. 오류 payload가 필요하면 raw 응답을 사용한다. |
| 큰 JSON 응답이 network 오류처럼 보인다 | response body 한도를 넘으면 `Rejected`다. 압축 응답도 해제한 크기로 판단한다. |
| timeout 뒤 요청이 한 번 더 서버에 도달한다 | 설정한 retry가 `DeadlineExceeded`를 다시 시도했다. |
| cancellation을 Framework kind로 잡으려 한다 | 호출자 cancellation은 각 언어의 cancellation 결과로 전달된다. |

## 5. 다음 장

- 4xx·5xx payload를 raw 응답으로 읽는 방법 — [응답 처리 규칙](10-response-rules.ko.md)
- retry와 streaming을 제외하는 조건 — [Redirect·Retry·Cookie](09-redirect-retry-cookie.ko.md)
