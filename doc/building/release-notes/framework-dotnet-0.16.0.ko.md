[English](./framework-dotnet-0.16.0.md) | [한국어](./framework-dotnet-0.16.0.ko.md)

# ZLink .NET Framework 0.16.0 릴리스 노트

Framework 0.16.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Select-one channel에서 eligibility와 drain 조건을 적용한 뒤 남은 member가 하나도 없으면 `Unavailable`로 끝납니다. Request와 one-way send가 같은 kind입니다. Weight가 `0`이거나 draining이어서 후보에서 빠진 경우가 여기에 해당하며, 송신 경로와 connection은 그대로 있으므로 `NotFound`가 아닙니다. 이전에는 언어마다 답이 달랐습니다.
- Channel 호출이 node-direct 매퍼를 타면서 request와 send 모두 `NotFound`로 끝나던 문제를 고쳤습니다. Node-direct 호출은 그대로 `NotFound`입니다.

## 설치

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.16.0
```

릴리스 태그는 [`framework-dotnet/v0.16.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.16.0)입니다.
