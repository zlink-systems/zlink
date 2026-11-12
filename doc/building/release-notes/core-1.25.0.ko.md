[English](./core-1.25.0.md) | [한국어](./core-1.25.0.ko.md)

# libzlink 1.25.0 릴리스 노트

## 변경

- 비동기 명령 owner가 idle 상태에서 분리되기 전에 mailbox를 다시 확인합니다. 경계 시점에 도착한 명령이 남지 않으며, 첫 inproc send가 `EAGAIN`을 반환하던 문제도 해결합니다(#4186, #4234).
- binding의 close 과정에서 `SHUTDOWN`을 처리하고 submit과 close의 순서를 보장합니다(#4060; Java #4193, .NET #4196, Node.js #4195).

## 호환

공개 API·ABI 변경은 없으며 linker SOVERSION 0을 유지합니다. Core와 일곱 binding 버전은 1.25.0입니다. Framework 자체 버전은 0.32.0을 유지합니다. 구성 요소의 호환 관계는 [버전 정책](../versioning.ko.md)을 따릅니다.