[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 Release Notes

## Compatibility

- Registering a standalone callback without an owner fails with `invalid_operation`. Create a `completion_owner_t` before registering callbacks. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#41-completion-owner))
- The constructors of `route_send_call_t`, `spot_send_call_t`, and `spot_request_call_t` are private, so only `route_client_t` creates call objects; `route_send_call_t::async_submit_fn_t` has been removed. ([§4 Messaging API](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md#4-messaging-api))
- The result storage representation of `task_t` changed, so the C++ binary ABI differs. Rebuild the Framework, consumers, plugins, fake/test adapters, and engine bridges with the same header, and do not mix in objects built with an earlier header. Public signatures are unchanged. ([§4 Common result, coroutine and message](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#4-common-result-coroutine-and-message))

- DTOs that use default typed JSON must declare `zlink::json::dto_traits<T>::fields` from `<zlink/json.hpp>`; enums must declare `enum_traits<E>::values`.
- 64-bit integers use range-checked decimal strings, and enums use declared name strings. Previous integer number tokens and enum ordinals are rejected.
- Default typed JSON no longer falls back to nlohmann `to_json` or `from_json` callbacks.
- To migrate, move wire names and public members to `field(name, &T::member)`, and declare enum mappings with `enum_value(value, name)`. Every field is required; use `std::optional` for nullable fields.
- Removed the raw `stream_t::write_packet(const zlink::message_t&)` and `reply_packet(const zlink::message_t&)` overloads. Pass typed payloads with `zlink::framework::message_t::from(value)`.
- Changed the `packet_stream_session_t::on_packet` payload to `const zlink::framework::message_t&`. Read the received encoded value with `payload.decode<T>()` or forward it unchanged.
- Changed `session_actor_t::relay` and `relay_request` inputs to `const zlink::framework::message_t&`. The `relay_request_call_t` result is also an encoded `zlink::framework::message_t`.
