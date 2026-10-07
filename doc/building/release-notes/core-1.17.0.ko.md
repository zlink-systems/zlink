[English](./core-1.17.0.md) | [한국어](./core-1.17.0.ko.md)

# libzlink 1.17.0 릴리스 노트

## 변경

- `ws`·`wss`에서는 STREAM 메시지 하나를 WebSocket 메시지 하나로 전송합니다. 이전에는 Core가 STREAM frame 여러 개를 WebSocket 메시지 하나에 넣어 보냈고, 수신 측은 `frame length does not match prefix` 오류로 연결을 종료했습니다 (#1545, #1553).
- `unittest_recv_admission.cpp`의 지역 buffer 이름을 `small`에서 `short_buffer`로 바꿔 Windows SDK 매크로 충돌로 발생하던 테스트 빌드 실패를 고쳤습니다 (#1553).
