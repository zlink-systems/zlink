[English](./framework-java-0.19.0.md) | [한국어](./framework-java-0.19.0.ko.md)

# ZLink Java·Kotlin Framework 0.19.0 릴리스 노트

Framework 0.19.0은 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- HTTP client(Java·Kotlin): server builder의 one-way `submit()`, Kotlin `await(): Unit`(one-way), Java `yieldRaw()`가 없어집니다. `submit(Class<T>)`·`submitRaw()`·`fetch(Class<T>)`·`download`·`yield(Class<T>)`와 Kotlin `await`·`awaitRaw`·`fetch`·`awaitDownload`·`yield`는 그대로입니다. (#707)
- HTTP client: 응답 없는 요청이 없으므로 **one-way 종결자를 제거**했습니다. 응답 값이 필요 없는 호출은 raw 응답 종결자의 결과를 쓰지 않습니다. 종결자 이름의 소유자는 binding 정책과 Framework Submit과 완료 §2로 일원화되고 HTTP 언어별 인터페이스 §1.4가 응답 형태를 대응시킵니다. (#707)
- HTTP client: error kind를 스펙 09장 §9.1에 맞췄습니다. 응답 body 크기 초과는 `Rejected`, redirect 형식·한도 초과는 `ProtocolError`, transport·연결 거부·proxy·TLS 실패는 `Unavailable`, timeout은 `DeadlineExceeded`입니다. kind로 분기하는 호출자는 영향을 받습니다. (#704)

## 공통 변경

- tutorial에 `HttpClient` 프로그램을 더했습니다. tutorial Client의 HTTP 표면을 HTTP client로 부르는 11단계이며, Client·Server HTTP 표면에 admin Basic auth, `GET /rooms/{id}` gzip, `GET /player/{id}` 301, `GET /rooms/{id}/export` chunked 다운로드, `POST /rooms/{id}/import` 스트리밍 업로드를 더했습니다. (#705)
- HTTP client 사용자 가이드를 다섯 언어 공통 소스(언어 탭)에서 생성하는 11장 구성으로 다시 썼습니다. 기능별 가이드 코드는 tutorial `HttpClient`의 snippet입니다. (#706)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.19.0")
    implementation("systems.zlink:zlink-http-client:0.19.0")
}
```

릴리스 태그는 [`framework-java/v0.19.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.19.0)입니다.
