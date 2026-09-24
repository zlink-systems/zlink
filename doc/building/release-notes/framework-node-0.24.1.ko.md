[English](./framework-node-0.24.1.md) | [한국어](./framework-node-0.24.1.ko.md)

# ZLink Node.js Framework 0.24.1 릴리스 노트

Framework 0.24.1은 Node.js binding 1.7.0과 Core 1.7.0을 사용합니다.

## 수정

- Core 1.7.0은 libstdc++을 정적으로 링크한 host가 `dlopen()`으로 `libzlink`를 읽을 때 발생하던 static TLS 부족 문제를 해결합니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- Node.js Framework의 계약과 동작은 변경되지 않았습니다.

## 설치

```bash
npm install @zlink-systems/framework@0.24.1 @zlink-systems/http-client@0.24.1
```

릴리스 태그는 [`framework-node/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.24.1)입니다.
