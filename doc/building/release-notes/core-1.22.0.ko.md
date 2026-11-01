[English](./core-1.22.0.md) | [한국어](./core-1.22.0.ko.md)

# libzlink 1.22.0 릴리스 노트

## 변경

- 내부 socket 진행을 공개 API 실행 수락 판정에서 제외했습니다. Monitor를 해제한 직후 socket을 닫으면 `EBUSY`가 반환되고 `zlink_ctx_term`이 끝나지 않던 결함을 수정했습니다(#3444, #3448).
- Socket monitor 전달을 유휴 상태의 10ms 주기 폴링에서 이벤트 enqueue 시 명시적 예약으로 바꿨습니다(#2277, #3245).

## 호환

공개 API·ABI 변경은 없으며 linker SOVERSION 0을 유지합니다. 일곱 binding 버전과 Framework 네 언어의 정확한 Core·binding pin은 1.22.0으로 맞춥니다. Framework 자체 버전은 0.31.0을 유지합니다. 구성 요소의 호환 관계는 [버전 정책](../versioning.ko.md)을 따릅니다.

## Binding 변경

`core/v1.21.0..origin/main`의 binding 코드 변경은 없습니다. 이 범위의 binding 변경은 C++ Conan 레시피에서 1.21.0 릴리스 자산의 체크섬을 갱신한 #3220뿐입니다. 1.22.0 자산의 vcpkg·Conan 체크섬 갱신은 태그와 자산 생성 뒤 별도 PR에서 진행합니다.
