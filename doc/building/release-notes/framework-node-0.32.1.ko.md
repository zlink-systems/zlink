[English](./framework-node-0.32.1.md) | [한국어](./framework-node-0.32.1.ko.md)

# ZLink Node.js Framework 0.32.1 릴리스 노트

Framework 0.32.1은 0.32.0의 결함을 고치는 hotfix입니다. Node.js binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- Location Store가 Actor authority 삭제에 한 번 실패하면, 노드에는 없는 Actor를 Store가 계속 가리키고 `getOrCreate`는 그 incarnation을 돌려주며 같은 ref의 destroy는 generation 불일치로 끝나 빠져나갈 수 없었습니다. 이제 destroy는 정리가 끝날 때까지 정확한 incarnation을 유지하고, Store 삭제 실패를 destroy 실패로 돌려줍니다. 같은 ref로 다시 destroy하면 Store 정리를 다시 시도합니다. (#4397, #4403)

## 0.30.x에서 올릴 때

- `ZLinkMessageContext.receivedAt`은 0.32.0부터 필수입니다. 시험용 fake context 등 `ZLinkMessageContext`를 직접 구현한 코드는 `receivedAt`(Framework가 message를 받은 UTC 시각)을 채워야 합니다. (#3602)

## 설치

```bash
npm install @zlink-systems/framework@0.32.1
```

릴리스 태그는 [`framework-node/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.1)입니다.
