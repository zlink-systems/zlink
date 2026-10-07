[English](./framework-dotnet-0.29.0.md) | [한국어](./framework-dotnet-0.29.0.ko.md)

# ZLink .NET Framework 0.29.0 릴리스 노트

Framework 0.29.0은 .NET binding 1.17.0과 Core 1.17.0을 사용합니다. (#1554)

## 계약 변경

- Missing Instance Spot의 cold activation은 Core request/reply로 target에 제출합니다. target이 예약과 활성화를 소유합니다. Reserve에서 경쟁에 진 요청은 다른 target으로 전달하거나 winner의 완료를 기다리지 않고 `Unavailable` 한 번으로 끝납니다. type을 정할 serving node가 없으면 `NotFound`, 생략한 type 후보가 여러 개면 `InvalidOperation`, capacity가 부족하면 `Unavailable`을 반환합니다. (#1467)
- Spot·Actor·handler 등록의 생성자 의존성을 optional·collection·이름/qualifier 선택 규칙에 따라 host 시작 때 선언 metadata로 검증합니다. 필수 의존성이 없거나 선택이 모호하면 시작이 실패합니다. .NET은 keyed 서비스와 `IServiceProviderIsService` metadata를 사용하며 명시적 factory는 실행하지 않습니다. (#1549)
- target을 다시 시작할 때 이전 lifecycle에 속한 Creating 예약을 recovery 단계에서 해제하고 pending capacity를 반환합니다. (#1532)

## 변경

- Stream node에 heartbeat 간격·제한 시간과 idle 종료 시간을 설정하는 `SetHeartbeat`와 `SetIdleTimeout`을 추가했습니다. heartbeat 기본값은 1초·5초입니다. idle timeout 기본값은 0이며, 이 값에서는 유휴 종료가 비활성화됩니다. heartbeat control frame은 애플리케이션 활동으로 계산하지 않습니다. (#1538, #1541)

## 결함 수정

- Store provider가 실패하면 Spot Send·Request에서 공개 오류 `Unavailable`을 반환합니다. provider 원인은 보존합니다. (#1466)
- Core 1.17.0은 WS·WSS의 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. TCP batching 동작은 유지합니다. (#1545)
- .NET은 고정 RoutingId 충돌을 설정 오류로 분류하고 중복 Store 조회를 제거합니다. (#1525)
- Actor가 Entry Spot으로 돌아올 때 source leave 알림은 one-way로 제출하며, Join coordinator가 처리 순서를 소유합니다. callback 조회가 이미 정리된 Actor scope에 의존하지 않습니다. (#1490)
- target 종료 중 수락된 authority terminal 기록은 Store completion을 마친 뒤 해제합니다. (#1083)
- SpotWide Actor queue는 Spot의 공통 execution gate를 통해 소비합니다. (#1496)
- ZoneWorld 예제가 실제 owner 배치에서 시나리오 대상을 고르고, 재시작 알림을 도착 순서대로 처리합니다. (#1490)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.29.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.29.0" />
```

릴리스 태그는 [`framework-dotnet/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.29.0)입니다.
[한국어](./framework-dotnet-0.29.0.ko.md) | [English](./framework-dotnet-0.29.0.md)
