[English](./framework-dotnet-0.32.0.md) | [한국어](./framework-dotnet-0.32.0.ko.md)

# ZLink .NET Framework 0.32.0 릴리스 노트

Framework 0.32.0은 .NET binding 1.23.0과 Core 1.23.0을 사용합니다. (#3843)

## 동작 변경

- 알 수 없는 control packet이나 heartbeat payload version을 받아도 연결을 닫지 않습니다. (#3773, #3790)
- Actor owner가 relay된 session sequence의 중복·역순을 거부합니다. stale-relay record의 `source_rid`에는 session owner node를 기록합니다. (#1721, #3763, #3799)
- `MessageContext.receivedAt`은 Framework가 message를 받은 UTC 시각입니다. 기본값이 없는 DTO property는 framework-json-v1에서 필수이며, 중복 envelope header field는 거부됩니다. (#2029, #3602, #1705, #3592, #3376, #3396)
- 중복 bind는 owner Actor의 `onDuplicateBind` callback이 판정합니다. (#2205, #3640)

## 결함 수정

- 늦게 도착한 physical 결과는 fence가 일치할 때만 route cache 항목을 무효화하며, cache 무효화는 08-routing §2의 trigger 표를 따릅니다. 강제 종료 뒤 Store 정리는 deadline 하나를 쓰고 authority를 기준으로 sweep하며, 대상 lifecycle이 끝나면 reservation을 해제합니다. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- relocation terminal 결과를 message flow에 기록하고 cutover flow와 Bingo join·observer 정책을 보존합니다. (#2465, #3621)
- callback 실패에서 원래 exception을 보존합니다. correlation을 읽을 수 있는 잘못된 request에는 terminal protocol error를 응답합니다. (#2098, #3429, #2512, #3418)
- Instance-intent request는 admission 전에 stale route를 거부하고 request deadline 안에서 현재 authority를 따릅니다. disconnect가 실패해도 liveness 정리가 peer 정리와 통지를 마칩니다. (#3353, #3442, #2370, #3434)
- Auto-connect는 같은 endpoint의 descriptor만 자기 자신으로 제외하며, 게시가 끝난 뒤 peer를 표시합니다. 자기 descriptor 게시가 실패하면 peer를 목록에 넣지 않습니다. (#2155, #3433, #3569, #3664, #3723, #3736)
- binding 완료 뒤 도착한 terminal reply는 Actor slot을 해제하고 Session reply로 보냅니다. Close 중에도 이미 수락한 Instance intent를 유지합니다. (#2757, #3419, #3606, #3628)
- Location, publication, activation deadline에 주입된 `TimeProvider`를 사용합니다. (#3767, #3782)
- request 완료 continuation을 비동기로 실행하고 preemption 뒤에도 receive 처리를 이어갑니다. (#3610, #1674, #3413)
- receive-claim에서 authority 조회가 실패하면 해당 record만 정산해 batch의 나머지 record를 처리합니다. (#3352, #3362)

## 성능 개선

- request-response 변환을 최종 receiver에 합쳐 전달만 하던 async 층을 제거했습니다. (#1674, #3481)

## 시험과 CI

- relocation 준비 시험이 worker pool과 trace export의 관찰 순서를 동기화합니다. (#3761, #3766)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.0" />
```

릴리스 태그는 [`framework-dotnet/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.0)입니다.
