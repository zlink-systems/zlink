[English](./framework-java-0.32.1.md) | [한국어](./framework-java-0.32.1.ko.md)

# ZLink Java·Kotlin Framework 0.32.1 릴리스 노트

Framework 0.32.1은 0.32.0의 결함 하나를 고치는 hotfix입니다. Java·Kotlin binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- Instance Spot cold activation, 신규 Actor 배치와 ClientServer 후보 선택이 Location Store의 owner lease를 확인하지 않아, 강제 종료된 host를 고를 수 있었습니다. 이제 lease가 살아 있는 descriptor 목록에서만 후보를 고릅니다. (#4205, #4220)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.1")
    implementation("systems.zlink:zlink-http-client:0.32.1")
}
```

릴리스 태그는 [`framework-java/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.1)입니다.
