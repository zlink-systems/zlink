[English](./bindings-node-1.12.0.md) | [한국어](./bindings-node-1.12.0.ko.md)

# ZLink Node.js binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다.

## 변경

- STREAM socket은 connect·disconnect를 제공하지 않으며 `disconnectRid`를 제공합니다. 존재하지 않는 RID는 connect 오류 NotFound (605)입니다.
- 1.11.1의 `disconnectRid` 결과 분류 수정을 포함합니다 (#1188).

## 동작 변경

- 구독 topic 수신 버퍼가 Core가 보고한 길이만큼 확장됩니다. 긴 topic도 잘리거나 수신 실패하지 않습니다 (#1218).
- REQUEST 결과별 대표 errno를 Core 표에 맞춥니다. `CONFLICT`는 `EEXIST`, `INTERNAL_ERROR`는 `EIO`를 반환합니다 (#1248).
