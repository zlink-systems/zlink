[English](./framework-dotnet-0.30.2.md) | [한국어](./framework-dotnet-0.30.2.ko.md)

# ZLink .NET Framework 0.30.2 릴리스 노트

Framework 0.30.2은 .NET binding 1.20.0과 Core 1.20.0을 사용합니다.

## 변경

- request admission과 reply 완료를 분리해 수락 시점을 확인할 수 있게 했습니다. deferred Join의 deadline은 등록 clock을 따르고 cancellation 뒤 callback suspension도 관찰합니다. (#2069, #2070, #2270, #2253)
- relocation·Session·Join 경로에서 source request owner와 Session owner identity를 일관되게 사용하고, Location runtime 지표를 Store 기록 및 polling 시점에 맞췄습니다. (#2150, #2151, #2280, #2285, #1933, #1992, #1995)
- per-message Store 조회·completion 처리를 줄이고, ingress envelope header 재사용으로 작은 메시지 처리 비용을 낮췄습니다. (#1817, #1992, #2093, #2178)
- .NET의 Actor population 및 runtime metric 관측을 object/lifecycle lane과 맞추고, liveness probe를 제출 전에 집계합니다. (#2261, #2269, #2381)

## 결함 수정

- Join completion과 Actor handoff가 같은 turn을 기다려 교착하거나 순서가 뒤바뀌던 문제를 수정했습니다. 복원 frame은 Actor FIFO에 수락된 뒤 실행되며, shutdown 시 종료된 Actor를 회수합니다. (#2117, #2118, #2261, #2291)
- ClientServer·Spot·Session의 실패 분류와 binding 정리를 수정해 이미 사라진 physical RID 정리가 완료되며 잘못된 source descriptor 요청은 적절한 오류로 끝납니다. (#2114, #2132, #2137, #2147, #2391)
- Join deadline, relocation 중 route 교체, liveness 관리 중 callback 취소가 정체되거나 실패하던 문제를 수정했습니다. (#1937, #1954, #2270, #2253)
- idle worker와 timer의 불필요한 대기·CPU 사용을 줄이고, RouteMesh 선택과 head fence 평가를 정리했습니다. (#2279, #1817, #1992)
- target Join lifecycle 실패 때 원래 예외를 기존 error reporter에 기록합니다. (#2459)
- relocation rollback은 복구 frame의 FIFO 수락까지만 기다리고, 교체된 연결의 typed transport 결과를 재전송 판정에 사용합니다. (#2462)
- Bingo 관찰자가 Join 뒤 방의 현재 상태와 확정 reward를 조회해 놓친 이벤트를 받도록 sample을 수정했습니다. (#2427)
- server drain으로 닫힌 STREAM 연결의 close reason을 스펙 값 `server_shutdown`으로 기록합니다. (#2189)
- Core 1.20.0은 handshake 중 completion poller를 등록할 때 임시 transport owner의 executor가 멈추던 문제를 수정했습니다. (#2504)
- Actor reply를 STREAM frame 한 개로 보내고 오류 payload를 스펙의 `{code, message}` 형식으로 맞췄습니다. 네 언어가 같은 공유 vector로 검증합니다. (#2507)
- service wire command 21·22·24·25를 schema 생성 codec으로 읽고 써, 다른 언어와 같은 배치를 사용합니다. (#2507)
- Session bind fence를 제출된 ActorRef를 설명하는 row에서만 만들어, relocation 뒤 다른 row의 값으로 bind가 거부되던 문제를 수정했습니다. (#2507)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.2" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.2" />
```

릴리스 태그는 [`framework-dotnet/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.2)입니다.
[한국어](./framework-dotnet-0.30.2.ko.md) | [English](./framework-dotnet-0.30.2.md)
