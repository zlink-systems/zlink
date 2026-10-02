[English](./bindings-node-1.13.1.md) | [한국어](./bindings-node-1.13.1.ko.md)

# ZLink Node.js binding 1.13.1 릴리스 노트

Core 1.13.0을 사용합니다.

## 변경

- blocking request가 socket의 RCVTIMEO를 넘어서도 Core의 request terminal을 기다립니다. 이전에는 RCVTIMEO에서 대기를 먼저 끝내 Core 결과와 다른 결과를 보고할 수 있었습니다 (#1164).
