[English](./bindings-node-1.5.0.md) | [한국어](./bindings-node-1.5.0.ko.md)

# ZLink Node.js binding 1.5.0 릴리스 노트

이 릴리스는 npm 패키지 버전을 Core 1.5.0에 맞춥니다.

## 변경

- npm 패키지 버전을 Core 1.5.0 릴리스에 맞춰 1.5.0으로 올립니다.
- Core 1.5.0은 static TLS 여유 공간이 적은 host에서 늦은 dlopen()으로 libzlink를 불러오지 못하던 문제를 수정합니다. Beast WebSocket의 secure PRNG는 thread별 상태를 112바이트 `thread_local` 객체에 두는 대신, 8바이트 소유 포인터가 가리키는 heap 객체에 저장합니다. 이에 따라 PT_TLS 사용량은 312바이트에서 200바이트로 줄었습니다. 공개 C API와 ABI는 변경되지 않았습니다.
- Node.js binding의 동작은 변경되지 않았습니다. 이 릴리스는 binding 버전을 Core 1.5.0에 맞춥니다.

## 검증

- 릴리스 사전 검사는 Node.js binding 버전, 릴리스 노트, npm 패키지 메타데이터를 확인합니다.

릴리스 태그는 `node/v1.5.0`입니다.

