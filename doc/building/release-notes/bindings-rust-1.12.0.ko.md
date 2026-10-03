[English](./bindings-rust-1.12.0.md) | [한국어](./bindings-rust-1.12.0.ko.md)

# ZLink Rust binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다. Binding 버전을 0.18.0에서 Core 버전에 맞춘 1.12.0으로 올립니다.

## 변경

- ROUTER가 선택한 route를 수신 결과에서 관찰할 수 있습니다 (#1087).
- STREAM socket은 RID 단위 disconnect를 제공하며, 일반 connect·disconnect는 제공하지 않습니다. 존재하지 않는 RID는 NotFound (605)입니다 (#1164).
- SEND의 WRITABLE completion이 Core 결과를 그대로 보고합니다. timeout으로 끝난 대기는 BACKPRESSURED와 `EAGAIN`으로 보고합니다 (#1154).

## 동작 변경

- socket close의 결과를 Core에서 받은 그대로 보고합니다. REQUEST 제출은 socket lock 밖에서 실행합니다 (#1164).
- 공개 completion owner가 없는 SEND 대기는 `InvalidState`로 거부합니다 (#1197).
- 긴 구독 topic을 수신할 때 버퍼를 Core가 보고한 길이까지 늘립니다 (#1218).
- REQUEST 결과의 대표 errno를 Core 표에 맞춥니다. `CONFLICT`는 `EEXIST`이며, `INTERNAL_ERROR`는 기존 0 대신 `EIO`입니다 (#1218, #1248).
