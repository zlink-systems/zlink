[English](./bindings-python-1.13.0.md) | [한국어](./bindings-python-1.13.0.ko.md)

# ZLink Python binding 1.13.0 릴리스 노트

Core 1.13.0을 사용합니다.

## 변경

- Core 1.13.0의 수신 진입 판정, 긴 구독 trie와 물리 연결 종료 monitor 수정이 포함됩니다 (#1292, #1334).
- REQUEST 결과의 대표 errno를 Core 표에 맞춥니다 (#1218).
- sdist에서 만든 wheel의 Core loader link 검사를 수정했습니다 (#1277).

## 유지하는 계약

다음 항목은 1.12.0에 포함된 동작을 유지하며, 1.13.0에서 새로 추가한 변경이 아닙니다.

- STREAM은 bind 전용이며 RID 단위 disconnect를 제공합니다 (#1192, #1194).
- WRITABLE completion은 Core 결과를 보고하며, timeout으로 끝난 대기는 `BACKPRESSURED`와 `EAGAIN`입니다 (#1168).
