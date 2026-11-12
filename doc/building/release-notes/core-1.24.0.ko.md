[English](./core-1.24.0.md) | [한국어](./core-1.24.0.ko.md)

# libzlink 1.24.0 릴리스 노트

## 변경

- message의 마지막 content 참조를 한 곳에서 해제합니다. 중첩 view의 pool zero-copy slice metadata가 누수되지 않고 pool로 돌아갑니다(#3918, #3930).
- C++에서 send·receive flags enum의 기반 타입을 `unsigned int`로 고정합니다. 알 수 없는 bit가 정의되지 않은 동작이 되지 않고 기존 `EINVAL` 검사에 도달합니다. C 선언, 이름과 시그니처는 바뀌지 않습니다(#3918, #3930).
- 비어 있는 queue에서는 `MORE` 앞부분이 작은 HWM을 넘는 완전한 multipart record도 받습니다. 이전에는 수신 측이 다시 읽기 시작한 뒤에도 이 record가 계속 backpressure 상태로 남았습니다(#4118, #4135).

## 호환

공개 API·ABI 변경은 없으며 linker SOVERSION 0을 유지합니다. 일곱 binding 버전을 1.24.0으로 맞춥니다. Framework 자체 버전은 0.32.0을 유지합니다. 구성 요소의 호환 관계는 [버전 정책](../versioning.ko.md)을 따릅니다.
