[English](./framework-java-0.29.0.md) | [한국어](./framework-java-0.29.0.ko.md)

# ZLink Java·Kotlin Framework 0.29.0 릴리스 노트

Framework 0.29.0은 Java·Kotlin binding 1.17.0과 Core 1.17.0을 사용합니다. (#1554)

## 계약 변경

- Missing Instance Spot의 cold activation은 Core request/reply로 target에 제출합니다. target이 예약과 활성화를 소유합니다. Reserve에서 경쟁에 진 요청은 다른 target으로 전달하거나 winner의 완료를 기다리지 않고 `Unavailable` 한 번으로 끝납니다. type을 정할 serving node가 없으면 `NotFound`, 생략한 type 후보가 여러 개면 `InvalidOperation`, capacity가 부족하면 `Unavailable`을 반환합니다. (#1467)
- Spot·Actor·handler 등록의 생성자 의존성을 optional·collection·이름/qualifier 선택 규칙에 따라 host 시작 때 선언 metadata로 검증합니다. 필수 의존성이 없거나 선택이 모호하면 시작이 실패합니다. 명시적 factory는 실행하지 않습니다. Spring은 `@Qualifier`와 parameter name으로 의존성을 선택합니다. (#1549)
- target을 다시 시작할 때 이전 lifecycle에 속한 Creating 예약을 recovery 단계에서 해제하고 pending capacity를 반환합니다. (#1532)

## 변경

- Stream node에 heartbeat 간격·제한 시간과 idle 종료 시간을 설정하는 `heartbeat`와 `idleTimeout`을 추가했습니다. heartbeat 기본값은 1초·5초입니다. idle timeout 기본값은 0이며, 이 값에서는 유휴 종료가 비활성화됩니다. heartbeat control frame은 애플리케이션 활동으로 계산하지 않습니다. Spring 앱은 같은 Java builder 설정을 사용합니다. (#1538, #1541)
- Spring starter가 공개 runtime 상태 bean을 제공합니다. 앱은 internal lifecycle 구현에 의존하지 않고 runtime 상태를 조회하고 관찰할 수 있습니다. (#1550)

## 결함 수정

- Store provider가 실패하면 Spot Send·Request에서 공개 오류 `Unavailable`을 반환합니다. provider 원인은 보존합니다. (#1466)
- Core 1.17.0은 WS·WSS의 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. TCP batching 동작은 유지합니다. (#1545)
- Java STREAM monitor 대기가 virtual thread의 carrier를 점유해 `bind()`와 state lane 진행을 막던 문제를 수정했습니다. monitor 대기는 platform daemon thread에서 진행합니다. (#1546)
- relocation 중 shutdown이 stage 설치 사이에 시작되어 target terminal이 누락되던 문제를 수정했습니다. source는 queue 복원을 확정한 뒤 abort를 전송합니다. (#1490)
- Instance Spot의 Store 요구 조건은 host 시작 때 한 번 검증합니다. disabled 정책이 포함된 factory도 같은 조건을 따릅니다. (#1490)
- Spring host는 runtime 시작 실패를 컨텍스트 시작 실패로 전달합니다. (#1525)
- SpotWide Actor queue는 Spot의 공통 execution gate를 통해 소비합니다. (#1496)
- ZoneWorld 예제가 실제 owner 배치에서 시나리오 대상을 고르고, 재시작 알림을 도착 순서대로 처리합니다. (#1490)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.29.0")
    implementation("systems.zlink:zlink-http-client:0.29.0")
}
```

릴리스 태그는 [`framework-java/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.29.0)입니다.
[한국어](./framework-java-0.29.0.ko.md) | [English](./framework-java-0.29.0.md)
