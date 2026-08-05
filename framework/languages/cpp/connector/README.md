# C++ Stream Connector

이 디렉터리는 C++ STREAM client connector 산출물을 나눈다.

| 디렉터리 | 용도 | 배포 |
|----------|------|------|
| `core` | 일반 C++ client용 connector runtime | CMake, vcpkg, Conan |
| `e2e-client` | 서버 e2e, smoke, perf scenario용 coroutine helper | CMake, vcpkg, Conan |
| `engines` | Unreal, Godot, Axmol wrapper | engine source/plugin package |
| `perf` | connector 성능 테스트 client와 runner | test/tool artifact |

core package는 `zlink::stream_connector`를 제공한다. 이 package의 public header는
`<coroutine>`이나 Boost.Asio executor type을 노출하지 않는다. callback completion과
`dispatch()` 의미는 core 계약에 속한다.

core connector는 `connector_t::observe_inbound(...)`를 제공한다. 이 API는 connector를
만든 뒤 `connect()` 또는 async `connect(...)`를 시작하기 전에 등록해야 한다. 반환된
`inbound_observer_registration_t`가 살아 있는 동안 수신 frame마다
`inbound_observation_t` snapshot을 받는다.

```cpp
auto connector = zlink::stream_connector::connector_factory_t::create(options);
auto inbound_log = connector.observe_inbound(
  [] (const zlink::stream_connector::inbound_observation_t &observation) {
      std::cout << "stream-inbound name=" << observation.name
                << " bytes=" << observation.payload_length << '\n';
  });
connector.connect();
```

observation에는 message kind, packet name, codec, request sequence, metadata,
payload byte length, 압축 여부, 수신 시간이 들어간다. observer callback은 receive 경로에서
직접 실행하지 않는다. callback 예외는 `error_code_t::observer_failed`, bounded queue
overflow는 `error_code_t::observer_dropped`로 `on_error(...)`에 보고한다. 두 오류는
관찰 기능의 실패를 알릴 뿐 원래 frame 처리, pending request 완료, packet dispatch를
막지 않는다.

e2e client package는 `zlink::stream_e2e_client`를 제공한다. 이 package를 선택한 경우에만
`task_t`와 `async()` 표면이 보인다. 일반 engine wrapper는 e2e client package에 의존하지 않는다.

성능 테스트는 `connector/perf` 아래의 `connector_perf_client`를 사용한다. smoke gate는 CTest
`connector-perf-smoke` label로 실행한다.
