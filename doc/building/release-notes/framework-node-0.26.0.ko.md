[English](./framework-node-0.26.0.md) | [한국어](./framework-node-0.26.0.ko.md)

# ZLink Node.js Framework 0.26.0 릴리스 노트

Framework 0.26.0은 Node.js binding 1.10.0과 Core 1.10.0을 사용합니다.

## 변경

- Stream Connector는 frame decode 실패와 수신 payload 한도 초과 시 연결을 `ProtocolError` 사유로 종료합니다. §6.3에 따라 연결 전 모든 option을 검증하며, 값이 허용 범위를 벗어나면 `ValidationFailed`, 항목 사이가 맞지 않으면 `ConfigurationError`를 반환합니다. 종료 뒤 미전송 frame은 버리고 진행 중인 요청은 실패로 완료합니다.
- Spot context `Close`는 결과를 반환합니다. Close 완료는 authority를 해제한 뒤에 통지합니다. 이 결과 반환 방식은 0.25.0 이후 main에 확정된 계약을 유지합니다.
- Relocation `Restore`는 source만 판정합니다. Wire schema에서 `remainingDeadlineMs`를 제거했으며, 1.0 이전 wire 형식과의 호환성은 제공하지 않습니다. Store retention을 밀리초로 바꿀 때는 올림을 적용합니다.
- listener 상태 조회는 실제 bind가 확정한 endpoint를 반환합니다. MeshNode의 activation 기록으로 placement 수를 판정하고, Session의 `actor_slot`은 turn 시작 시 해석합니다.

## 설치

```bash
npm install @zlink-systems/framework@0.26.0 @zlink-systems/http-client@0.26.0
```

릴리스 태그는 [`framework-node/v0.26.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.26.0)입니다.
