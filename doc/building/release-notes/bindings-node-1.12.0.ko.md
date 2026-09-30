[English](./bindings-node-1.12.0.md) | [한국어](./bindings-node-1.12.0.ko.md)

# ZLink Node.js binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다.

## 변경

- STREAM socket은 connect·disconnect를 제공하지 않으며 `disconnectRid`를 제공합니다. 존재하지 않는 RID는 connect 오류 NotFound (605)입니다.
- 1.11.1의 `disconnectRid` 결과 분류 수정을 포함합니다 (#1188).
