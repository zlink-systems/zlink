[English](./framework-cpp-0.29.0.md) | [한국어](./framework-cpp-0.29.0.ko.md)

# ZLink C++ Framework 0.29.0 릴리스 노트

Framework 0.29.0은 C++ binding 1.17.0과 Core 1.17.0을 사용합니다. (#1554)

## 계약 변경

- Missing Instance Spot의 cold activation은 Core request/reply로 target에 제출합니다. target이 예약과 활성화를 소유합니다. Reserve에서 경쟁에 진 요청은 다른 target으로 전달하거나 winner의 완료를 기다리지 않고 `Unavailable` 한 번으로 끝납니다. type을 정할 serving node가 없으면 `NotFound`, 생략한 type 후보가 여러 개면 `InvalidOperation`, capacity가 부족하면 `Unavailable`을 반환합니다. (#1467)
- Spot·Actor·handler 등록의 생성자 의존성을 optional·collection·이름/qualifier 선택 규칙에 따라 host 시작 때 선언 metadata로 검증합니다. 필수 의존성이 없거나 선택이 모호하면 시작이 실패합니다. 명시적 factory는 실행하지 않습니다. (#1549)
- target을 다시 시작할 때 이전 lifecycle에 속한 Creating 예약을 recovery 단계에서 해제하고 pending capacity를 반환합니다. (#1532)

## 변경

- Stream node에 heartbeat 간격·제한 시간과 idle 종료 시간을 설정하는 `set_heartbeat`와 `set_idle_timeout`을 추가했습니다. heartbeat 기본값은 1초·5초입니다. idle timeout 기본값은 0이며, 이 값에서는 유휴 종료가 비활성화됩니다. heartbeat control frame은 애플리케이션 활동으로 계산하지 않습니다. (#1538, #1541)

## 결함 수정

- Store provider가 실패하면 Spot Send·Request에서 공개 오류 `Unavailable`을 반환합니다. provider 원인은 보존합니다. (#1466)
- Core 1.17.0은 WS·WSS의 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. TCP batching 동작은 유지합니다. (#1545)
- ClientServer worker의 종료와 transport poller 대기를 정리해 종료 중 닫힌 연결을 다시 사용하는 문제를 수정했습니다. 같은 socket에 두던 monitor poller를 없애고 공유 poller가 새 입력을 전달합니다. (#1423)
- maintenance 상태의 Zone으로 Actor를 이동하면 Join이 `ProtocolError` 대신 정상 거부로 끝나고 source membership을 유지합니다. (#1490)
- SpotWide Actor queue는 Spot의 공통 execution gate를 통해 소비합니다. C++ diagnostics file logger가 실패해도 프로세스가 abort하지 않습니다. (#1496, #1466)
- ZoneWorld 예제가 실제 owner 배치에서 시나리오 대상을 고르고, 재시작 알림을 도착 순서대로 처리합니다. (#1490)

## 설치

[`framework-cpp/v0.29.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.0)에서 플랫폼별 Framework archive를 선택하거나 `bootstrap.cmake`로 내려받습니다. 서버 Framework에는 C++ binding 1.17.0과 Core 1.17.0이 필요합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.0)입니다.
[한국어](./framework-cpp-0.29.0.ko.md) | [English](./framework-cpp-0.29.0.md)
