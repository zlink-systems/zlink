[English](./core-1.21.0.md) | [한국어](./core-1.21.0.ko.md)

# libzlink 1.21.0 릴리스 노트

## 변경

- connect 등록별 연결 시도 snapshot과 `POLLCONNECT`를 추가했습니다. 연결 상태·실패 원인과 단계, opaque 식별자를 조회하며 연결이 회복돼도 마지막 실패 이력을 보존합니다(#1710, #2692).
- Pair 종료와 handover 전에 큐에 도착한 reply를 먼저 처리해 해당 request를 종료 오류로 조기에 완료하던 결함을 수정했습니다(#2981, #3021).
- 숫자 IP bind의 불필요한 NIC 조회와 inproc unbind의 peer 종료 ACK 대기를 제거했습니다(#3035, #3064, #3072).
- Asio listener 종료 시 남은 accept 완료를 처리하고, monitor READY의 정본을 logical-ready count로 맞췄습니다(#2658, #2967, #2396, #2876).
- Context 종료 중 대기 inproc connect와 예약 reconnect의 control runtime 수명을 바로잡았습니다(#2899, #2901).
- Windows signaler의 지원되지 않는 AF_UNIX datagram 시도를 제거했습니다. 기존 stream pair와 조건부 TCP fallback은 유지합니다(#3053, #3158).

## 호환

공개 C ABI의 기존 symbol과 linker SOVERSION 0을 유지하며 새 API를 추가합니다. Binding은 [버전 정책](../versioning.ko.md)에 따라 Core 1.21에 맞는 1.21 계열을 사용합니다. Framework는 별도 버전과 정확한 binding pin을 사용합니다.
