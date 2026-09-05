# C++ m6b 미승인 요청 fixture 수정 결과

## 결과

`verify_unadmitted_request_is_rejected_without_framework_queue()`가 native 응답을 기다리기 전에
같은 DEALER의 FIFO 선두에 있는 한 part의 `hello` 메시지를 받는다. service-wire로 받은 헤더의
command가 `hello`인지 확인하지만 source를 Framework에 admit하지는 않는다. request에 설정한
5초 deadline, `protocol_error`, application domain의 mailbox가 비어 있다는 조건, native 응답의
correlation과 terminal, failure code를 검사하는 assertion은 그대로 유지된다.

## 변경 diff

```diff
@@
     assert (target.mailbox ().pending_messages (
               mesh::service_mailbox_domain_t::application) == 0);
 
+    zlink::received_t hello;
+    assert (source.recv (hello) == 0);
+    assert (hello.parts ().size () == 1);
+    assert (protocol::decode_header (hello.parts ().front ().to_bytes ()).kind
+            == protocol::command::hello);
+
     auto rejected_reply =
       await_native_reply (std::move (request)).result ().value ();
```

변경 파일은
`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:5240`이다.
Framework의 runtime, Core, binding과 다른 언어의 소스는 변경하지 않았다.

## 계층 판정

- 소유 계층: Core는 같은 socket에서 DATA와 request 응답을 처리하는 FIFO 순서와 request 완료 처리를 소유한다. Framework는 hello와 peer를 논리적으로 승인하는 판단(admission)을 소유한다. FIFO의 DATA를 받는 동작은 raw DEALER를 사용하는 fixture가 담당한다.
- Spec 조항: Core socket README §4 RID 중복 정책, `06-dealer` §2:39–51, `07-router` §9:287–322, `05-polling` §4:86–102, `06-monitoring` §3.1–3.2와 Framework wire protocol §4:315–320을 따른다.
- 교차언어 대조: .NET·Java·Node는 Core가 알린 timeout을 각 언어의 요청 시간 초과 결과로 전달하며, 미승인 peer의 ingress에서는 응답 없이 거부한다. C++에만 raw DEALER로 거부 payload를 확인하는 fixture가 있으므로 먼저 hello DATA를 받아 FIFO를 진행해야 한다.
- 변경 분류: **B — 기존 C++ fixture에서 DATA를 받지 않아 진행이 멈춘 결함의 수정.** Runtime 변경이나 계약 적응은 아니다.

## 검증

환경은 `TMPDIR=/dev/shm/zlink-tmp-cpp`, 빌드 디렉터리는
`framework/languages/cpp/build/linux-ninja-c-e2e`를 사용했다.

- `cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j4 --target test_cpp_framework_m6b_runtime`: 컴파일과 링크 통과
- `cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j4`: 통과(`ninja: no work to do.`)
- `test_cpp_framework_m6b_runtime` 직접 실행 3회: 3/3 통과
- `ctest -R 'm6b' --output-on-failure`: 1/1 통과, 0 실패, 6.12초

## Binding 오류 문구 후속 사항

`request_result_t` 값 101이 `"Network is unreachable (errno=110)"`으로 표시되는 문제는 이번 범위에서
수정하지 않았다. 문자열은
`bindings/cpp/include/zlink/Contracts/Errors/errors.hpp:45-51`의
`binding_error_t::build_message()`가 `error_text(code_)`와 `_internal_errno`를 조합한다.
`bindings/cpp/src/Runtime/Core/capability.cpp:15-17`의 `error_text()`는 `request_result_t` 값 101을
그대로 `zlink_strerror(101)`에 전달한다. Binding 소유자는 `request_result_t` 값과 오류 문구의
mapping을 별도 B 결함으로 수정해야 한다.
