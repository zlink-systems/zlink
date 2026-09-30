[English](./core-1.12.0.md) | [한국어](./core-1.12.0.ko.md)

# libzlink 1.12.0 릴리스 노트

## 변경

- STREAM socket은 bind 전용입니다. `zlink_connect`는 `ZLINK_CONNECT_NOT_SUPPORTED` (602)와 `ENOTSUP`으로 거절됩니다. accept한 peer는 `zlink_disconnect_rid`로 연결을 끊으며, 존재하지 않는 RID는 `ZLINK_CONNECT_NOT_FOUND` (605)입니다 (#1164).
- NODROP PUB/XPUB가 topic frame을 쓴 뒤 payload에서 HWM에 도달해 record가 중간에 끊기던 결함을 수정했습니다. record 시작에서 filter가 일치하는 pipe의 HWM을 한 번 판정하고, 승인한 record는 끝까지 씁니다 (#1157).
