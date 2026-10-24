[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 릴리스 노트

## 호환성

- Owner 없이 standalone callback을 등록하면 `invalid_operation`으로 실패합니다. `completion_owner_t`를 생성해 사용하십시오. ([§4.1 Completion owner](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md#41-completion-owner))
- `route_send_call_t`·`spot_send_call_t`·`spot_request_call_t`의 생성자가 private가 되어 호출 객체는 `route_client_t`만 생성하며, `route_send_call_t::async_submit_fn_t`가 제거되었습니다. ([§4 Messaging API](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md#4-messaging-api))
