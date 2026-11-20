[English](./bindings-go-1.26.0.md) | [한국어](./bindings-go-1.26.0.ko.md)

# ZLink Go binding 1.26.0 릴리스 노트

Core 1.26.0을 사용합니다.

## 변경

- binding 단독 변경은 없으며 Core 1.26.0을 따릅니다.

## 포함된 Core 1.26.0 변경

- `zlink_disconnect(endpoint)`는 connect 등록과 자동 재연결 intent만, `zlink_unbind(endpoint)`는 bind 등록과 그것으로 받은 연결만 끝냅니다. disconnect가 같은 주소에 bind된 listener를 더 이상 닫지 않습니다(#4846, #4864).
- connecter가 자기 session의 I/O thread를 씁니다(#3541, #4303).
