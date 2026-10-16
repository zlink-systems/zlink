[English](./bindings-dotnet-1.19.2.md) | [한국어](./bindings-dotnet-1.19.2.ko.md)

# ZLink .NET binding 1.19.2 릴리스 노트

Core 1.19.0을 따릅니다.

## 변경

- 1.19.1에서 `Message.Allocate`는 사용자 소유 `Message`를 수신 wrapper pool에서 빌렸습니다. 반환된 wrapper를 다시 Dispose하면 native storage가 해제되어 이후 `Message` 사용에서 `ObjectDisposedException`이 발생했습니다. `Allocate`는 pool을 사용하지 않으며 같은 `Message`를 여러 번 Dispose해도 안전합니다. 공개 API 변경은 없습니다. `Allocate`의 연산당 할당량은 managed 32 B, native 64 B 증가했습니다(#1858).
