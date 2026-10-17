[English](./bindings-dotnet-1.19.3.md) | [한국어](./bindings-dotnet-1.19.3.ko.md)

# ZLink .NET binding 1.19.3 릴리스 노트

Core 1.19.0을 따릅니다.

## 변경

- 1.19.2까지는 send·request 제출이 즉시 실패하면 caller는 예외를 받았지만, 반환되지 않은 내부 완료 `Task`도 같은 예외로 fault된 채 남았습니다. 그래서 GC 뒤 `TaskScheduler.UnobservedTaskException`이 발생했습니다. 대상 peer가 없는 Router에 보낼 때가 그 예입니다. 이제 반환되지 않은 내부 `Task`의 예외를 binding이 관측하므로, caller가 받는 예외는 그대로이고 미관측 예외는 생기지 않습니다. 공개 API 변경은 없습니다(#2073).
