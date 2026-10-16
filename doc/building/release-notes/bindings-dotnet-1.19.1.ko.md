[English](./bindings-dotnet-1.19.1.md) | [한국어](./bindings-dotnet-1.19.1.ko.md)

# ZLink .NET binding 1.19.1 릴리스 노트

Core 1.19.0을 따릅니다.

## 변경

- Core는 payload가 29바이트 이하이면 이를 `zlink_msg_t` 안에 둡니다. .NET binding의 `Message`가 이 구조체를 GC가 이동할 수 있는 managed 필드에 저장하면 compacting GC 뒤 payload view가 잘못된 주소를 가리킬 수 있습니다. 이로 인해 payload 불일치나 libcoreclr SIGSEGV가 발생했습니다. `Message`는 이제 `zlink_msg_t`를 native 메모리에 저장하고 Dispose 또는 finalizer에서 한 번 해제합니다. 공개 API 변경은 없습니다. pool을 사용하지 않는 `Message`의 생성·해제 비용은 약 80 ns 늘었습니다(#1835).
