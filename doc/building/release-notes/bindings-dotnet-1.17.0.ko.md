[English](./bindings-dotnet-1.17.0.md) | [한국어](./bindings-dotnet-1.17.0.ko.md)

# ZLink .NET binding 1.17.0 릴리스 노트

Core 1.17.0을 따릅니다.

## 변경

- Core 1.17.0은 `ws`·`wss`에서 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. 이전에는 Core가 STREAM frame 여러 개를 WebSocket 메시지 하나에 넣어 보냈고, 수신 측은 `frame length does not match prefix` 오류로 연결을 종료했습니다 (#1545, #1553).
