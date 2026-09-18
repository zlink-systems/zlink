[English](./framework-cpp-0.18.0.md) | [한국어](./framework-cpp-0.18.0.ko.md)

# ZLink C++ Framework 0.18.0 릴리스 노트

Framework 0.18.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

Stream connector의 공개 표면이 바뀝니다. 다섯 언어의 기능 차이를 없애면서 C++에서 가장 많이 바뀎습니다.

- `connector_options_t::transport`가 `std::optional<transport_t>`가 됩니다. 기본값이 사라지고, 지정하지 않으면 endpoint scheme이 정합니다. 명시한 값이 scheme과 어긋나면 `ConfigurationError`입니다.
- 등록 네 종(`on`·`on_error`·`on_disconnected`·`on_connection_state_changed`)이 `connector_t&` 대신 `[[nodiscard]] subscription_t`를 돌려줍니다. **반환값을 버리면 등록이 즉시 풀립니다.** 컴파일 오류가 아니라 경고이므로, 체이닝 호출을 개별 호출로 바꾸고 반환값을 보관해야 합니다.
- `result_t<T>::error_code()`가 `std::optional<error_code_t>`가 됩니다. `static_cast<int>`는 더 이상 컴파일되지 않습니다.
- `error_code_t`에서 `unsupported_codec`·`closed`·`canceled`가 빠지고 `validation_failed`·`disconnected`로 옵겨갑니다.
- `send_call_t::codec()`과 `request_call_t::codec()`이 없어집니다. codec과 resolver는 생성 옵션으로 주입합니다.
- `on<T>` callback이 `const message_t<T>&`를 받습니다.
- `wait_for<T>().submit()`이 `result_t<message_t<T>>`를, `wait_for_sequence<T>().submit()`이 `result_t<std::vector<message_t<T>>>`를 돌려줍니다. 대기 표면의 timeout은 `request_timeout`이 아니라 `validation_failed`입니다.
- **기본 packet 이름이 `typeid(T).name()`에서 타입의 단순 이름으로 바뀝니다.** 컴파일러마다 달라지는 이름을 쓰지 않기 위해서입니다. 서버와 client가 합의하는 wire 이름이 달라지므로, 이름을 명시하지 않던 코드는 양쪽을 함께 올려야 합니다.
- `on_disconnected` handler가 `std::optional<close_reason_t>`를 받습니다.
- `codecs::on<T>(connector, ...)` framework helper도 `subscription_t`를 돌려줍니다.

## 공통 변경

- Runtime descriptor 변경의 게시 시점을 스펙에 적었습니다. 조율은 요청하는 쪽이 하고, 받는 쪽은 요청에 제약을 두지 않습니다. (#546)
- 원격 생성 예약 record의 `requestContentReference` 문법에서 checksum 구간을 없앰습니다. 예약은 별도의 record가 아니라 descriptor record의 상태입니다. (#559)
- 샘플은 한 번에 하나씩 실행합니다. 언어별 집계 러너를 없애고, 실행 방법을 공통 sample 문서가 소유하도록 했습니다. (#585)
- 언어별 e2e 시나리오 스위트를 걷어냈습니다. 크로스 언어 e2e는 유지합니다. (#541)

## 수정

- 예약 record의 authority payload 자리에 응용 요청 바이트를 넣어 다른 언어로의 원격 Actor 생성이 막히던 것을 고쳤습니다. (#549)
- 계약 시험이 파일을 읽을 때 줄바꿈을 각 플랫폼의 CRT에 맡겨, CRLF 체크아웃과 LF 체크아웃에서 결과가 달랐습니다. 그중 둘은 음성 단언이라 검사를 멈춘 채 통과하고 있었습니다. 읽는 자리 한 곳에서 정규화하도록 바꿨습니다. (#581)

## 설치

[`framework-cpp/v0.18.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.0)에서 `zlink-framework-cpp-0.18.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
