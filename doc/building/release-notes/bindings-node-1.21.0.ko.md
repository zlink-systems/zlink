[English](./bindings-node-1.21.0.md) | [한국어](./bindings-node-1.21.0.ko.md)

# ZLink Node.js binding 1.21.0 릴리스 노트

Core 1.21.0을 따릅니다.

## 변경

- 연결 시도 snapshot과 PollConnect를 제공하고 호환되지 않는 Core를 거부합니다(#1710, PR #3161).
- Buffer part 송신 시 `nativeMessage` 속성 때문에 다른 Message가 소비될 수 있던 결함을 수정하고 수신 저장소 할당을 줄였습니다(#1675, PR #2603).

## Core 1.21.0 포함

- 연결별 연결 시도 snapshot과 POLLCONNECT 준비 상태를 추가했습니다. snapshot은 불투명 토큰과 유형화된 실패 원인을 제공하며 복구 후에도 마지막 실패를 유지합니다(#1710, #2692).
- 연결 handover를 포함한 pair 종료 전에 대기열에 들어온 응답을 처리한 뒤 미완료 요청을 종료합니다(#2981, #3021).
- 숫자 bind 주소를 네트워크 인터페이스 조회 없이 해석합니다(#3035, #3064).
- 상대의 종료 ACK를 기다리지 않고 inproc endpoint를 해제합니다(#3035, #3072).
- listener 종료 전에 미완료 Asio accept completion을 마칩니다(#2658, #2967).
- monitor READY를 논리적 ready 개수에 따라 보고합니다(#2396, #2876).
- context 종료 시 대기 중인 inproc 연결을 완료하고 예약된 reconnect에 활성 control runtime을 사용합니다(#2899, #2901).
- Windows에서 지원하지 않는 AF_UNIX datagram signaler 시도를 제거하고 stream pair와 조건부 TCP fallback을 유지합니다(#3053, #3158).
- inproc reconnect 내부 작업을 socket close 전에 join해, 사용자 호출이 없는데도 `zlink_close`가 `EBUSY`로 실패하던 결함을 수정했습니다(#3197, #3199).
