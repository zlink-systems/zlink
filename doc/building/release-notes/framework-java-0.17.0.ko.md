[English](./framework-java-0.17.0.md) | [한국어](./framework-java-0.17.0.ko.md)

# ZLink Java Framework 0.17.0 릴리스 노트

Framework 0.17.0는 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 묶인 session이 없는 Actor에서 bound session으로 보낼 때의 결과를 다섯 언어가 같게 맞췄습니다. 유효한 binding이 없으면 `InvalidOperation`으로 끝나고, 그 실패는 다른 호출 실패와 같이 **호출의 terminal**에서 관측합니다. 호출 객체를 만드는 자리에서 던지지 않습니다.
- `boundSession()`이 binding이 없을 때 그 자리에서 던지던 것을 고쳤습니다. 이제 `CompletionStage`로 실패를 관측할 수 있고 accessor를 `try`/`runCatching`으로 감쌀 필요가 없습니다. Kotlin도 같습니다.

## 설치

```kotlin
implementation("systems.zlink:zlink-framework-core:0.17.0")
```

릴리스 태그는 [`framework-java/v0.17.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.17.0)입니다.
