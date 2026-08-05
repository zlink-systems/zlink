<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [이전: Runtime Architecture](../../common/internals/README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:end -->

[C++ 묶음](../README.ko.md) | [공개 인터페이스](../../common/spec/server/languages/cpp/interfaces/README.ko.md)

# ZLink Framework C++ Backend Dependency Policy

## 1. 목적

framework public API가 zlink binding의 socket 객체와 native 구현 세부에 결합되지
않도록 backend 경계를 정의한다.

## 2. 원칙

- framework는 binding의 public API만 호출한다.
- backend context, socket, SpotNode, Spot과 monitor handle은 private runtime에 둔다.
- public header에는 Boost.Asio, Boost.Beast, OpenSSL과 binding concrete socket 타입을
  노출하지 않는다.
- `RoutingId`, message와 명시적으로 승인된 전송 값 타입만 공개 계약에 남긴다.
- serializer, location codec과 wire framing 결정은 각각 한 runtime subsystem이
  소유한다.

## 3. adapter 책임

backend adapter는 context 생성, socket bind/connect, frame submit/receive, Spot과
stream operation, monitoring event 변환을 담당한다. registration, handler dispatch,
timeout과 application lifecycle은 framework runtime이 담당한다.

sample이나 application이 backend 부족 기능을 raw frame, private header 또는 test
adapter로 우회하지 않는다. 필요한 binding public API가 없으면 먼저 binding 계약으로
설계하고 구현한다.

## 4. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `test_cpp_framework_layout_contract` | public header에 private runtime과 backend concrete dependency가 노출되지 않는다. |
| `test_cpp_framework_contract_headers` | public header가 private include path 없이 compile된다. |
| `test_cpp_framework_native_backend` | backend adapter가 public binding operation으로 channel, Spot과 stream을 연결한다. |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../README.ko.md) | [이전: Runtime Architecture](../../common/internals/README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:bottom:end -->
