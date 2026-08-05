# C++ Stream Connector Package Notes

core connector package는 `zlink::stream_connector` target을 제공한다.

- Boost.Asio와 Beast는 implementation dependency다.
- public header는 Boost.Asio executor type과 coroutine type을 노출하지 않는다.
- vcpkg와 Conan package import smoke는 `connector-package` CTest label로 확인한다.
- e2e coroutine helper는 별도 `zlink::stream_e2e_client` package에서 제공한다.
