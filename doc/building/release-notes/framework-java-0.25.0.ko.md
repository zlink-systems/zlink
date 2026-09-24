[English](./framework-java-0.25.0.md) | [한국어](./framework-java-0.25.0.ko.md)

# ZLink Java·Kotlin Framework 0.25.0 릴리스 노트

Framework 0.25.0은 Java·Kotlin binding 1.7.0과 Core 1.7.0을 사용합니다.

## 수정

- Core 1.7.0은 libstdc++을 정적으로 링크한 host가 `dlopen()`으로 `libzlink`를 읽을 때 발생하던 static TLS 부족 문제를 해결합니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- samples README가 IntelliJ IDEA 안내를 소스 저장소 기준으로 명시합니다 ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.25.0")
    implementation("systems.zlink:zlink-http-client:0.25.0")
}
```

릴리스 태그는 [`framework-java/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.25.0)입니다.
