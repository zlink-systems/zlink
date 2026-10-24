[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 Release Notes

## Compatibility

- Registering a standalone callback without an owner fails with `invalid_operation`. Create a `completion_owner_t` before registering callbacks. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md#41-completion-owner))
- The constructors of `route_send_call_t`, `spot_send_call_t`, and `spot_request_call_t` are private, so only `route_client_t` creates call objects; `route_send_call_t::async_submit_fn_t` has been removed. ([§4 Messaging API](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md#4-messaging-api))
