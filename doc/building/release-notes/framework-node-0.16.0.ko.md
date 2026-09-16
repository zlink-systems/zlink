[English](./framework-node-0.16.0.md) | [한국어](./framework-node-0.16.0.ko.md)

# ZLink Node.js Framework 0.16.0 릴리스 노트

Framework 0.16.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Select-one channel에서 eligibility와 drain 조건을 적용한 뒤 남은 member가 하나도 없으면 `Unavailable`로 끝납니다. Request와 one-way send가 같은 kind입니다. Weight가 `0`이거나 draining이어서 후보에서 빠진 경우가 여기에 해당하며, 송신 경로와 connection은 그대로 있으므로 `NotFound`가 아닙니다. 이전에는 언어마다 답이 달랐습니다.
- Channel request가 이 경우에 `protocol_error`로 끝나던 문제를 고쳤습니다. 없는 node를 지목한 호출은 그대로 `not_found`입니다.

## 설치

```bash
npm install @zlink-systems/framework@0.16.0
```

릴리스 태그는 [`framework-node/v0.16.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.16.0)입니다.
