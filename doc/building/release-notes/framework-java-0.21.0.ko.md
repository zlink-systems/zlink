[English](./framework-java-0.21.0.md) | [한국어](./framework-java-0.21.0.ko.md)

# ZLink Java·Kotlin Framework 0.21.0 릴리스 노트

Framework 0.21.0은 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 없음. 공개 계약은 0.20.0과 같습니다.

## 공통 변경

- google-java-format 1.27.0 전환 외 Java·Kotlin 변경은 없습니다.
- runtime과 공개 계약은 0.20.0과 같습니다. 이 릴리스는 tutorial·samples·quickstart와 저장소 도구를 정리합니다.
- quickstart·tutorial·samples는 언어별 examples 저장소(`zlink-<lang>-examples`)에서 받습니다. 미러 workflow가 실행 비트를 보존하고(#831), README 상단에 English | 한국어 선택 줄을 둡니다.
- tutorial CI의 C++ job이 vcpkg binary cache를 Actions cache에 둡니다. (#852)
- google-java-format 1.27.0으로 Java·Kotlin 포맷 검사가 JDK 25에서 그대로 돕니다. (#798)
- 사용되지 않던 v11 public-contract trace generator와 inventory를 제거했습니다. (#747)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.21.0")
    implementation("systems.zlink:zlink-http-client:0.21.0")
}
```

릴리스 태그는 [`framework-java/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.21.0)입니다.
