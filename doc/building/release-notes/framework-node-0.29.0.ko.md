[English](./framework-node-0.29.0.md) | [한국어](./framework-node-0.29.0.ko.md)

# ZLink Node.js Framework 0.29.0 릴리스 노트

Framework 0.29.0은 Node.js binding 1.17.0과 Core 1.17.0을 사용합니다. (#1554)

## 계약 변경

- Missing Instance Spot의 cold activation은 Core request/reply로 target에 제출합니다. target이 예약과 활성화를 소유합니다. Reserve에서 경쟁에 진 요청은 다른 target으로 전달하거나 winner의 완료를 기다리지 않고 `Unavailable` 한 번으로 끝납니다. type을 정할 serving node가 없으면 `NotFound`, 생략한 type 후보가 여러 개면 `InvalidOperation`, capacity가 부족하면 `Unavailable`을 반환합니다. (#1467)
- Spot·Actor·handler 등록의 생성자 의존성을 optional·collection·이름/qualifier 선택 규칙에 따라 host 시작 때 선언 metadata로 검증합니다. 필수 의존성이 없거나 선택이 모호하면 시작이 실패합니다. Node는 provider metadata를 사용하며 명시적 factory는 실행하지 않습니다. (#1549)
- target을 다시 시작할 때 이전 lifecycle에 속한 Creating 예약을 recovery 단계에서 해제하고 pending capacity를 반환합니다. (#1532)

## 변경

- Stream node에 heartbeat 간격·제한 시간과 idle 종료 시간을 설정하는 `setHeartbeat`와 `setIdleTimeout`을 추가했습니다. heartbeat 기본값은 1초·5초입니다. idle timeout 기본값은 0이며, 이 값에서는 유휴 종료가 비활성화됩니다. heartbeat control frame은 애플리케이션 활동으로 계산하지 않습니다. (#1538, #1541)
- Stream call에 `submit(ReplyType, signal?)`을 추가했습니다. callback 방식은 `submit(callback)`에서 `submitCallback(callback)`으로 이름을 바꾸고, `submitCallback(ReplyType, callback)`을 추가했습니다. Protobuf codec은 수신 handler가 지정한 message type으로 여러 메시지 종류를 디코딩합니다. (#1503)

## 결함 수정

- Store provider가 실패하면 Spot Send·Request에서 공개 오류 `Unavailable`을 반환합니다. provider 원인은 보존합니다. (#1466)
- Core 1.17.0은 WS·WSS의 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. TCP batching 동작은 유지합니다. (#1545)
- Unity WebGL의 `-O2` optimizer가 제거하던 stream-connector 바인딩을 수정했습니다. (#1539)
- SpotWide Actor queue는 Spot의 공통 execution gate를 통해 소비합니다. (#1496)
- Node sample harness는 SIGTERM을 받은 자식이 제한 시간 안에 종료하지 않으면 SIGKILL을 전송하고 임시 파일을 정리합니다. ZoneWorld 예제는 실제 owner 배치에서 시나리오 대상을 고르고 재시작 알림을 도착 순서대로 처리합니다. (#1490)

## 설치

```bash
npm install @zlink-systems/framework@0.29.0 @zlink-systems/http-client@0.29.0
```

릴리스 태그는 [`framework-node/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.29.0)입니다.
[한국어](./framework-node-0.29.0.ko.md) | [English](./framework-node-0.29.0.md)
