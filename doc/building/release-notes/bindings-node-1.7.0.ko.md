[English](./bindings-node-1.7.0.md) | [한국어](./bindings-node-1.7.0.ko.md)

# ZLink Node.js binding 1.7.0 릴리스 노트

이 릴리스는 npm 패키지 버전을 Core 1.7.0에 맞춥니다.

## 변경

- npm 패키지 버전을 1.7.0으로 올리고 Core 1.7.0을 사용합니다.
- Core 1.7.0은 libstdc++을 정적으로 링크한 host가 `dlopen()`으로 `libzlink`를 읽을 때 발생하던 static TLS 부족 문제를 해결합니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- Node.js binding의 동작은 변경되지 않았습니다.

## 검증

- 릴리스 사전 검사는 Node.js binding 버전, 릴리스 노트, npm 패키지 메타데이터를 확인합니다.

릴리스 태그는 `node/v1.7.0`입니다.
