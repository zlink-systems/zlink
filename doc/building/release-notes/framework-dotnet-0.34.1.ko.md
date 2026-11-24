[English](./framework-dotnet-0.34.1.md) | [한국어](./framework-dotnet-0.34.1.ko.md)

# ZLink .NET Framework 0.34.1 릴리스 노트

Framework 0.34.1은 0.34.0 템플릿을 막던 결함을 고치는 hotfix입니다. .NET binding 1.26.0과 Core 1.26.0을 그대로 사용합니다.

## 결함 수정

- Actor와 bound session이 서로 다른 node에 있을 때, 같은 Actor가 같은 session으로 순서대로 보낸 push가 client에 역순으로 도착할 수 있었습니다. 0.34.0에서 생긴 회귀입니다. 이제 relay된 push를 받은 순서 그대로 그 session의 송신 순서에 넣습니다. (#5459, #5469)
- 클라이언트가 연달아 보낸 send가 Actor에 역순으로 도착하고, 먼저 보낸 메시지가 `stale_target`으로 버려질 수 있었습니다. 이제 수신 경계에서 도착 순서대로 Actor 순서에 등록합니다. wire는 바뀌지 않습니다. (#5497)
- Host relocation으로 SpotWide와 함께 옮겨 온 Actor가 새 소유 node에서 `LeaveActorAsync`를 부르면 "not tracked by this location owner"로 실패했습니다. 이제 옮겨 온 Actor를 새 owner가 추적합니다. 핸들러 예외가 `no_handler`로 잘못 기록되던 것도 `handler_exception`으로 바로잡았습니다. (#5480)
- 위 이동 직후 Leave가 relocation target의 staging 경계에서 간헐적으로 거절되거나 멈출 수 있었습니다. 이제 target lifecycle 작업을 queue 한 곳에서 받고, SpotWide replay를 Actor 실행 범위 안에서 실행합니다. (#5526)
- 두 번 이동한 Actor의 첫 요청이 bind 검증 때 채운 오래된 route cache로 가서 실패할 수 있었습니다. 이제 authority 확인용 Store 읽기는 route cache를 채우지 않습니다. (#5491)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.34.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.34.1" />
```

릴리스 태그는 [`framework-dotnet/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.34.1)입니다.
