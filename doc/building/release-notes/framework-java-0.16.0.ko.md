[English](./framework-java-0.16.0.md) | [한국어](./framework-java-0.16.0.ko.md)

# ZLink Java Framework 0.16.0 릴리스 노트

Framework 0.16.0는 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Select-one channel에서 eligibility와 drain 조건을 적용한 뒤 남은 member가 하나도 없으면 `Unavailable`로 끝납니다. Request와 one-way send가 같은 kind입니다. Weight가 `0`이거나 draining이어서 후보에서 빠진 경우가 여기에 해당하며, 송신 경로와 connection은 그대로 있으므로 `NotFound`가 아닙니다. 이전에는 언어마다 답이 달랐습니다.
- Java와 Kotlin은 이미 이 kind로 끝내고 있었으므로 동작이 바뀌지 않습니다.

## 고친 문제

- Auto-connect loop의 종료가 진행 중이던 tick을 기다리지 않고 reconciler를 종료해, 같은 맵을 동시에 순회하고 변경하던 문제를 고쳤습니다.
- `zlink-framework-testkit`이 컴파일되지 않던 문제를 고쳤습니다. 가짜 publisher 소켓이 `setNoDrop`을 구현하지 않았습니다.

## 설치

```kotlin
implementation("systems.zlink:zlink-framework-core:0.16.0")
```

릴리스 태그는 [`framework-java/v0.16.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.16.0)입니다.
