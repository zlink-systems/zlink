[English](./framework-dotnet-0.15.0.md) | [한국어](./framework-dotnet-0.15.0.ko.md)

# ZLink .NET Framework 0.15.0 릴리스 노트

Framework 0.15.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- `IZLinkFanoutHandler<TEvent>.HandleAsync`가 publish context를 함께 받습니다. 시그니처가 `HandleAsync(TEvent message, ZLinkPublishMessageContext context, CancellationToken cancellationToken)`이며, handler 구현은 인자를 추가해야 합니다. Java·Node와 같은 표면이 되었고, publisher가 정한 topic을 handler에서 읽을 수 있습니다.

## 주요 변경

- Classic fanout publisher에 `SetNoDrop` 설정을 더했습니다. 켜면 topic이 일치하는 pipe 전체에 대해 전부 보내거나 하나도 보내지 않으며, 보낼 수 없으면 `DeadlineExceeded`로 끝납니다.
- Fanout subscriber가 받을 topic을 startup에 `Subscribe`로 등록합니다. 등록한 topic의 byte prefix 합집합을 받으며, 등록이 없으면 빈 topic만 받습니다.
- Fanout publish가 local admission을 기다린 뒤 결과를 돌려줍니다. 기다린 시간이 send timeout을 넘으면 `DeadlineExceeded`입니다. Binding의 send timeout이 Core의 `SNDTIMEO`로 이어집니다.
- Descriptor를 처음 받아들이는 자리에서 owner의 생존을 판단합니다. 죽은 node의 routing id를 peer로 받지 않습니다.
- Wildcard bind host에서 `SetAdvertiseHost`를 생략하면 같은 address family의 loopback을 광고합니다. `0.0.0.0`은 `127.0.0.1`, `::`은 `::1`입니다.
- Admission을 마치고 RID가 확정된 peer를 node-direct 대상으로 지정할 수 있습니다.

## 설치

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.15.0
```

릴리스 태그는 [`framework-dotnet/v0.15.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.15.0)입니다.
