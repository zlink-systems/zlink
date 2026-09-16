[English](./framework-node-0.15.0.md) | [한국어](./framework-node-0.15.0.ko.md)

# ZLink Node.js Framework 0.15.0 릴리스 노트

Framework 0.15.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 주요 변경

- Classic fanout publisher에 `setNoDrop` 설정을 더했습니다. 켜면 topic이 일치하는 pipe 전체에 대해 전부 보내거나 하나도 보내지 않으며, 보낼 수 없으면 `DeadlineExceeded`로 끝납니다.
- Fanout subscriber가 받을 topic을 startup에 `subscribe`로 등록합니다. 등록한 topic의 byte prefix 합집합을 받으며, 등록이 없으면 빈 topic만 받습니다.
- Fanout publish가 local admission을 기다린 뒤 결과를 돌려줍니다. 기다린 시간이 send timeout을 넘으면 `DeadlineExceeded`입니다.
- Descriptor를 처음 받아들이는 자리에서 owner의 생존을 판단합니다.
- Wildcard bind host에서 `setAdvertiseHost`를 생략하면 같은 address family의 loopback을 광고합니다. `0.0.0.0`은 `127.0.0.1`, `::`은 `::1`입니다. 이전에는 startup이 거부됐습니다.
- 두 dispatch mode 모두에서 수신하므로 Manual 대기에 별도의 pump가 필요하지 않습니다.

## 설치

```bash
npm install @zlink-systems/framework@0.15.0
```

릴리스 태그는 [`framework-node/v0.15.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.15.0)입니다.
