[English](./bindings-dotnet-1.16.0.md) | [한국어](./bindings-dotnet-1.16.0.ko.md)

# ZLink .NET binding 1.16.0 릴리스 노트

Core 1.16.0을 따릅니다.

## 변경

- Core 1.16.0은 `ZLINK_OPT_BINDTODEVICE`를 `tcp`, `tls`, `ws`, `wss` socket의 bind·connect 전에 적용합니다. 지원하지 않는 플랫폼에서 비어 있지 않은 값을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환합니다 (#1469).
- Core 1.16.0은 socket mailbox signaler를 만들지 못하면(프로세스 fd 한도 도달 포함) `zlink_socket()`에서 `NULL`과 시스템 errno를 반환합니다 (#1481).
