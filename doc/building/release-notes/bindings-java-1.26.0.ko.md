[English](./bindings-java-1.26.0.md) | [한국어](./bindings-java-1.26.0.ko.md)

# ZLink Java binding 1.26.0 릴리스 노트

Core 1.26.0을 사용합니다.

## 변경

- borrowed routing id를 복사하는 수신이 기존 native-call reservation을 잡아 close와 겹치지 않습니다(#4568, PR #4597).
- message copy가 message slot pool의 header를 재사용합니다(#4570, PR #4588).

## 포함된 Core 1.26.0 변경

- `zlink_disconnect(endpoint)`는 connect 등록과 자동 재연결 intent만, `zlink_unbind(endpoint)`는 bind 등록과 그것으로 받은 연결만 끝냅니다. disconnect가 같은 주소에 bind된 listener를 더 이상 닫지 않습니다(#4846, #4864).
- connecter가 자기 session의 I/O thread를 씁니다(#3541, #4303).
