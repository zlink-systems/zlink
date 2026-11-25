[English](./framework-java-0.34.2.md) | [한국어](./framework-java-0.34.2.ko.md)

# ZLink Java Framework 0.34.2 릴리스 노트

Framework 0.34.2는 0.34.1에서 Kotlin board-tables 템플릿을 깨뜨린 결함을 고치는 hotfix입니다. Java binding 1.26.0과 Core 1.26.0을 그대로 사용합니다.

## 결함 수정

- Actor가 다른 node로 옮겨 간 뒤(예: 다른 node의 Spot에 Join), bound session이 없던 Actor는 원래 node에 옛 사본이 남았습니다. 그래서 원래 node에서 같은 Actor ID를 다시 만들거나 가져오면(예: 다른 node가 멈춘 뒤 player가 재접속) "actor already exists"로 실패했습니다. 이제 이동이 commit될 때 원래 node가 사본을 지우고, 새 생성은 이전 generation이 완전히 떠난 뒤에 진행하며, 이전 generation으로 온 늦은 메시지는 새 Actor가 아니라 Message Follow route로 보냅니다. (#5573, #5585)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.34.2")
    implementation("systems.zlink:zlink-http-client:0.34.2")
}
```

릴리스 태그는 [`framework-java/v0.34.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.34.2)입니다.
