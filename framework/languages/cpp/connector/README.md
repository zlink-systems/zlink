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

e2e client package는 `zlink::stream_e2e_client`를 제공한다. 이 package를 선택한 경우에만
`task_t`와 `async()` 표면이 보인다. 일반 engine wrapper는 e2e client package에 의존하지 않는다.

성능 테스트는 `connector/perf` 아래의 `connector_perf_client`를 사용한다. smoke gate는 CTest
`connector-perf-smoke` label로 실행한다.
