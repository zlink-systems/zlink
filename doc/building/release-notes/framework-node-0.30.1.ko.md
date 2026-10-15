[English](./framework-node-0.30.1.md) | [한국어](./framework-node-0.30.1.ko.md)

# ZLink Node.js Framework 0.30.1 릴리스 노트

Framework 0.30.1은 Node.js binding 1.19.0과 Core 1.19.0을 사용합니다.

## 변경

- 서버 Framework의 라이선스가 zlink License 1.0으로 바뀌며, 1.0.0 전 버전은 키나 유료 라이선스 없이 머신 수와 관계없이 무료입니다. Client connector·공용 client 코드·sample·tutorial·quickstart와 HTTP client는 Apache-2.0이며, 0.30.0 이하 릴리스의 라이선스는 바뀌지 않습니다. (#1681)
- 공개 API 추가: request 결과 `ZLinkRequest<TReply>.admitted`로 reply와 별도로 source-local 수락을 기다릴 수 있습니다. Infrastructure 조회는 이 단계를 완료하지 않으며, 수락 전 실패는 admission과 reply에 같은 원인으로 전달됩니다. (#1682)
- Nest handler 준비와 provider 조회를 등록 시점으로 모으고, 정상 경로의 예외 생성·중간 Buffer·Promise·반복 pressure 판정을 줄였습니다. 메시지 송수신과 completion은 payload 수명을 유지하면서 중간 객체를 줄여 처리합니다. (#1652, #1657, #1693)

## 결함 수정

- 잘못된 envelope를 받은 request가 언어에 따라 오류 응답 없이 끝나거나 다르게 처리되던 결함을 수정했습니다. Header가 문법에 맞는 JSON object이고 유효한 correlation ID와 reply token이 있으면 오류 응답을 보내며, reply 경로를 확인할 수 없으면 응답 없이 관측 정보에 기록합니다. (#1671)
- Spot Configure의 잘못된 handler·subscription 등록을 activation의 Ready 전에 `InvalidOperation`으로 거부하도록 네 언어의 오류 분류를 맞췄습니다. (#1715)
- Relocation이 전달하는 새 owner generation을 Store CAS 결과와 일치시키고 sealed Session binding을 함께 전달합니다. 이동 후 Session route를 확정할 때 서로 다른 generation이나 누락된 binding 상태를 사용하던 경로를 수정했습니다. (#1807)
- Stream Connector 기본 JSON codec을 서버와 같은 `framework-json-v1`로 맞추고 Unity WebGL 배포 bundle에도 반영했습니다. 64-bit 정수 문자열과 typed field 검증이 공통으로 적용되며, Unity package의 `LICENSE.md`에 맞지 않던 meta 파일도 수정했습니다. (#1688, #1703, #1868)
- 같은 node의 Actor가 Spot Join에 성공해도 Store membership이 이전 Entry에 남아 maintenance relocation이 거부되던 결함을 수정했습니다. Authority fence와 Store CAS commit 뒤에만 Join을 수락하고 native membership을 적용합니다. (#1739)
- 빈 reply를 가진 deferred Actor Join recovery가 metadata 누락으로 거부되던 결함을 수정했습니다. 빈 reply는 metadata 없이 보존하고 복구합니다. (#1746)
- 초기 owner lease claim이 오래 걸리면 첫 heartbeat가 늦어지거나, 부동소수점 deadline 계산 때문에 timeout이 제때 끝나지 않던 결함을 수정했습니다. 갱신과 남은 timeout은 같은 operation 시작 시각과 설정 budget을 기준으로 계산합니다. (#1830, #1869)
- Relocation source의 `safeToShutdown` 관찰 누락과 Actor member가 있는 host 종료·maintenance Entry 복원 실패를 수정하여, 조회·관찰은 같은 status를 사용하고 종료는 local Actor를 정리하며 maintenance는 target startup Entry에 복원합니다. `findSpot`은 현재 Spot generation을 반환하고 Session route 확정은 실제 Session owner로 보냅니다. (#1831)
- STREAM receive 대기에서 미완료 Promise가 회전마다 쌓여 메모리가 증가하고 `Map maximum size exceeded`까지 발생하던 결함을 수정했습니다. Poller의 취소 가능한 대기 하나가 wake와 shutdown을 함께 처리합니다. (#1839)
- Route 교체 중 cross-node Join과 relocation이 멈추던 결함을 수정하여, 다른 선택 route를 관찰하면 ready 상태에서 durable request와 batch·cutover를 다시 제출합니다. ADMIT까지 같은 HELLO snapshot을 재전송·응답에 사용하고 이후 변경은 UPDATE로 보내며, 같은 상대의 기존 route·manual intent가 있으면 자동 중복 연결을 만들지 않고 물리 연결이 끊겨도 manual intent를 유지합니다. (#1850, #1864)
- 중복 Spot packet·subscription 등록을 허용하거나 무시하던 결함을 수정했습니다. Activation이 `InvalidOperation`으로 거부합니다. (#1715)

## 설치

```bash
npm install @zlink-systems/framework@0.30.1 @zlink-systems/http-client@0.30.1
```

릴리스 태그는 [`framework-node/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.30.1)입니다.
[한국어](./framework-node-0.30.1.ko.md) | [English](./framework-node-0.30.1.md)
