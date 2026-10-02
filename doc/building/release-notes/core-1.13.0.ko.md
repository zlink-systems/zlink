[English](./core-1.13.0.md) | [한국어](./core-1.13.0.ko.md)

# libzlink 1.13.0 릴리스 노트

## 변경

- 공개 data 수신은 handle의 receive bit로 진입을 판정하며, 호출 하나가 record 하나를 소유합니다 (#1292).
- 긴 구독 topic을 처리하는 trie의 재귀 순회를 없애 Windows stack 고갈을 수정했습니다 (#1334).
- engine을 직접 종료하는 경로에서도 물리 연결 종료에 대한 `DISCONNECTED` monitor event를 발행합니다 (#1334).
- 종료 중인 ROUTER lane에서 READY 처리 때문에 protocol error가 발생하던 결함을 수정했습니다 (#1334).
- TSan 계측과 ROUTER pair의 정상 종료 분류를 수정했습니다 (#1293).

## 유지하는 계약

다음 항목은 1.12.0에 포함된 동작을 유지하며, 1.13.0에서 새로 추가한 변경이 아닙니다.

- STREAM socket은 bind 전용이며 accept한 peer는 `zlink_disconnect_rid`로 종료합니다 (#1192).
- 대기 토큰은 `SNDTIMEO`에 만료되며, reservation 상한 초과는 `BACKPRESSURED`입니다. Binding의 WRITABLE completion은 timeout 결과를 `BACKPRESSURED`와 `EAGAIN`으로 보고합니다 (#1168).
- PUB/XPUB는 publish record 시작에서 HWM을 한 번 판정하고 승인한 record는 끝까지 씁니다 (#1183).
- socket close 경합, READY ID와 monitor 순서에 대한 수정은 유지됩니다 (#1214).
- XSUB 수신 경로는 socket turn으로 subscription trie를 보호하며 별도 mutex를 사용하지 않습니다 (#1202).
