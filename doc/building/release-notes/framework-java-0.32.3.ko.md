[English](./framework-java-0.32.3.md) | [한국어](./framework-java-0.32.3.ko.md)

# ZLink Java·Kotlin Framework 0.32.3 릴리스 노트

Framework 0.32.3은 0.32.2의 결함 두 개를 고치는 hotfix입니다. Java·Kotlin binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- binding이 이미 닫은 Actor를 destroy하면 정상 완료 대신 예외로 끝났습니다. 이용자가 연결을 끊은 뒤 Actor를 정리하는 흐름에서 오류가 남았습니다. 이제 이미 닫힌 Actor의 destroy는 정상 완료합니다. (#4465, #4481)
- Stream connector가 연결마다 스레드를 새로 만들었습니다. 접속이 많으면 스레드 수가 연결 수만큼 늘었습니다. 이제 연결들이 공유 실행 자원을 쓰고, heartbeat timer 취소도 연결 수와 관계없이 일정한 비용으로 끝납니다. (#4487, #4493)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.3")
    implementation("systems.zlink:zlink-http-client:0.32.3")
}
```

릴리스 태그는 [`framework-java/v0.32.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.3)입니다.
