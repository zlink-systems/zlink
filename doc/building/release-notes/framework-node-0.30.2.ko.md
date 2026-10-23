[English](./framework-node-0.30.2.md) | [한국어](./framework-node-0.30.2.ko.md)

# ZLink Node.js Framework 0.30.2 릴리스 노트

Framework 0.30.2은 Node.js binding 1.20.0과 Core 1.20.0을 사용합니다.

## 호환성 변경

- `addEntrySpot`은 Entry Spot type과 함께 그 Entry Spot이 받는 Actor stable type을 받습니다. TypeScript의 Actor type 인자는 실행 중에 남지 않으므로, 이 값이 없으면 새 Actor를 그 type을 받지 않는 node에 만들 수 있었습니다. `addActorFactory`에 쓰는 이름을 그대로 넘깁니다. (#1772)

  ```ts
  // 0.30.1
  objects.addEntrySpot(LobbySpot);
  // 0.30.2
  objects.addEntrySpot(LobbySpot, 'player');
  ```

## 변경

- request source owner를 ingress에서 기록하고 relocation 뒤에도 원래 requester를 보존합니다. source descriptor가 유효한 Actor·Spot 요청만 처리합니다. (#2103, #2140, #2151)
- per-message Store 조회와 completion 처리를 줄였고 runtime metric label·refresh 시점을 계약에 맞췄습니다. (#1809, #1834, #1866, #2189, #2190)
- deferred Join과 HTTP handler Yield가 실행 turn을 올바르게 넘기며, 여러 번 연속으로 defer하는 경로를 지원합니다. (#2003, #2064)
- Actor relocation 시 pending timer와 frame을 새 owner로 전달하고, relocation 결과 metric을 갱신합니다. (#1987, #1960, #2013)

## 결함 수정

- Spot close·Session seal이 실행 중인 handler나 열린 binding 종료를 기다리지 못해 정체되던 문제를 수정했습니다. (#1896, #1915, #2135, #1886)
- deferred Join·source settlement와 relocation 완료가 서로 다른 수명·authority를 사용하던 문제를 수정했습니다. (#1903, #1938, #2050, #2003)
- source descriptor 없는 request를 거부하고, relocation 중 원래 requester를 유지해 결과가 잘못 라우팅되지 않게 했습니다. (#2103, #2140, #2151)
- runtime metric에서 Location capacity가 descriptor polling과 함께 갱신되고 outcome label은 정해진 값만 사용합니다. (#2189, #2190)
- Bingo 관찰자가 Join 뒤 방의 현재 상태와 확정 reward를 조회하도록 sample을 수정했습니다. (#2427)
- target Join lifecycle 실패의 원래 예외를 error reporter에 기록합니다. (#2459)
- Core 1.20.0은 handshake 중 completion poller를 등록할 때 임시 transport owner의 executor가 멈추던 문제를 수정했습니다. (#2504)
- Actor reply를 STREAM frame 한 개로 보내고 오류 payload를 스펙의 `{code, message}` 형식으로 맞췄습니다. 네 언어가 같은 공유 vector로 검증합니다. (#2507)
- Actor Response를 요청 packet의 reply 계약으로 검사합니다. 이전에는 payload 계약으로 검사해 Entry Spot Actor 요청의 typed 응답이 거부될 수 있었습니다. (#2507)

## 설치

```bash
npm install @zlink-systems/framework@0.30.2 @zlink-systems/http-client@0.30.2
```

릴리스 태그는 [`framework-node/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.30.2)입니다.
[한국어](./framework-node-0.30.2.ko.md) | [English](./framework-node-0.30.2.md)
