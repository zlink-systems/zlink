[English](./core-1.23.0.md) | [한국어](./core-1.23.0.ko.md)

# libzlink 1.23.0 릴리스 노트

## 변경

- poller의 마지막 drain 전에 scheduler를 재시작해 `stop()` 뒤 게시된 handler를 실행하고 지연된 engine 삭제도 완료합니다(#3453, #3513).
- monitor 비동기 command 처리를 해제할 때 idle 차례를 기다리지 않고 남은 command를 inline owner에게 넘깁니다(#3495).

## 호환

공개 API·ABI 변경은 없으며 linker SOVERSION 0을 유지합니다. 일곱 binding 버전을 1.23.0으로 맞춥니다. Framework 자체 버전은 0.31.0을 유지합니다. 구성 요소의 호환 관계는 [버전 정책](../versioning.ko.md)을 따릅니다.
