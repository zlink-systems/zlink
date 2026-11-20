[English](./core-1.26.0.md) | [한국어](./core-1.26.0.ko.md)

# libzlink 1.26.0 릴리스 노트

## 변경

- `zlink_disconnect(endpoint)`는 그 endpoint의 connect 등록과 자동 재연결 intent만, `zlink_unbind(endpoint)`는 bind 등록과 그것으로 받은 연결만 끝냅니다. 이전에는 둘 다 endpoint의 모든 등록을 닫아, disconnect가 같은 주소에 bind된 listener와 inbound 연결까지 끊을 수 있었습니다. bind만 있는 endpoint의 `zlink_disconnect`는 이제 `ZLINK_CONNECT_NOT_FOUND`를 반환합니다(#4846, #4864).
- connecter가 자기 session의 I/O thread를 씁니다(#3541, #4303).
- C++·Java binding: borrowed routing id를 복사하는 수신이 기존 call reservation을 잡아 close와 겹치지 않습니다(#4568; C++ #4599, Java #4597). Java message copy가 message slot pool의 header를 재사용합니다(#4570, #4588).

## 호환

공개 API·ABI 변경은 없으며 linker SOVERSION 0을 유지합니다. disconnect 변경은 기존 socket spec을 따르는 수정이며, 자기 bind endpoint에 `zlink_disconnect`를 불러 listener를 닫던 코드는 `zlink_unbind`를 불러야 합니다. Core와 일곱 binding 버전은 1.26.0입니다. 구성 요소의 호환 관계는 [버전 정책](../versioning.ko.md)을 따릅니다.
