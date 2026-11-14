[English](./framework-node-0.32.2.md) | [한국어](./framework-node-0.32.2.ko.md)

# ZLink Node.js Framework 0.32.2 릴리스 노트

Framework 0.32.2는 0.32.1의 결함을 고치는 hotfix입니다. Node.js binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- Actor가 `joinEntrySpot().defer()` 등으로 User Spot에서 Entry Spot으로 옮겨 가는 순간에 같은 Actor로 보낸 request가 `SPOT actor is not registered locally`(ActorLocationStale)로 끝날 수 있었습니다. 이동 전에 이전 Spot의 Actor 대기열에 들어간 message를, 이동이 끝난 뒤 이전 Spot을 떠났다는 표시만 보고 Actor가 없다고 판정했기 때문입니다. 이제 그 message는 원래 correlation·deadline·reply 경로를 유지한 채 Actor의 현재 Spot 대기열로 넘어가 한 번 실행됩니다. 반대 방향(Entry → User Spot)은 원래 성공했고 동작이 같습니다. (#4437, #4455)

## 0.30.x에서 올릴 때

- `ZLinkMessageContext.receivedAt`은 0.32.0부터 필수입니다. 시험용 fake context 등 `ZLinkMessageContext`를 직접 구현한 코드는 `receivedAt`(Framework가 message를 받은 UTC 시각)을 채워야 합니다. (#3602)

## 설치

```bash
npm install @zlink-systems/framework@0.32.2
```

릴리스 태그는 [`framework-node/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.2)입니다.
