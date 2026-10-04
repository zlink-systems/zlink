[English](./core-1.14.0.md) | [한국어](./core-1.14.0.ko.md)

# libzlink 1.14.0 릴리스 노트

## 변경

- 수신 DATA를 처리할 때 종료 중인 pipe의 `ETERM`을 연결 오류로 분류합니다 (#1434).
- 진행 중인 비동기 쓰기가 완료된 뒤 terminal ERROR frame을 보냅니다. 쓰기가 실패하거나 취소되면 추가 쓰기 없이 연결을 닫아 WS/WSS에서 해제된 write buffer에 접근하던 결함을 수정했습니다 (#1434).
- terminal ERROR frame을 보류하는 동안에도 handshake timeout이 발생하면 연결을 닫습니다 (#1434).
- session에 오류를 알리기 전에 engine을 분리하고 session 종료 뒤 바로 반환하여 파괴된 session에 접근하던 결함을 수정했습니다 (#1434).
