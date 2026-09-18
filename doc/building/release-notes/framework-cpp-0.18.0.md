[English](./framework-cpp-0.18.0.md) | [한국어](./framework-cpp-0.18.0.ko.md)

# ZLink C++ Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public surface changes. Removing the capability differences across the five languages changed C++ the most.

- `connector_options_t::transport` is now `std::optional<transport_t>`. The default is gone; when it is not set, the endpoint scheme decides. A value that contradicts the scheme is a `ConfigurationError`.
- The four registration functions (`on`, `on_error`, `on_disconnected`, `on_connection_state_changed`) return `[[nodiscard]] subscription_t` instead of `connector_t&`. **Discarding the return value unregisters immediately.** That is a warning, not a compile error, so chained calls must become individual calls whose results you keep.
- `result_t<T>::error_code()` is now `std::optional<error_code_t>`; `static_cast<int>` no longer compiles.
- `error_code_t` drops `unsupported_codec`, `closed`, and `canceled`; they move to `validation_failed` and `disconnected`.
- `send_call_t::codec()` and `request_call_t::codec()` are gone. Codecs and resolvers are injected through the construction options.
- The `on<T>` callback takes `const message_t<T>&`.
- `wait_for<T>().submit()` returns `result_t<message_t<T>>` and `wait_for_sequence<T>().submit()` returns `result_t<std::vector<message_t<T>>>`. A timeout on the wait surface is `validation_failed`, not `request_timeout`.
- **The default packet name changes from `typeid(T).name()` to the type's simple name**, so the name no longer varies by compiler. The wire name the server and client agree on therefore changes: code that did not spell the name out must be upgraded on both sides together.
- The `on_disconnected` handler receives `std::optional<close_reason_t>`.
- The `codecs::on<T>(connector, ...)` framework helper also returns a `subscription_t`.

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- Fixed the reservation record carrying application request bytes in the authority payload slot, which blocked remote Actor creation into other languages. (#549)
- Contract tests read files leaving newline handling to each platform's CRT, so the same assertion behaved differently on a CRLF checkout and an LF checkout. Two of them were negative assertions that passed while checking nothing. Normalization now happens in one place. (#581)

## Installation

Download `zlink-framework-cpp-0.18.0.tar.gz` from the [`framework-cpp/v0.18.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. A vcpkg overlay port and a Conan recipe are also available.
