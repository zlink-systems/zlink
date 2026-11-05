[English](./framework-node-0.32.0.md) | [한국어](./framework-node-0.32.0.ko.md)

# ZLink Node.js Framework 0.32.0 릴리스 노트

Framework 0.32.0은 Node.js binding 1.23.0과 Core 1.23.0을 사용합니다. (#3843)

## 동작 변경

- 알 수 없는 control packet이나 heartbeat payload version을 받아도 연결을 닫지 않습니다. (#3773, #3790)
- Actor owner가 relay된 session sequence의 중복·역순을 거부합니다. (#1721, #3763)
- `MessageContext.receivedAt`은 Framework가 message를 받은 UTC 시각입니다. 기본값이 없는 DTO property는 framework-json-v1에서 필수이며, 중복 envelope header field는 거부됩니다. (#2029, #3602, #1705, #3592, #3376, #3396)
- 중복 bind는 owner Actor의 `onDuplicateBind` callback이 판정합니다. (#2205, #3640)
- 공개 `configureRouterSocket()`의 HWM 설정을 RouteMesh ROUTER socket에 전달합니다. (#2567, #3570)

## 결함 수정

- 늦게 도착한 physical 결과는 fence가 일치할 때만 route cache 항목을 무효화하며, cache 무효화는 08-routing §2의 trigger 표를 따릅니다. 강제 종료 뒤 Store 정리는 deadline 하나를 쓰고 authority를 기준으로 sweep하며, 대상 lifecycle이 끝나면 reservation을 해제합니다. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- relocation terminal 결과를 message flow에 기록하고 cutover flow와 Bingo join·observer 정책을 보존합니다. (#2465, #3621)
- callback 실패에서 원래 exception을 보존합니다. correlation을 읽을 수 있는 잘못된 request에는 terminal protocol error를 응답합니다. (#2098, #3429, #2512, #3418)
- Instance-intent request는 admission 전에 stale route를 거부하고 request deadline 안에서 현재 authority를 따릅니다. disconnect가 실패해도 liveness 정리가 peer 정리와 통지를 마칩니다. (#3353, #3442, #2370, #3434)
- Auto-connect는 stop/start 사이에 lifecycle 하나를 유지하고 descriptor 게시를 기다립니다. 자기 descriptor 게시가 실패하면 peer를 다시 조정하지 않습니다. (#3569, #3679, #3723, #3730, #3664)
- Auto-connect는 같은 endpoint의 descriptor만 자기 자신으로 제외합니다. binding 완료 뒤 도착한 terminal reply는 Actor slot을 해제하고 Session reply로 보냅니다. (#2155, #3433, #2757, #3419)
- 0.31.0의 `mmo-world-channels` template update에서 record lease를 다시 얻을 때 첫 ChannelView가 누락되던 문제를 수정했습니다. (#3344, #3363)
- peer를 발견한 뒤 보류된 HELLO 처리를 재개합니다. (#3326, #3370)

## 성능 개선

- payload 크기의 Framework buffer를 내부 slab에서 할당하고 shared receive queue가 permit context를 재사용합니다. (#1675, #3422, #3655)

## 시험과 CI

- M6B stale-route contract 시험이 각 시나리오에 필요한 상태를 직접 설정합니다. (#3778, #3789)

## 알려진 제한

- Node `mmo-seamless` template border E2E는 간헐적으로 `PlayerAppeared`를 기다리다 시간 초과합니다. (#3680)

## 설치

```bash
npm install @zlink-systems/framework@0.32.0 @zlink-systems/http-client@0.32.0
```

릴리스 태그는 [`framework-node/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.0)입니다.
