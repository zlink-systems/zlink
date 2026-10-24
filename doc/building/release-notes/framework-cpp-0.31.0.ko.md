[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 릴리스 노트

## 호환성

- Owner 없이 standalone callback을 등록하면 `invalid_operation`으로 실패합니다. `completion_owner_t`를 생성해 사용하십시오. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md#41-completion-owner))
- `task_t`의 결과 저장 표현이 바뀌어 C++ 바이너리 ABI가 달라졌습니다. Framework·consumer·plugin·fake/test adapter·engine bridge를 같은 header로 모두 다시 빌드하고, 이전 header로 만든 객체와 섞지 마십시오. 공개 signature는 그대로입니다. ([§4 Common result, coroutine과 message](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md#4-common-result-coroutine과-message))
