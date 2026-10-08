[English](./framework-java-0.29.1.md) | [한국어](./framework-java-0.29.1.ko.md)

# ZLink Java·Kotlin Framework 0.29.1 릴리스 노트

Framework 0.29.1은 Java·Kotlin binding 1.17.0과 Core 1.17.0을 사용합니다.

## 결함 수정

- 2코어 환경에서 Java native socket monitor 대기가 virtual-thread carrier를 점유해 route 준비와 request 처리가 멈추던 문제를 수정했습니다. Stream·Channel monitor 실행자는 공용 backend owner가 선택하며, Stream·Channel과 ClientServer control의 native 대기는 platform thread에서 진행합니다. (#1567, #1568)
- Windows Kotlin GameQuest runner가 `close-replay.release`를 만들지 않아 client와 runner가 서로 기다리던 문제를 수정했습니다. (#1568)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.29.1")
    implementation("systems.zlink:zlink-http-client:0.29.1")
}
```

릴리스 태그는 [`framework-java/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.29.1)입니다.
[한국어](./framework-java-0.29.1.ko.md) | [English](./framework-java-0.29.1.md)
