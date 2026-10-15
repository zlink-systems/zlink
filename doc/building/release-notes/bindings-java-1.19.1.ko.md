[English](./bindings-java-1.19.1.md) | [한국어](./bindings-java-1.19.1.ko.md)

# ZLink Java binding 1.19.1 릴리스 노트

Core 1.19.0을 따릅니다.

## 변경

- virtual thread의 native 메모리 누수를 수정했습니다. virtual thread에서 닫은 `Message`의 native slot이 그 thread의 thread-local pool에 남아, thread가 끝난 뒤 재사용도 해제도 되지 않았습니다(RouteMesh request 하나당 shared arena 4개). 이제 virtual thread는 thread-local slot·wrapper pool을 쓰지 않고 slot을 크기 제한이 있는 공유 pool로 돌려주며, 공유 pool이 가득 차면 slot을 닫습니다. 공개 API는 바뀌지 않습니다(#1808, PR #1811).
