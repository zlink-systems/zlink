[English](./core-1.11.0.md) | [한국어](./core-1.11.0.ko.md)

# libzlink 1.11.0 릴리스 노트

## 변경

- WRITABLE completion의 send_result 값은 ADMITTED 0, NOT_FOUND 801 (ENOENT), NOT_CONNECTED 802 (ENOTCONN, STREAM), TIMED_OUT 803 (EAGAIN)입니다 (#1154).
- DONTWAIT SEND·REQUEST가 반환한 대기 토큰은 submit 시점에 snapshot한 SNDTIMEO 기한까지 완료되지 않으면 TIMED_OUT·EAGAIN으로 끝납니다. SNDTIMEO=-1은 기한이 없습니다 (#1154).
- socket의 completion reservation 65,536개가 모두 사용 중이면 SEND DONTWAIT도 REQUEST와 같이 BACKPRESSURED·EAGAIN, ID 0을 반환합니다. 이전에는 OUT_OF_MEMORY·ENOMEM을 반환했습니다 (#1165).
- 한 thread가 socket을 poll하고 다른 thread가 send·recv할 때, send·recv 경로의 command 처리가 poller 알림을 소비해 poller가 다음 사건까지 잠들던 결함을 수정했습니다. poller 알림은 poller만 소비합니다 (#1082).
- 바로 수락된 send는 SNDTIMEO 조회와 시계 읽기를 하지 않습니다 (#1154).
