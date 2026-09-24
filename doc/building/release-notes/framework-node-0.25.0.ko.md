[English](./framework-node-0.25.0.md) | [한국어](./framework-node-0.25.0.ko.md)

# ZLink Node.js Framework 0.25.0 릴리스 노트

Framework 0.25.0은 Node.js binding 1.7.0과 Core 1.7.0을 사용합니다.

## 수정

- Core 1.7.0은 libstdc++을 정적으로 링크한 host가 `dlopen()`으로 `libzlink`를 읽을 때 발생하던 static TLS 부족 문제를 해결합니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- GameQuest sample client가 one-way close 뒤 이전 owner로 간 첫 요청의 stale 오류를 예상 결과로 처리합니다. Windows에서 이 sample이 실패하던 문제를 해결합니다 ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## 설치

```bash
npm install @zlink-systems/framework@0.25.0 @zlink-systems/http-client@0.25.0
```

릴리스 태그는 [`framework-node/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.25.0)입니다.
