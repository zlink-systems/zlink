[English](./framework-java-0.32.2.md) | [한국어](./framework-java-0.32.2.ko.md)

# ZLink Java·Kotlin Framework 0.32.2 릴리스 노트

Framework 0.32.2는 0.32.1의 결함 하나를 고치는 hotfix입니다. Java·Kotlin binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- Spot actor handler 목록을 runtime 하나가 공유했고, Spot 인스턴스가 활성화될 때마다 잠금 없이 이 목록에 썼습니다. 여러 Spot 종류의 첫 인스턴스가 동시에 활성화되면 활성화가 `ConcurrentModificationException`으로 실패하거나 handler가 빠져, 이후 Actor 요청이 `actor packet handler is not registered`로 끝날 수 있었습니다. 이제 각 Spot 인스턴스가 자기 actor handler 목록을 가지며, dispatch는 대상 인스턴스의 목록을 읽습니다. (#4389, #4391)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.2")
    implementation("systems.zlink:zlink-http-client:0.32.2")
}
```

릴리스 태그는 [`framework-java/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.2)입니다.
