[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 Release Notes

## Compatibility

- Registering a standalone callback without an owner fails with `invalid_operation`. Create a `completion_owner_t` before registering callbacks. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#41-completion-owner))
- The result storage representation of `task_t` changed, so the C++ binary ABI differs. Rebuild the Framework, consumers, plugins, fake/test adapters, and engine bridges with the same header, and do not mix in objects built with an earlier header. Public signatures are unchanged. ([§4 Common result, coroutine and message](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#4-common-result-coroutine-and-message))
