[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 Release Notes

## Compatibility

- Registering a standalone callback without an owner fails with `invalid_operation`. Create a `completion_owner_t` before registering callbacks. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#41-completion-owner))
- The result storage representation of `task_t` changed, so the C++ binary ABI differs. Rebuild the Framework, consumers, plugins, fake/test adapters, and engine bridges with the same header, and do not mix in objects built with an earlier header. Public signatures are unchanged. ([§4 Common result, coroutine and message](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#4-common-result-coroutine-and-message))

- DTOs that use default typed JSON must declare `zlink::json::dto_traits<T>::fields` from `<zlink/json.hpp>`; enums must declare `enum_traits<E>::values`.
- 64-bit integers use range-checked decimal strings, and enums use declared name strings. Previous integer number tokens and enum ordinals are rejected.
- Default typed JSON no longer falls back to nlohmann `to_json` or `from_json` callbacks.
- To migrate, move wire names and public members to `field(name, &T::member)`, and declare enum mappings with `enum_value(value, name)`. Every field is required; use `std::optional` for nullable fields.
