[English](./bindings-node-1.2.0.md) | [한국어](./bindings-node-1.2.0.ko.md)

# ZLink Node.js binding 1.2.0 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. 1.1.0 binding은 Core 1.1.0 ABI를 전제하므로 Core 1.2.0과 함께 쓸 수 없고, 이 릴리스가 그 간극을 닫습니다.

## 변경

- Core 1.2.0 native와 결합합니다. 패키지에 담긴 native와 provenance는 Core 1.2.0 릴리스 자산입니다.
- Windows에서 node-gyp configure가 성공하도록 gyp-safe Core 경로를 냅니다.
## 검증

- binding 테스트 스위트가 Core 1.2.0 패키지 위에서 통과했습니다.

릴리스 태그는 `node/v1.2.0`입니다.
