[English](./framework-node-0.27.0.md) | [한국어](./framework-node-0.27.0.ko.md)

# ZLink Node.js Framework 0.27.0 릴리스 노트

Framework 0.27.0은 Node.js binding 1.14.0과 Core 1.14.0을 사용합니다. (#1447)

## 계약 변경

- capacity count와 `activationConcurrency.active`가 바뀐 것만으로 descriptor를 게시하거나 Revision을 올리지 않습니다. 수용 공간은 Location Store의 atomic reservation이 확보하고 activation 수락은 target MeshNode가 판정합니다. (#1436)
- Classic fanout publisher는 host startup 중 endpoint를 bind합니다. Bind 실패는 startup 실패이며, subscriber는 첫 publish 전에도 연결할 수 있습니다. (#1440)
- Spot `Close`를 lifecycle 실행 순서의 작업으로 처리합니다. Ready route는 Instance intent를 전달하며 정상 Close 뒤 요청은 다시 활성화할 수 있습니다. Drain·relocation 중 Close는 다시 배치하지 않고 typed 실패로 끝내며, Reincarnate하지 않는 Close는 authority를 해제합니다. (#1314, #1321, #1372)
- Relocation seal은 Session→Actor one-way relay를 보관하며 보관소의 수락을 admission으로 판정합니다. Join 승인 응답은 commit을 뜻하지 않습니다. 검증된 cutover 전의 target staging도 host 종료 seal로 끝납니다. (#1196, #1204, #1416)
- 비활성 target으로 향하는 Actor Join은 `Unavailable`, Closing 대상 Join은 `Rejected`로 끝납니다. Closing·Draining owner의 신규 admission 결과를 공통 오류 계약에 맞추고, 같은 node Actor Join은 단일 local 경로로 처리합니다. (#1147, #1150, #1191, #1230)
- Channel 선택 결과와 ClientServer 준비 대기는 admission deadline을 따릅니다. 대기 토큰 없는 capacity 거절은 즉시 `Unavailable`로 끝납니다. 내부 작업이 permit을 기다릴 때 자원을 점유하지 않도록 하되 application handler의 public 비동기 API 대기는 허용합니다. (#1229, #1333, #1412)
- ClientServer application metadata를 JSON header의 `metadata` 객체로 전달하며 크기는 최소 escape 표현으로 계산합니다. (#1255, #1294)
- 상태 관찰은 구독 시점 status를 첫 항목으로 제공합니다. Topology·host status의 Sequence는 공개 field가 바뀐 게시에서만 증가합니다. RouteMesh 전체 state는 peer·Location Store 상태로 degraded를 판정합니다. (#1298, #1330, #1232)
- Owner lease 자격 검사는 Value 조건을 사용합니다. Location Store Conflict 뒤에는 작업 자격을 다시 확인하고 요청을 재구성합니다. Store 복구 뒤 intent 제거는 owner lease TTL 뒤 다시 읽은 목록을 사용하며, service summary는 owner lease를 검증해 수를 계산하고 lease 조회 실패·손상을 query 오류로 반환합니다. Binding이 connect를 거절해도 connection intent는 유지합니다. (#1148, #1320, #1251, #1361, #1358)
- Source 정리 실패가 뒤 정리 단계를 막지 않으며, Relocation Store 재확인은 원래 operation의 deadline을 따릅니다. Location 조회의 기본 page 크기는 100입니다. (#1313, #1302)
- STREAM은 bind 전용이며 binding에 `disconnectRid`가 필요합니다. (#1191) STREAM session heartbeat는 Stream Connector의 timeout 기산점을 따릅니다. Manual dispatch queue는 용량 때문에 callback 등록을 미루지 않습니다. Packet name은 비어 있거나 공백 문자만일 수 없으며, timeout·취소된 request의 미전송 frame은 보내지 않습니다. Transport close 실패는 원래 종료 사유를 유지하면서 `Disconnected` 오류 event로 전달합니다. (#1343, #1359, #1302)
- Application이 완료시키는 비동기 결과의 제한 시간 관찰과 host 종료 시 runtime 대기 종료를 정했습니다. HTTP client에서 요청과 close가 모두 실패하면 요청 실패를 반환하고, 요청 성공 뒤 close만 실패하면 close 실패를 반환합니다. (#1256, #1265, #1302)
- Wire failure code와 `ErrorKind`, code만 담는 실패의 `ShuttingDown`·원인 없는 `InvalidOperation` 표현을 공통 오류 계약에 맞췄습니다. Binding의 one-way `NOT_ADMITTED` 결과는 `Rejected`입니다. (#1367, #1369, #1302)

## 결함 수정과 언어별 변경

- Count만 바뀌었을 때 descriptor를 게시하지 않도록 고쳤습니다. Actor placement가 descriptor 전체를 읽고, 생성 자격 확인 뒤 공유 capacity CAS를 계산하며, Redis scan·history 판정을 바로잡았습니다. (#1441, #1437, #1443)
- 명시적인 User Spot Close가 location을 해제하고, 시작된 yield turn 뒤 `OnClosing`을 실행하며, Closing commit 뒤 lifecycle 작업이 들어오지 않도록 고쳤습니다. Close 뒤 오래된 Ready route의 Instance intent도 cold activation으로 처리합니다. (#1392)
- Relocation prepare reply 소유권과 target authority 반영, cutover 전 target 종료 seal을 고쳤습니다. (#1346, #1392, #1416)
- RouteMesh request backpressure와 자기 node를 제외하는 select-one 선택을 고쳤습니다. Local request 종료 시 capacity 대기를 정리합니다. (#1408, #1412)
- 수신 codec을 exact registry lookup으로 선택하고 STREAM 수신 검증·metadata 크기 판정, NestJS provider·handler 선택, GameQuest provider 등록과 Unity bundle 검사를 고쳤습니다. (#1252, #1281, #1336, #1392)

## 호환성에 영향을 주는 변경

- Ready route의 Instance intent와 wire 오류 분류가 달라졌습니다. 0.26.0과의 wire 호환성을 보장하지 않습니다. (#1372, #1367, #1369)
- 상태 관찰의 첫 항목·Sequence, Close 뒤 재활성화와 오류 결과, publisher bind 실패 시점이 달라질 수 있습니다. (#1298, #1330, #1372, #1440)

## 설치

```bash
npm install @zlink-systems/framework@0.27.0 @zlink-systems/http-client@0.27.0
```

릴리스 태그는 [`framework-node/v0.27.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.27.0)입니다.
