[English](./bindings-dotnet-1.12.0.md) | [한국어](./bindings-dotnet-1.12.0.ko.md)

# ZLink .NET binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다.

## 변경

- STREAM socket은 connect·disconnect를 제공하지 않으며 `disconnectRid`를 제공합니다. 존재하지 않는 RID는 connect 오류 NotFound (605)입니다.
